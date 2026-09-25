#pragma once

#include "characters/character_config.h"
#include "providers/model_backend.h"

#include <nlohmann/json.hpp>

namespace cha {

void add_web_search_tool(nlohmann::json& body, ProviderApi api);
GenerationResult tool_call_result(const nlohmann::json& continuation,
    ProviderApi api, bool received_answer, GenerationTokenUsage usage, bool collect_tool_calls,
    std::string_view no_answer_message = "Response completed without answer content",
    std::string_view finish_reason = {});

} // namespace cha
