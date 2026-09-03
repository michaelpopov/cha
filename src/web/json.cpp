#include "web/json.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>

namespace cha::web {
namespace {

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(
        result.begin(),
        result.end(),
        result.begin(),
        [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return result;
}

std::string_view trim_ascii_whitespace(std::string_view value) {
    while (!value.empty()
           && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.remove_prefix(1);
    }
    while (!value.empty()
           && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.remove_suffix(1);
    }
    return value;
}

const std::string& required_string(
    const nlohmann::json& json,
    std::string_view key) {
    const std::string name(key);
    if (!json.is_object() || !json.contains(name) || !json.at(name).is_string()) {
        throw std::invalid_argument("Invalid web command");
    }
    return json.at(name).get_ref<const std::string&>();
}

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

bool is_json_content_type(std::string_view content_type) {
    const auto semicolon = content_type.find(';');
    return lower(trim_ascii_whitespace(content_type.substr(0, semicolon)))
        == "application/json";
}

nlohmann::json parse_json_body(
    std::string_view body,
    std::size_t maximum_bytes) {
    if (body.size() > maximum_bytes) {
        throw std::length_error("Web request body is too large");
    }
    try {
        return nlohmann::json::parse(body.begin(), body.end());
    } catch (const nlohmann::json::exception&) {
        throw std::invalid_argument("Invalid JSON request body");
    }
}

RawCommand parse_input_command(const nlohmann::json& json) {
    exact_keys(json, {"text"});
    return {required_string(json, "text")};
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

std::string parse_rename_session_label(const nlohmann::json& json) {
    exact_keys(json, {"label"});
    return required_string(json, "label");
}

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
    if (*value == "off") return WebSearchMode::off;
    if (*value == "auto") return WebSearchMode::automatic;
    if (*value == "required") return WebSearchMode::required;
    throw std::invalid_argument("Invalid web command");
}

CharacterSettingsUpdate parse_character_settings_update(const nlohmann::json& json) {
    exact_keys(json, {"provider", "style", "reasoning_effort", "web_search"});
    return {
        .provider = required_string(json, "provider"),
        .style = nullable_string(json, "style"),
        .reasoning_effort = nullable_reasoning_effort(json),
        .web_search = nullable_web_search(json),
    };
}

void parse_empty_object(const nlohmann::json& json) {
    exact_keys(json, {});
}

} // namespace cha::web
