#include "runtime/request_parser.h"
#include "util/logging.h"

#include "characters/character_config.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cha {
namespace {

void exact_keys(
    const nlohmann::json& json,
    std::initializer_list<std::string_view> keys) {
    if (!json.is_object() || json.size() != keys.size()) {
        throw std::invalid_argument("Invalid web command");
    }
    for (const std::string_view key : keys) {
        if (!json.contains(std::string(key))) {
            throw std::invalid_argument("Invalid web command");
        }
    }
}

} // namespace

const std::string& required_string(
    const nlohmann::json& json,
    std::string_view key) {
    const std::string name(key);
    if (!json.is_object() || !json.contains(name) || !json.at(name).is_string()) {
        throw std::invalid_argument("Invalid web command");
    }
    return json.at(name).get_ref<const std::string&>();
}

RawCommand parse_input_command(const nlohmann::json& json) {
    exact_keys(json, {"text"});
    return {required_string(json, "text")};
}

CoverCommand parse_cover_command(const nlohmann::json& json) {
    exact_keys(json, {"through_entry_id"});
    const nlohmann::json& value = json.at("through_entry_id");
    if (!value.is_number_integer() || value.get<std::int64_t>() <= 0) {
        throw std::invalid_argument("Invalid web command");
    }
    return {static_cast<EntryId>(value.get<std::int64_t>())};
}

DeleteTurnCommand parse_delete_turn_command(const nlohmann::json& json) {
    exact_keys(json, {"response_entry_id"});
    const nlohmann::json& value = json.at("response_entry_id");
    if (!value.is_number_integer() || value.get<std::int64_t>() <= 0) {
        throw std::invalid_argument("Invalid web command");
    }
    return {static_cast<EntryId>(value.get<std::int64_t>())};
}

SetDefaultCharacterCommand parse_default_character_command(
    const nlohmann::json& json) {
    exact_keys(json, {"character_id"});
    return {required_string(json, "character_id")};
}

std::string parse_create_session_label(const nlohmann::json& json) {
    exact_keys(json, {"label"});
    return required_string(json, "label");
}

std::string parse_create_persona_name(const nlohmann::json& json) {
    exact_keys(json, {"display_name"});
    return required_string(json, "display_name");
}

CreateForumRequest parse_create_forum_request(const nlohmann::json& json) {
    exact_keys(json, {"display_name", "persona_id"});
    return {
        .display_name = required_string(json, "display_name"),
        .persona_id = required_string(json, "persona_id"),
    };
}

CreateCharacterRequest parse_create_character_request(const nlohmann::json& json) {
    exact_keys(json, {"display_name", "description"});
    return {
        .display_name = required_string(json, "display_name"),
        .description = required_string(json, "description"),
    };
}

std::string parse_rename_session_label(const nlohmann::json& json) {
    exact_keys(json, {"label"});
    return required_string(json, "label");
}

std::string parse_vault_switch_name(const nlohmann::json& json) {
    exact_keys(json, {"vault_name"});
    return required_string(json, "vault_name");
}

namespace {

std::optional<std::string> nullable_string(
    const nlohmann::json& json,
    std::string_view key) {
    const std::string name(key);
    if (!json.contains(name)) {
        throw std::invalid_argument("Invalid web command");
    }
    const nlohmann::json& value = json.at(name);
    if (value.is_null()) return std::nullopt;
    if (!value.is_string()) {
        throw std::invalid_argument("Invalid web command");
    }
    return value.get<std::string>();
}

std::optional<std::string> nullable_reasoning_effort(
    const nlohmann::json& json) {
    std::optional<std::string> value = nullable_string(json, "reasoning_effort");
    if (value && *value != "low" && *value != "medium"
        && *value != "high" && *value != "xhigh") {
        throw std::invalid_argument("Invalid web command");
    }
    return value;
}

std::optional<WebSearchMode> nullable_web_search(
    const nlohmann::json& json) {
    const std::optional<std::string> value = nullable_string(json, "web_search");
    if (!value) return std::nullopt;
    if (const auto mode = parse_web_search_mode(*value)) return mode;
    throw std::invalid_argument("Invalid web command");
}

} // namespace

CharacterSettingsUpdate parse_character_settings_update(const nlohmann::json& json) {
    exact_keys(json, {
        "provider", "style", "voice_id", "reasoning_effort", "web_search"});
    return {
        .provider = required_string(json, "provider"),
        .style = nullable_string(json, "style"),
        .voice = nullable_string(json, "voice_id"),
        .reasoning_effort = nullable_reasoning_effort(json),
        .web_search = nullable_web_search(json),
    };
}

CharacterDefinitionUpdate parse_character_definition_update(
    const nlohmann::json& json) {
    if (!json.is_object() || json.empty() || json.size() > 2) {
        throw std::invalid_argument("Invalid web command");
    }
    CharacterDefinitionUpdate update;
    for (const auto& [key, value] : json.items()) {
        if (!value.is_string()) {
            throw std::invalid_argument("Invalid web command");
        }
        if (key == "display_name") {
            update.display_name = value.get<std::string>();
        } else if (key == "character_markdown") {
            update.character_markdown = value.get<std::string>();
        } else {
            throw std::invalid_argument("Invalid web command");
        }
    }
    return update;
}

PersonaUpdate parse_persona_update(const nlohmann::json& json) {
    if (!json.is_object() || json.empty() || json.size() > 4) {
        throw std::invalid_argument("Invalid web command");
    }
    PersonaUpdate update;
    for (const auto& [key, value] : json.items()) {
        if (key == "display_name") {
            if (!value.is_string()) throw std::invalid_argument("Invalid web command");
            update.display_name = value.get<std::string>();
        } else if (key == "persona_markdown") {
            if (!value.is_string()) throw std::invalid_argument("Invalid web command");
            update.persona_markdown = value.get<std::string>();
        } else if (key == "style") {
            if (!value.is_null() && !value.is_string()) {
                throw std::invalid_argument("Invalid web command");
            }
            update.style = value.is_null()
                ? std::optional<std::string>{}
                : std::optional<std::string>{value.get<std::string>()};
        } else if (key == "voice_id") {
            if (!value.is_null() && !value.is_string()) {
                throw std::invalid_argument("Invalid web command");
            }
            update.voice = value.is_null()
                ? std::optional<std::string>{}
                : std::optional<std::string>{value.get<std::string>()};
        } else {
            throw std::invalid_argument("Invalid web command");
        }
    }
    return update;
}

ForumUpdate parse_forum_update(const nlohmann::json& json) {
    if (!json.is_object() || json.empty() || json.size() > 2) {
        throw std::invalid_argument("Invalid web command");
    }
    ForumUpdate update;
    for (const auto& [key, value] : json.items()) {
        if (!value.is_string()) {
            throw std::invalid_argument("Invalid web command");
        }
        if (key == "display_name") {
            update.display_name = value.get<std::string>();
        } else if (key == "forum_markdown") {
            update.forum_markdown = value.get<std::string>();
        } else {
            throw std::invalid_argument("Invalid web command");
        }
    }
    return update;
}

ForumMembersUpdate parse_forum_members_update(const nlohmann::json& json) {
    exact_keys(json, {"character_ids", "persona_id"});
    const nlohmann::json& ids = json.at("character_ids");
    const nlohmann::json& persona_id = json.at("persona_id");
    if (!ids.is_array() || ids.empty() || !persona_id.is_string()
        || persona_id.get_ref<const std::string&>().empty()) {
        throw std::invalid_argument("Invalid web command");
    }
    ForumMembersUpdate update;
    update.persona_id = persona_id.get<std::string>();
    std::set<std::string> unique;
    for (const nlohmann::json& id : ids) {
        if (!id.is_string()) throw std::invalid_argument("Invalid web command");
        std::string value = id.get<std::string>();
        if (value.empty() || !unique.insert(value).second) {
            throw std::invalid_argument("Invalid web command");
        }
        update.character_ids.push_back(std::move(value));
    }
    return update;
}

void parse_empty_object(const nlohmann::json& json) {
    exact_keys(json, {});
}

namespace {

template<typename Value>
Value required_field(const nlohmann::json& json, std::string_view name) {
    const auto found = json.find(std::string(name));
    if (found == json.end()) throw std::invalid_argument("Missing field");
    try {
        return found->get<Value>();
    } catch (const nlohmann::json::exception&) {
        throw std::invalid_argument("Invalid field");
    }
}

std::optional<std::string> settings_nullable_string(
    const nlohmann::json& json,
    std::string_view name) {
    const auto found = json.find(std::string(name));
    if (found == json.end()) throw std::invalid_argument("Missing field");
    if (found->is_null()) return std::nullopt;
    if (!found->is_string()) throw std::invalid_argument("Invalid field");
    return found->get<std::string>();
}

std::optional<double> settings_nullable_double(
    const nlohmann::json& json,
    std::string_view name) {
    const auto found = json.find(std::string(name));
    if (found == json.end()) throw std::invalid_argument("Missing field");
    if (found->is_null()) return std::nullopt;
    if (!found->is_number()) throw std::invalid_argument("Invalid field");
    const double result = found->get<double>();
    if (!std::isfinite(result)) throw std::invalid_argument("Invalid field");
    return result;
}

std::optional<int> settings_nullable_int(
    const nlohmann::json& json,
    std::string_view name) {
    const auto found = json.find(std::string(name));
    if (found == json.end()) throw std::invalid_argument("Missing field");
    if (found->is_null()) return std::nullopt;
    if (!found->is_number_integer()) throw std::invalid_argument("Invalid field");
    return found->get<int>();
}

template<typename Enum>
Enum settings_choice(
    std::string_view value,
    std::initializer_list<std::pair<std::string_view, Enum>> choices) {
    for (const auto& [name, result] : choices) {
        if (name == value) return result;
    }
    throw std::invalid_argument("Invalid choice");
}

template<typename Enum>
Enum settings_choice(
    std::string_view value,
    std::optional<Enum> (*parse)(std::string_view)) {
    if (const auto result = parse(value)) return *result;
    throw std::invalid_argument("Invalid choice");
}

} // namespace

CreateProviderRequest parse_create_provider_request(const nlohmann::json& json) {
    if (!json.is_object() || json.size() != 2) {
        throw std::invalid_argument("Invalid provider");
    }
    return {
        .display_name = required_field<std::string>(json, "display_name"),
        .copy_from = settings_nullable_string(json, "copy_from").value_or(""),
    };
}

ProviderUpdate parse_provider_update(
    const nlohmann::json& json,
    const std::vector<std::string>& existing_openrouter_targets) {
    static constexpr std::size_t legacy_field_count = 19;
    const bool includes_openrouter_targets =
        json.is_object() && json.contains("openrouter_targets");
    if (!json.is_object()
        || json.size() != legacy_field_count + includes_openrouter_targets) {
        throw std::invalid_argument("Invalid provider");
    }
    ProviderUpdate result;
    result.display_name = required_field<std::string>(json, "display_name");
    result.config.host = required_field<std::string>(json, "host");
    result.config.port = required_field<int>(json, "port");
    result.config.base_path = required_field<std::string>(json, "base_path");
    result.config.mode = settings_choice(
        required_field<std::string>(json, "mode"), parse_mode);
    result.config.model = required_field<std::string>(json, "model");
    result.config.stream = required_field<bool>(json, "stream");
    result.config.temperature = settings_nullable_double(json, "temperature");
    result.config.max_tokens = settings_nullable_int(json, "max_tokens");
    result.config.timeout_s = required_field<int>(json, "timeout_s");
    result.config.idle_timeout_s = required_field<int>(json, "idle_timeout_s");
    result.config.api_key_id =
        settings_nullable_string(json, "api_key").value_or("");
    result.config.reasoning_effort =
        required_field<std::string>(json, "reasoning_effort");
    result.config.reasoning_format = settings_choice(
        required_field<std::string>(json, "reasoning_format"),
        parse_reasoning_format);
    result.config.https = required_field<bool>(json, "https");
    result.config.api = settings_choice(
        required_field<std::string>(json, "api"), parse_provider_api);
    result.config.auth = settings_choice(
        required_field<std::string>(json, "auth"), parse_provider_auth);
    result.config.web_search = settings_choice(
        required_field<std::string>(json, "web_search"), parse_web_search_mode);
    result.config.cache_retention = settings_choice(
        required_field<std::string>(json, "cache_retention"),
        parse_cache_retention);
    if (includes_openrouter_targets) {
        result.config.openrouter_targets =
            required_field<std::vector<std::string>>(json, "openrouter_targets");
    } else if (is_openrouter_host(result.config.host)) {
        result.config.openrouter_targets = existing_openrouter_targets;
    }
    if (!valid_openrouter_targets(result.config)) {
        throw std::invalid_argument("Invalid OpenRouter inference targets");
    }
    return result;
}

std::string parse_create_display_name(const nlohmann::json& json) {
    if (!json.is_object() || json.size() != 1) {
        throw std::invalid_argument("Invalid settings item");
    }
    return required_field<std::string>(json, "display_name");
}

StyleUpdate parse_style_update(const nlohmann::json& json) {
    if (!json.is_object() || json.size() != 6) {
        throw std::invalid_argument("Invalid style");
    }
    return {
        .display_name = required_field<std::string>(json, "display_name"),
        .appearance = {
            .font = settings_choice<CharacterFont>(
                required_field<std::string>(json, "font"),
                {{"sans", CharacterFont::sans},
                 {"serif", CharacterFont::serif},
                 {"mono", CharacterFont::mono}}),
            .style = settings_choice<CharacterSlant>(
                required_field<std::string>(json, "style"),
                {{"normal", CharacterSlant::normal},
                 {"italic", CharacterSlant::italic}}),
            .weight = settings_choice<CharacterWeight>(
                required_field<std::string>(json, "weight"),
                {{"light", CharacterWeight::light},
                 {"normal", CharacterWeight::normal},
                 {"medium", CharacterWeight::medium},
                 {"semibold", CharacterWeight::semibold},
                 {"bold", CharacterWeight::bold}}),
            .size = settings_choice<CharacterScale>(
                required_field<std::string>(json, "size"),
                {{"small", CharacterScale::small},
                 {"normal", CharacterScale::normal},
                 {"large", CharacterScale::large}}),
            .text_color = settings_choice<CharacterTextColor>(
                required_field<std::string>(json, "text_color"),
                {{"normal", CharacterTextColor::normal},
                 {"muted", CharacterTextColor::muted},
                 {"accent", CharacterTextColor::accent}}),
        },
    };
}

CreateVoiceRequest parse_create_voice_request(const nlohmann::json& json) {
    if (!json.is_object() || json.size() != 3) {
        throw std::invalid_argument("Invalid voice");
    }
    return {
        .display_name = required_field<std::string>(json, "display_name"),
        .description = required_field<std::string>(json, "description"),
        .elevenlabs_voice_id =
            required_field<std::string>(json, "elevenlabs_voice_id"),
    };
}

VoiceUpdate parse_voice_update(const nlohmann::json& json) {
    if (!json.is_object() || json.size() != 4) {
        throw std::invalid_argument("Invalid voice");
    }
    return {
        .display_name = required_field<std::string>(json, "display_name"),
        .description = required_field<std::string>(json, "description"),
        .elevenlabs_voice_id =
            required_field<std::string>(json, "elevenlabs_voice_id"),
        .settings = {
            .speed = settings_nullable_double(json, "speed"),
        },
    };
}

JevSettings parse_jev_settings(const nlohmann::json& json) {
    if (!json.is_object()) throw std::invalid_argument("Invalid recipient detection settings");
    for (const auto& [key, value] : json.items()) {
        (void)value;
        if (key != "url" && key != "model" && key != "api_key")
            log_warn("Ignoring unused recipient detection field: " + key);
    }
    return {required_field<std::string>(json, "url"),
        required_field<std::string>(json, "model"), required_field<std::string>(json, "api_key")};
}

WebSearchSettings parse_web_search_settings(const nlohmann::json& json) {
    if (!json.is_object()) throw std::invalid_argument("Invalid web search settings");
    for (const auto& [key, value] : json.items()) {
        (void)value;
        if (key != "enabled" && key != "provider" && key != "api_key"
            && key != "query_provider")
            log_warn("Ignoring unused web search field: " + key);
    }
    return {required_field<bool>(json, "enabled"),
        required_field<std::string>(json, "provider"),
        required_field<std::string>(json, "api_key"),
        required_field<std::string>(json, "query_provider")};
}

VoiceInputSettings parse_voice_input_settings(const nlohmann::json& json) {
    if (!json.is_object() || json.size() != 7) {
        throw std::invalid_argument("Invalid voice input settings");
    }
    VoiceInputSettings settings{
        .provider = required_field<std::string>(json, "provider"),
        .url = required_field<std::string>(json, "url"),
        .model = required_field<std::string>(json, "model"),
        .api_key = required_field<std::string>(json, "api_key"),
        .delay = required_field<std::string>(json, "delay"),
        .prompt = required_field<std::string>(json, "prompt"),
        .send_phrase = required_field<std::string>(json, "send_phrase"),
    };
    if (settings.provider != "openai" && settings.provider != "xai") {
        throw std::invalid_argument("Invalid voice input settings");
    }
    return settings;
}

VoiceOutputSettings parse_voice_output_settings(const nlohmann::json& json) {
    if (!json.is_object() || json.size() != 5) {
        throw std::invalid_argument("Invalid voice output settings");
    }
    return {
        .url = required_field<std::string>(json, "url"),
        .model = required_field<std::string>(json, "model"),
        .api_key = required_field<std::string>(json, "api_key"),
        .output_format = required_field<std::string>(json, "output_format"),
        .default_voice = required_field<std::string>(json, "default_voice"),
    };
}

CreateApiKeyRequest parse_create_api_key_request(const nlohmann::json& json) {
    if (!json.is_object() || json.size() != 2) {
        throw std::invalid_argument("Invalid API key");
    }
    return {
        .display_name = required_field<std::string>(json, "display_name"),
        .value = required_field<std::string>(json, "value"),
    };
}

std::string parse_rename_display_name(const nlohmann::json& json) {
    if (!json.is_object() || json.size() != 1) {
        throw std::invalid_argument("Invalid API key");
    }
    return required_field<std::string>(json, "display_name");
}

std::string parse_replace_secret_value(const nlohmann::json& json) {
    if (!json.is_object() || json.size() != 1) {
        throw std::invalid_argument("Invalid API key");
    }
    return required_field<std::string>(json, "value");
}

SaveR2StorageRequest parse_save_r2_storage_request(const nlohmann::json& json) {
    if (!json.is_object() || json.size() != 4) {
        throw std::invalid_argument("Invalid R2 credentials");
    }
    return {
        .display_name = required_field<std::string>(json, "display_name"),
        .url = required_field<std::string>(json, "url"),
        .access_key_id = required_field<std::string>(json, "access_key_id"),
        .secret_key = settings_nullable_string(json, "secret_key"),
    };
}

} // namespace cha
