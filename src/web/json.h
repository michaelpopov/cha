#pragma once

#include "web/protocol.h"

#include <cstddef>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace cha::web {

bool is_json_content_type(std::string_view content_type);
nlohmann::json parse_json_body(std::string_view body, std::size_t maximum_bytes);
RawCommand parse_input_command(const nlohmann::json& json);
SetDefaultCharacterCommand parse_default_character_command(const nlohmann::json& json);
std::string parse_create_session_label(const nlohmann::json& json);
std::string parse_rename_session_label(const nlohmann::json& json);
CharacterSettingsUpdate parse_character_settings_update(const nlohmann::json& json);
void parse_empty_object(const nlohmann::json& json);

} // namespace cha::web
