#pragma once

#include <nlohmann/json_fwd.hpp>
#include <optional>

namespace cha {

struct GenerationResult;
struct ModelBackendConfig;

std::optional<double> reported_text_model_cost(
    const ModelBackendConfig* config, const nlohmann::json& usage);
std::optional<double> estimate_text_model_cost(
    const ModelBackendConfig& config, const GenerationResult& round);

} // namespace cha
