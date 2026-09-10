#pragma once

#include "web/protocol.h"

#include <cstddef>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace cha::web {

bool is_json_content_type(std::string_view content_type);
nlohmann::json parse_json_body(std::string_view body, std::size_t maximum_bytes);
RawCommand parse_input_command(const nlohmann::json& json);
CoverCommand parse_cover_command(const nlohmann::json& json);
SetDefaultCharacterCommand parse_default_character_command(const nlohmann::json& json);
std::string parse_create_session_label(const nlohmann::json& json);
std::string parse_create_persona_name(const nlohmann::json& json);
CreateForumRequest parse_create_forum_request(const nlohmann::json& json);
CreateCharacterRequest parse_create_character_request(const nlohmann::json& json);
std::string parse_rename_session_label(const nlohmann::json& json);
std::string parse_vault_switch_name(const nlohmann::json& json);
CharacterSettingsUpdate parse_character_settings_update(const nlohmann::json& json);
CharacterDefinitionUpdate parse_character_definition_update(const nlohmann::json& json);
PersonaUpdate parse_persona_update(const nlohmann::json& json);
ForumUpdate parse_forum_update(const nlohmann::json& json);
ForumMembersUpdate parse_forum_members_update(const nlohmann::json& json);
void parse_empty_object(const nlohmann::json& json);

} // namespace cha::web
