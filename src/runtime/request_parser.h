#pragma once

#include "runtime/protocol.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace cha {

const std::string& required_string(
    const nlohmann::json& json,
    std::string_view key);
RawCommand parse_input_command(const nlohmann::json& json);
CoverCommand parse_cover_command(const nlohmann::json& json);
DeleteTurnCommand parse_delete_turn_command(const nlohmann::json& json);
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
CreateProviderRequest parse_create_provider_request(const nlohmann::json& json);
ProviderUpdate parse_provider_update(
    const nlohmann::json& json,
    const std::vector<std::string>& existing_openrouter_targets);
std::string parse_create_display_name(const nlohmann::json& json);
StyleUpdate parse_style_update(const nlohmann::json& json);
CreateVoiceRequest parse_create_voice_request(const nlohmann::json& json);
VoiceUpdate parse_voice_update(const nlohmann::json& json);
VoiceInputSettings parse_voice_input_settings(const nlohmann::json& json);
VoiceOutputSettings parse_voice_output_settings(const nlohmann::json& json);
CreateApiKeyRequest parse_create_api_key_request(const nlohmann::json& json);
std::string parse_rename_display_name(const nlohmann::json& json);
std::string parse_replace_secret_value(const nlohmann::json& json);
SaveR2StorageRequest parse_save_r2_storage_request(const nlohmann::json& json);

} // namespace cha
