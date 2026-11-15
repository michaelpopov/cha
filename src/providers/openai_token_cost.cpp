#include "providers/openai_token_cost.h"

#include "characters/character_config.h"
#include "providers/model_backend.h"

#include <algorithm>
#include <string_view>

namespace cha {
namespace {

struct Rates {
    double input{};
    double cache_read{};
    double cache_write{};
    double output{};
};

struct ModelRates {
    std::string_view model;
    std::string_view tier;
    Rates short_context;
    Rates long_context;
    bool short_context_only{};
};

// USD per million tokens, checked 2026-11-15.
// https://developers.openai.com/api/docs/pricing
// Notes use the provider's default 30-minute cache write lifetime.
constexpr ModelRates prices[]{
    {"gpt-6-astra", "standard", {10.00, 1.00, 12.50, 50.00}, {20.00, 2.00, 25.00, 75.00}},
    {"gpt-6.1-sol", "standard", {2.00, 0.10, 2.50, 10.00}, {4.00, 0.20, 5.00, 15.00}},
    {"gpt-6.1-terra", "standard", {1.00, 0.05, 1.25, 5.00}, {2.00, 0.10, 2.50, 7.50}},
    {"gpt-6-luna", "standard", {0.10, 0.01, 0.125, 0.50}, {0.20, 0.02, 0.25, 0.75}},
    {"gpt-6-sol", "standard", {2.00, 0.20, 2.50, 10.00}, {4.00, 0.40, 5.00, 15.00}},
    {"gpt-5.6-sol", "standard", {4.00, 0.40, 5.00, 20.00}, {8.00, 0.80, 10.00, 30.00}},
    {"gpt-5.6-terra", "standard", {2.00, 0.20, 2.50, 12.00}, {4.00, 0.40, 5.00, 18.00}},
    {"gpt-5.6-luna", "standard", {0.20, 0.02, 0.25, 1.20}, {0.40, 0.04, 0.50, 1.80}},
    {"gpt-5.5", "standard", {5.00, 0.50, 0, 30.00}, {10.00, 1.00, 0, 45.00}},
    {"gpt-5.5-pro", "standard", {30.00, 0, 0, 180.00}, {60.00, 0, 0, 270.00}},
    {"gpt-5.4", "standard", {2.50, 0.25, 0, 15.00}, {5.00, 0.50, 0, 22.50}},
    {"gpt-5.4-pro", "standard", {30.00, 0, 0, 180.00}, {60.00, 0, 0, 270.00}},
    {"gpt-5.4-mini", "standard", {0.75, 0.075, 0, 4.50}, {}},
    {"gpt-5.4-nano", "standard", {0.20, 0.02, 0, 1.25}, {}},
    {"gpt-5.2", "standard", {1.75, 0.175, 0, 14.00}, {}},
    {"gpt-5.2-pro", "standard", {21.00, 0, 0, 168.00}, {}},
    {"gpt-5.1", "standard", {1.25, 0.125, 0, 10.00}, {}},
    {"gpt-5", "standard", {1.25, 0.125, 0, 10.00}, {}},
    {"gpt-5-mini", "standard", {0.25, 0.025, 0, 2.00}, {}},
    {"gpt-5-nano", "standard", {0.05, 0.005, 0, 0.40}, {}},
    {"gpt-5-pro", "standard", {15.00, 0, 0, 120.00}, {}},
    {"gpt-4.1", "standard", {2.00, 0.50, 0, 8.00}, {}},
    {"gpt-4.1-mini", "standard", {0.40, 0.10, 0, 1.60}, {}},
    {"gpt-4o", "standard", {2.50, 1.25, 0, 10.00}, {}},
    {"gpt-4o-mini", "standard", {0.15, 0.075, 0, 0.60}, {}},
    {"o3-pro", "standard", {20.00, 0, 0, 80.00}, {}},
    {"o3", "standard", {2.00, 0.50, 0, 8.00}, {}},
    {"gpt-6-astra", "flex", {5.00, 0.50, 6.25, 25.00}, {10.00, 1.00, 12.50, 37.50}},
    {"gpt-6.1-sol", "flex", {1.00, 0.05, 1.25, 5.00}, {2.00, 0.10, 2.50, 7.50}},
    {"gpt-6.1-terra", "flex", {0.50, 0.025, 0.625, 2.50}, {1.00, 0.05, 1.25, 3.75}},
    {"gpt-6-luna", "flex", {0.05, 0.005, 0.0625, 0.25}, {0.10, 0.01, 0.125, 0.375}},
    {"gpt-6-sol", "flex", {1.00, 0.10, 1.25, 5.00}, {2.00, 0.20, 2.50, 7.50}},
    {"gpt-5.6-sol", "flex", {2.00, 0.20, 2.50, 10.00}, {4.00, 0.40, 5.00, 15.00}},
    {"gpt-5.6-terra", "flex", {1.00, 0.10, 1.25, 6.00}, {2.00, 0.20, 2.50, 9.00}},
    {"gpt-5.6-luna", "flex", {0.10, 0.01, 0.125, 0.60}, {0.20, 0.02, 0.25, 0.90}},
    {"gpt-5.5", "flex", {2.50, 0.25, 0, 15.00}, {5.00, 0.50, 0, 22.50}},
    {"gpt-5.5-pro", "flex", {15.00, 0, 0, 90.00}, {}, true},
    {"gpt-5.4", "flex", {1.25, 0.13, 0, 7.50}, {2.50, 0.25, 0, 11.25}},
    {"gpt-5.4-pro", "flex", {15.00, 0, 0, 90.00}, {30.00, 0, 0, 135.00}},
    {"gpt-5.4-mini", "flex", {0.375, 0.0375, 0, 2.25}, {}},
    {"gpt-5.4-nano", "flex", {0.10, 0.01, 0, 0.625}, {}},
    {"gpt-5.2", "flex", {0.875, 0.0875, 0, 7.00}, {}},
    {"gpt-5.1", "flex", {0.625, 0.0625, 0, 5.00}, {}},
    {"gpt-5", "flex", {0.625, 0.0625, 0, 5.00}, {}},
    {"gpt-5-mini", "flex", {0.125, 0.0125, 0, 1.00}, {}},
    {"gpt-5-nano", "flex", {0.025, 0.0025, 0, 0.20}, {}},
    {"o3", "flex", {1.00, 0.25, 0, 4.00}, {}},
    {"gpt-6-astra", "priority", {20.00, 2.00, 25.00, 100.00}, {40.00, 4.00, 50.00, 150.00}},
    {"gpt-6.1-sol", "priority", {4.00, 0.20, 5.00, 20.00}, {8.00, 0.40, 10.00, 30.00}},
    {"gpt-6.1-terra", "priority", {2.00, 0.10, 2.50, 10.00}, {4.00, 0.20, 5.00, 15.00}},
    {"gpt-6-luna", "priority", {0.20, 0.02, 0.25, 1.00}, {0.40, 0.04, 0.50, 1.50}},
    {"gpt-6-sol", "priority", {4.00, 0.40, 5.00, 20.00}, {8.00, 0.80, 10.00, 30.00}},
    {"gpt-5.6-sol", "priority", {8.00, 0.80, 10.00, 40.00}, {16.00, 1.60, 20.00, 60.00}},
    {"gpt-5.6-terra", "priority", {4.00, 0.40, 5.00, 24.00}, {8.00, 0.80, 10.00, 36.00}},
    {"gpt-5.6-luna", "priority", {0.40, 0.04, 0.50, 2.40}, {0.80, 0.08, 1.00, 3.60}},
    {"gpt-5.5", "priority", {12.50, 1.25, 0, 75.00}, {}, true},
    {"gpt-5.4", "priority", {5.00, 0.50, 0, 30.00}, {}, true},
    {"gpt-5.4-mini", "priority", {1.50, 0.15, 0, 9.00}, {}},
    {"gpt-5.2", "priority", {3.50, 0.35, 0, 28.00}, {}},
    {"gpt-5.1", "priority", {2.50, 0.25, 0, 20.00}, {}},
    {"gpt-5", "priority", {2.50, 0.25, 0, 20.00}, {}},
    {"gpt-5-mini", "priority", {0.45, 0.045, 0, 3.60}, {}},
    {"gpt-4.1", "priority", {3.50, 0.875, 0, 14.00}, {}},
    {"gpt-4.1-mini", "priority", {0.70, 0.175, 0, 2.80}, {}},
    {"gpt-4o", "priority", {4.25, 2.125, 0, 17.00}, {}},
    {"gpt-4o-mini", "priority", {0.25, 0.125, 0, 1.00}, {}},
    {"o3", "priority", {3.50, 0.875, 0, 14.00}, {}},
    {"gpt-6-astra", "ultrafast", {60.00, 6.00, 75.00, 300.00}, {120.00, 12.00, 150.00, 450.00}},
    {"gpt-6.1-sol", "ultrafast", {12.00, 0.60, 15.00, 60.00}, {24.00, 1.20, 30.00, 90.00}},
};

std::string_view pricing_model(std::string_view model) {
    // A dated snapshot uses the rates published for its base model. The first
    // gpt-4o snapshot has its own higher rates, which this table does not list.
    if (model != "gpt-4o-2024-05-13" && model.size() > 11) {
        const auto date = model.substr(model.size() - 11);
        if (date[0] == '-' && date[5] == '-' && date[8] == '-'
            && date.substr(1, 4).find_first_not_of("0123456789") == std::string_view::npos
            && date.substr(6, 2).find_first_not_of("0123456789") == std::string_view::npos
            && date.substr(9, 2).find_first_not_of("0123456789") == std::string_view::npos)
            model.remove_suffix(11);
    }
    return model;
}

} // namespace

bool uses_openai_api_key(const ModelBackendConfig& config) {
    return config.mode == Mode::net && config.auth == ProviderAuth::none
        && (config.api == ProviderApi::responses || config.api == ProviderApi::chat_completions)
        && is_direct_openai_host(config.host)
        && (!config.api_key_id.empty() || !config.api_key_env.empty());
}

std::optional<double> estimate_openai_token_cost(
    const ModelBackendConfig& config, const GenerationResult& result) {
    if (result.request_not_processed) return 0.0;
    const auto& usage = result.usage;
    if (!usage.input_tokens || !usage.output_tokens) return std::nullopt;

    const auto input = *usage.input_tokens;
    const auto model = pricing_model(result.model.empty() ? config.model : result.model);
    std::string_view tier = result.service_tier.empty()
        ? (config.flex_processing ? "flex" : "priority") : std::string_view(result.service_tier);
    if (tier == "default") tier = "standard";
    if (tier == "fast") tier = "priority";
    const auto row = std::ranges::find_if(prices, [&](const ModelRates& price) {
        return price.model == model && price.tier == tier;
    });
    if (row == std::end(prices)) return std::nullopt;
    if (input > 272'000 && row->short_context_only) return std::nullopt;

    const auto& rates = input > 272'000 && row->long_context.input > 0
        ? row->long_context : row->short_context;
    if ((rates.cache_read > 0 && !usage.cache_read_tokens)
        || (rates.cache_write > 0 && !usage.cache_write_tokens)) return std::nullopt;

    const auto reads = usage.cache_read_tokens.value_or(0);
    const auto writes = usage.cache_write_tokens.value_or(0);
    if (reads > input || writes > input - reads
        || (reads && rates.cache_read == 0) || (writes && rates.cache_write == 0)) return std::nullopt;

    return (static_cast<double>(input - reads - writes) * rates.input
        + static_cast<double>(reads) * rates.cache_read
        + static_cast<double>(writes) * rates.cache_write
        + static_cast<double>(*usage.output_tokens) * rates.output) / 1'000'000;
}

} // namespace cha
