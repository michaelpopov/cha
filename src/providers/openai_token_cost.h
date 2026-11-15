#pragma once

#include <optional>

namespace cha {

struct GenerationResult;
struct ModelBackendConfig;

bool uses_openai_api_key(const ModelBackendConfig& config);

// USD for one text request. Empty when its token cost cannot be estimated.
std::optional<double> estimate_openai_token_cost(
    const ModelBackendConfig& config, const GenerationResult& result);

} // namespace cha
