#pragma once

#include "ui/web/protocol.h"

#include <cstddef>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace cha::web {

bool is_json_content_type(std::string_view content_type);
nlohmann::json parse_json_body(std::string_view body, std::size_t maximum_bytes);
RawCommand parse_input_command(const nlohmann::json& json);
SetDefaultAgentCommand parse_default_agent_command(const nlohmann::json& json);

} // namespace cha::web
