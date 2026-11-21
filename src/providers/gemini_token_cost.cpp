#include "providers/gemini_token_cost.h"

#include "characters/character_config.h"
#include "providers/model_backend.h"

#include <algorithm>
#include <chrono>
#include <span>

namespace cha {
namespace {

using namespace std::chrono;

struct Rates {
    double input;
    double cached_input;
    double output;
};

struct ModelRates {
    std::string_view model;
    Rates rates;
    Rates long_context{};
    // Google has published that all three promotional token rates double on this date.
    sys_days full_price_from{sys_days::max()};
};

constexpr sys_days december_2026{year{2026}/December/1};
constexpr sys_days january_2027{year{2027}/January/1};

// Paid Flex rates in USD per million text tokens, checked 2026-11-15.
// https://aistudio.google.com/docs/pricing
constexpr ModelRates flex_prices[]{
    {"gemini-4-carbon", {1.00, 0.05, 5.00}, {}, december_2026},
    {"gemini-4-argon", {1.00, 0.05, 5.00}, {}, december_2026},
    {"gemini-4-neon", {1.50, 0.075, 7.50}},
    {"gemini-4-hydrogen", {0.75, 0.0375, 3.75}},
    {"gemini-3.8-flash", {0.375, 0.0375, 1.875}, {}, january_2027},
    {"gemini-3.6-flash", {0.375, 0.0375, 1.875}, {}, january_2027},
    {"gemini-3.5-flash-lite", {0.15, 0.02, 1.25}},
    {"gemini-3.1-flash-lite", {0.125, 0.0125, 0.75}},
    {"gemini-3.1-pro-preview", {1.00, 0.20, 6.00}, {2.00, 0.40, 9.00}},
    {"gemini-3.1-pro-preview-customtools", {1.00, 0.20, 6.00}, {2.00, 0.40, 9.00}},
    {"gemini-3-flash-preview", {0.25, 0.05, 1.50}},
    {"gemini-2.5-pro", {0.625, 0.125, 5.00}, {1.25, 0.25, 7.50}},
    {"gemini-2.5-flash", {0.15, 0.03, 1.25}},
    {"gemini-2.5-flash-lite", {0.05, 0.01, 0.20}},
};

// Paid Standard and Priority text rates, checked 2026-11-21.
// https://aistudio.google.com/docs/pricing
constexpr ModelRates standard_prices[]{
    {"gemini-4-carbon", {2.00, 0.10, 10.00}, {}, december_2026},
    {"gemini-4-argon", {2.00, 0.10, 10.00}, {}, december_2026},
    {"gemini-4-neon", {3.00, 0.15, 15.00}},
    {"gemini-4-hydrogen", {1.50, 0.075, 7.50}},
    {"gemini-3.8-flash", {0.75, 0.075, 3.75}, {}, january_2027},
    {"gemini-3.6-flash", {0.75, 0.075, 3.75}, {}, january_2027},
    {"gemini-3.5-flash-lite", {0.30, 0.03, 2.50}},
    {"gemini-3.1-flash-lite", {0.25, 0.025, 1.50}},
    {"gemini-3.1-pro-preview", {2.00, 0.20, 12.00}, {4.00, 0.40, 18.00}},
    {"gemini-3.1-pro-preview-customtools", {2.00, 0.20, 12.00}, {4.00, 0.40, 18.00}},
    {"gemini-3-flash-preview", {0.50, 0.05, 3.00}},
    {"gemini-2.5-pro", {1.25, 0.125, 10.00}, {2.50, 0.25, 15.00}},
    {"gemini-2.5-flash", {0.30, 0.03, 2.50}},
    {"gemini-2.5-flash-lite", {0.10, 0.01, 0.40}},
};

constexpr ModelRates priority_prices[]{
    {"gemini-4-carbon", {3.60, 0.18, 18.00}, {}, december_2026},
    {"gemini-4-argon", {3.60, 0.18, 18.00}, {}, december_2026},
    {"gemini-4-neon", {5.40, 0.27, 27.00}},
    {"gemini-4-hydrogen", {2.70, 0.135, 13.50}},
    {"gemini-3.8-flash", {1.35, 0.135, 6.75}, {}, january_2027},
    {"gemini-3.6-flash", {1.35, 0.135, 6.75}, {}, january_2027},
    // Google lists $0.05 for Priority cached input on this model.
    {"gemini-3.5-flash-lite", {0.54, 0.05, 4.50}},
    {"gemini-3.1-flash-lite", {0.45, 0.045, 2.70}},
    {"gemini-3.1-pro-preview", {3.60, 0.36, 21.60}, {7.20, 0.72, 32.40}},
    {"gemini-3.1-pro-preview-customtools", {3.60, 0.36, 21.60}, {7.20, 0.72, 32.40}},
    {"gemini-3-flash-preview", {0.90, 0.09, 5.40}},
    {"gemini-2.5-pro", {2.25, 0.225, 18.00}, {4.50, 0.45, 27.00}},
    {"gemini-2.5-flash", {0.54, 0.054, 4.50}},
    {"gemini-2.5-flash-lite", {0.18, 0.018, 0.72}},
};

std::optional<double> estimate_text_cost(
    const ModelBackendConfig& config, const GenerationResult& result,
    std::span<const ModelRates> prices) {
    // A stream can report intermediate usage before it reaches a terminal status.
    if (result.outcome != GenerationOutcome::completed && result.finish_reason.empty()) return std::nullopt;

    const auto& usage = result.usage;
    if (!usage.input_tokens || !usage.output_tokens) return std::nullopt;
    const auto input = *usage.input_tokens;
    // With implicit caching, Gemini can omit this count when there was no cache hit.
    const auto cached = usage.cache_read_tokens.value_or(0);
    if (cached > input) return std::nullopt;

    std::string_view model = result.model.empty() ? config.model : result.model;
    if (model.starts_with("models/")) model.remove_prefix(7);
    const auto row = std::ranges::find(prices, model, &ModelRates::model);
    if (row == std::end(prices)) return std::nullopt;

    const auto& rates = input > 200'000 && row->long_context.input > 0 ? row->long_context : row->rates;
    const double multiplier = floor<days>(system_clock::now()) >= row->full_price_from ? 2.0 : 1.0;
    // CHA uses implicit caching here. Google applies the cache rate to reads.
    return (static_cast<double>(input - cached) * rates.input
        + static_cast<double>(cached) * rates.cached_input
        + static_cast<double>(*usage.output_tokens) * rates.output) * multiplier / 1'000'000;
}

} // namespace

std::optional<double> estimate_gemini_flex_token_cost(
    const ModelBackendConfig& config, const GenerationResult& result) {
    if (result.request_not_processed) return 0.0;
    if (!result.service_tier.empty() && result.service_tier != "flex") return std::nullopt;
    return estimate_text_cost(config, result, flex_prices);
}

std::optional<double> estimate_gemini_chat_token_cost(
    const ModelBackendConfig& config, const GenerationResult& result) {
    const std::string_view tier = result.service_tier.empty()
        ? (config.flex_processing ? "flex" : "standard") : std::string_view(result.service_tier);
    if (tier == "flex") return estimate_gemini_flex_token_cost(config, result);
    if (tier == "standard" || tier == "unspecified") return estimate_text_cost(config, result, standard_prices);
    if (tier == "priority") return estimate_text_cost(config, result, priority_prices);
    return std::nullopt;
}

double estimate_gemini_batch_audio_cost(
    std::uint64_t text_tokens, std::uint64_t audio_tokens, sys_days batch_date) {
    // https://aistudio.google.com/docs/pricing#gemini-3.8-flash-tts
    const double multiplier = batch_date >= january_2027 ? 2.0 : 1.0;
    return (text_tokens * 0.25 + audio_tokens * 4.50) * multiplier / 1'000'000;
}

} // namespace cha
