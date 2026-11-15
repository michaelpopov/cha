#include "providers/gemini_token_cost.h"

#include "characters/character_config.h"
#include "providers/model_backend.h"

#include <algorithm>
#include <chrono>

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
constexpr ModelRates prices[]{
    {"gemini-4-carbon", {1.00, 0.05, 5.00}, {}, december_2026},
    {"gemini-4-argon", {1.00, 0.05, 5.00}, {}, december_2026},
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

} // namespace

std::optional<double> estimate_gemini_flex_token_cost(
    const ModelBackendConfig& config, const GenerationResult& result) {
    if (result.request_not_processed) return 0.0;
    if (!result.service_tier.empty() && result.service_tier != "flex") return std::nullopt;
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

} // namespace cha
