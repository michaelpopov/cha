#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace cha {

struct GenerationResult;
struct ModelBackendConfig;

// USD for one Gemini Developer API Flex text request.
std::optional<double> estimate_gemini_flex_token_cost(
    const ModelBackendConfig& config, const GenerationResult& result);
// USD for gemini-3.8-flash-tts on the Gemini Developer API Batch endpoint.
double estimate_gemini_batch_audio_cost(
    std::uint64_t text_tokens, std::uint64_t audio_tokens, std::chrono::sys_days batch_date);

} // namespace cha
