#pragma once

#include <optional>

namespace cha {

struct GenerationResult;
struct ModelBackendConfig;

// USD for one Gemini Developer API Flex text request.
std::optional<double> estimate_gemini_flex_token_cost(
    const ModelBackendConfig& config, const GenerationResult& result);

} // namespace cha
