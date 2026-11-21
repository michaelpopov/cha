#include "providers/text_reply_cost.h"

#include "characters/character_config.h"
#include "providers/gemini_token_cost.h"
#include "providers/model_backend.h"
#include "providers/openai_token_cost.h"
#include "util/text.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <string_view>

namespace cha {
namespace {

using Json = nlohmann::json;

std::string_view host_name(std::string_view host) {
    if (host.ends_with('.')) host.remove_suffix(1);
    return host;
}

std::optional<double> nonnegative_number(const Json& object, std::string_view name) {
    const auto field = object.find(name);
    if (field == object.end() || !field->is_number()) return std::nullopt;
    const double value = field->get<double>();
    if (!std::isfinite(value) || value < 0) return std::nullopt;
    return value;
}

std::optional<double> openrouter_cost(const ModelBackendConfig& config, const Json& usage) {
    // https://openrouter.ai/docs/cookbook/administration/usage-accounting
    const auto details = usage.find("cost_details");
    if (details != usage.end()) {
        if (auto cost = nonnegative_number(*details, "upstream_inference_cost")) return cost;
        const bool responses = config.api == ProviderApi::responses;
        const auto input = nonnegative_number(*details,
            responses ? "upstream_inference_input_cost" : "upstream_inference_prompt_cost");
        const auto output = nonnegative_number(*details,
            responses ? "upstream_inference_output_cost" : "upstream_inference_completions_cost");
        if (input && output && std::isfinite(*input + *output)) return *input + *output;
    }
    // The account charge can include hosted tool fees or just a BYOK fee.
    const auto tools = usage.find("server_tool_use");
    if (tools != usage.end() && !tools->is_null()) {
        if (!tools->is_object() || std::ranges::any_of(*tools, [](const Json& count) {
                return !count.is_number_integer() || count != 0;
            })) return std::nullopt;
    } else if (config.web_search != WebSearchMode::off || config.model.ends_with(":online")) {
        return std::nullopt;
    }
    const auto byok = usage.find("is_byok");
    if (byok != usage.end() && *byok != false) return std::nullopt;
    return nonnegative_number(usage, "cost");
}

struct TokenRates {
    std::string_view model;
    double input;
    double cached_input;
    double output;
};

std::optional<double> anthropic_cost(std::string_view model, const GenerationResult& round) {
    // https://platform.claude.com/docs/en/about-claude/pricing
    constexpr TokenRates prices[]{
        {"claude-fable-5-2", 8, 0.20, 40},
        {"claude-mythos-5-2", 8, 0.20, 40},
        {"claude-fable-5-1", 10, 0.25, 50},
        {"claude-mythos-5-1", 10, 0.25, 50},
        {"claude-fable-5", 10, 1, 50},
        {"claude-mythos-5", 10, 1, 50},
        {"claude-opus-5-5", 4, 0.20, 20},
        {"claude-sonnet-5-5", 2, 0.10, 10},
        {"claude-haiku-5-5", 0.10, 0.01, 0.50},
        {"claude-opus-5", 5, 0.50, 25},
        {"claude-sonnet-5", 2, 0.20, 10},
        {"claude-opus-4-8", 5, 0.50, 25},
        {"claude-opus-4-7", 5, 0.50, 25},
        {"claude-opus-4-6", 5, 0.50, 25},
        {"claude-opus-4-5", 5, 0.50, 25},
        {"claude-sonnet-4-6", 3, 0.30, 15},
        {"claude-sonnet-4-5", 3, 0.30, 15},
        {"claude-haiku-4-5", 1, 0.10, 5},
    };
    // Anthropic snapshots use a YYYYMMDD suffix.
    if (model.size() > 9 && model[model.size() - 9] == '-'
        && model.substr(model.size() - 8).find_first_not_of("0123456789") == std::string_view::npos)
        model.remove_suffix(9);
    const auto rate = std::ranges::find(prices, model, &TokenRates::model);
    if (rate == std::end(prices)) return std::nullopt;

    const auto& usage = round.usage;
    const auto input = *usage.input_tokens;
    const auto reads = usage.cache_read_tokens.value_or(0);
    const auto writes = usage.cache_write_tokens.value_or(0);
    const auto hour_writes = round.cache_write_1h_tokens.value_or(0);
    if (reads > input || writes > input - reads || hour_writes > writes) return std::nullopt;

    const double multiplier = model == "claude-haiku-5-5" && input > 100'000 ? 5.0 : 1.0;
    return (static_cast<double>(input - reads - writes) * rate->input
        + static_cast<double>(reads) * rate->cached_input
        + static_cast<double>(writes - hour_writes) * rate->input * 1.25
        + static_cast<double>(hour_writes) * rate->input * 2.0
        + static_cast<double>(*usage.output_tokens) * rate->output) * multiplier / 1'000'000;
}

std::optional<double> mistral_cost(std::string_view model, const GenerationTokenUsage& usage) {
    // https://docs.mistral.ai/inference/pricing?currency=usd
    constexpr TokenRates prices[]{
        {"mistral-large-4", 0.70, 0.07, 2.10},
        {"mistral-large-4-0", 0.70, 0.07, 2.10},
        {"mistral-large-latest", 0.70, 0.07, 2.10},
        {"mistral-large-2512", 0.50, 0.05, 1.50},
        {"mistral-small-2603", 0.15, 0.015, 0.60},
        {"mistral-small-latest", 0.15, 0.015, 0.60},
    };
    const auto rate = std::ranges::find(prices, model, &TokenRates::model);
    if (rate == std::end(prices)) return std::nullopt;

    // Mistral may omit cached_tokens when there was no cache hit.
    const auto cached = usage.cache_read_tokens.value_or(0);
    if (cached > *usage.input_tokens || usage.cache_write_tokens.value_or(0)) return std::nullopt;
    return (static_cast<double>(*usage.input_tokens - cached) * rate->input
        + static_cast<double>(cached) * rate->cached_input
        + static_cast<double>(*usage.output_tokens) * rate->output) / 1'000'000;
}

} // namespace

std::optional<double> reported_text_model_cost(const ModelBackendConfig* config, const Json& usage) {
    if (!config || !usage.is_object()) return std::nullopt;
    if (is_openrouter_host(config->host)) return openrouter_cost(*config, usage);
    const auto host = host_name(config->host);
    if (ascii_iequals(host, "api.x.ai") || ascii_iequals(host, "us.api.x.ai")
        || ascii_iequals(host, "eu.api.x.ai")) {
        // https://docs.x.ai/developers/cost-tracking
        const auto tools = nonnegative_number(usage, "num_server_side_tools_used");
        if (tools ? *tools != 0 : config->web_search != WebSearchMode::off) return std::nullopt;
        if (const auto ticks = nonnegative_number(usage, "cost_in_usd_ticks")) return *ticks / 1e10;
    }
    return std::nullopt;
}

std::optional<double> estimate_text_model_cost(const ModelBackendConfig& config, const GenerationResult& round) {
    if (round.request_not_processed) return 0.0;
    if (config.mode != Mode::net || round.outcome != GenerationOutcome::completed) return std::nullopt;
    if (round.reported_model_cost_usd) return round.reported_model_cost_usd;
    if (round.usage_is_partial || !round.usage.input_tokens || !round.usage.output_tokens) return std::nullopt;

    const auto host = host_name(config.host);
    const auto api = config.api;
    // Token usage gives the direct OpenAI model cost independently of hosted tool fees.
    if ((api == ProviderApi::chat_completions || api == ProviderApi::responses) && is_direct_openai_host(host))
        return estimate_openai_token_cost(config, round);
    if (api == ProviderApi::google && ascii_iequals(host, "generativelanguage.googleapis.com"))
        return estimate_gemini_chat_token_cost(config, round);

    const std::string_view model = round.model.empty() ? config.model : round.model;
    if (round.service_tier.empty() || round.service_tier == "standard" || round.service_tier == "default") {
        if (api == ProviderApi::anthropic && ascii_iequals(host, "api.anthropic.com"))
            return anthropic_cost(model, round);
        if (api == ProviderApi::chat_completions && ascii_iequals(host, "api.mistral.ai"))
            return mistral_cost(model, round.usage);
    }
    return std::nullopt;
}

} // namespace cha
