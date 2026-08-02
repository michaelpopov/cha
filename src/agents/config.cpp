#include "agents/config.h"

#include "util/utf8_path.h"

#include <toml++/toml.hpp>

#include <cmath>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace cha {
namespace {

struct ConfigPatch {
    std::optional<std::string> display_name;
    std::optional<std::string> host;
    std::optional<int> port;
    std::optional<Mode> mode;
    std::optional<std::string> model;
    std::optional<bool> stream;
    std::optional<double> temperature;
    std::optional<std::string> api_key;
    std::optional<std::string> api_key_env;
    std::optional<std::string> reasoning_effort;
    std::optional<ReasoningFormat> reasoning_format;
    std::optional<bool> https;
};

struct ParsedConfig {
    ConfigPatch patch;
    TemplateScope prompt_variables;
};

template<typename Value>
std::optional<Value> read_optional(
    const toml::table& table,
    const std::filesystem::path& path,
    std::string_view key,
    std::string_view type) {
    if (!table.contains(key)) {
        return std::nullopt;
    }
    const std::optional<Value> value = table[key].value<Value>();
    if (!value) {
        throw std::runtime_error(
            "Config file '" + utf8_path(path) + "' requires "
            + std::string(type) + " '" + std::string(key) + "' value");
    }
    return value;
}

std::optional<Mode> read_mode(
    const toml::table& table,
    const std::filesystem::path& path) {
    const std::optional<std::string> value =
        read_optional<std::string>(table, path, "mode", "string");
    if (!value) {
        return std::nullopt;
    }
    if (*value == "net") {
        return Mode::net;
    }
    if (*value == "test") {
        return Mode::test;
    }
    throw std::runtime_error(
        "Config file '" + utf8_path(path)
        + "' has unsupported mode '" + *value + "'");
}

std::optional<ReasoningFormat> read_reasoning_format(
    const toml::table& table,
    const std::filesystem::path& path) {
    const std::optional<std::string> value =
        read_optional<std::string>(
            table, path, "reasoning_format", "string");
    if (!value) {
        return std::nullopt;
    }
    if (*value == "auto") {
        return ReasoningFormat::automatic;
    }
    if (*value == "none") {
        return ReasoningFormat::none;
    }
    if (*value == "reasoning_content") {
        return ReasoningFormat::reasoning_content;
    }
    if (*value == "reasoning") {
        return ReasoningFormat::reasoning;
    }
    throw std::runtime_error(
        "Config file '" + utf8_path(path)
        + "' has unsupported reasoning_format '" + *value + "'");
}

ParsedConfig parse_config(
    const std::filesystem::path& path,
    bool base) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error(
            "Failed to read config file '" + utf8_path(path) + "'");
    }
    const toml::table table = toml::parse(file, utf8_path(path));
    if (table.contains("id") || table.contains("name")) {
        throw std::runtime_error(
            "Config file '" + utf8_path(path)
            + "' uses removed identity field '"
            + (table.contains("id") ? "id" : "name")
            + "'; use the directory name and 'display_name'");
    }
    if (base && table.contains("display_name")) {
        throw std::runtime_error(
            "Base config file '" + utf8_path(path)
            + "' cannot define character identity 'display_name'");
    }

    return {
        .patch = {
            .display_name = read_optional<std::string>(
                table, path, "display_name", "string"),
            .host = read_optional<std::string>(table, path, "host", "string"),
            .port = read_optional<int>(table, path, "port", "integer"),
            .mode = read_mode(table, path),
            .model = read_optional<std::string>(table, path, "model", "string"),
            .stream = read_optional<bool>(table, path, "stream", "boolean"),
            .temperature =
                read_optional<double>(table, path, "temperature", "numeric"),
            .api_key =
                read_optional<std::string>(table, path, "api_key", "string"),
            .api_key_env =
                read_optional<std::string>(table, path, "api_key_env", "string"),
            .reasoning_effort = read_optional<std::string>(
                table, path, "reasoning_effort", "string"),
            .reasoning_format = read_reasoning_format(table, path),
            .https = read_optional<bool>(table, path, "https", "boolean"),
        },
        .prompt_variables = template_scope_from_toml(
            table, prompt_scope_table, utf8_path(path)),
    };
}

template<typename Value>
void override_with(
    std::optional<Value>& effective,
    const std::optional<Value>& character) {
    if (character) {
        effective = character;
    }
}

void overlay(ConfigPatch& effective, const ConfigPatch& character) {
    override_with(effective.host, character.host);
    override_with(effective.port, character.port);
    override_with(effective.mode, character.mode);
    override_with(effective.model, character.model);
    override_with(effective.stream, character.stream);
    override_with(effective.temperature, character.temperature);
    override_with(effective.api_key, character.api_key);
    override_with(effective.api_key_env, character.api_key_env);
    override_with(effective.reasoning_effort, character.reasoning_effort);
    override_with(effective.reasoning_format, character.reasoning_format);
    override_with(effective.https, character.https);
}

void overlay(TemplateScope& effective, TemplateScope character) {
    for (auto& [key, value] : character) {
        effective.insert_or_assign(std::move(key), std::move(value));
    }
}

Config build_config(
    ConfigPatch effective,
    const ConfigPatch& character,
    const std::filesystem::path& character_path,
    const std::optional<std::filesystem::path>& base_path) {
    if (!character.display_name || character.display_name->empty()) {
        throw std::runtime_error(
            "Character config file '" + utf8_path(character_path)
            + "' requires a non-empty string 'display_name' value");
    }
    if (!effective.host || !effective.port) {
        throw std::runtime_error(
            "Effective config for character file '" + utf8_path(character_path)
            + "' requires string 'host' and integer 'port' values");
    }
    if (*effective.port < 1 || *effective.port > 65535) {
        const std::filesystem::path& source =
            character.port || !base_path ? character_path : *base_path;
        throw std::runtime_error(
            "Config file '" + utf8_path(source)
            + "' requires 'port' between 1 and 65535");
    }
    if (effective.temperature
        && (!std::isfinite(*effective.temperature)
            || *effective.temperature < 0.0
            || *effective.temperature > 2.0)) {
        const std::filesystem::path& source =
            character.temperature || !base_path ? character_path : *base_path;
        throw std::runtime_error(
            "Config file '" + utf8_path(source)
            + "' requires 'temperature' between 0 and 2");
    }

    Config config;
    config.id = utf8_path(character_path.parent_path().filename());
    config.name = *character.display_name;
    config.host = *effective.host;
    config.port = *effective.port;
    if (effective.mode) config.mode = *effective.mode;
    if (effective.model) config.model = *effective.model;
    if (effective.stream) config.stream = *effective.stream;
    if (effective.temperature) config.temperature = *effective.temperature;
    if (effective.api_key) config.api_key = *effective.api_key;
    if (effective.api_key_env) config.api_key_env = *effective.api_key_env;
    if (effective.reasoning_effort) {
        config.reasoning_effort = *effective.reasoning_effort;
    }
    if (effective.reasoning_format) {
        config.reasoning_format = *effective.reasoning_format;
    }
    if (effective.https) config.https = *effective.https;
    return config;
}

} // namespace

LoadedConfig load_config(
    const std::filesystem::path& character_path,
    std::optional<std::filesystem::path> base_path) {
    ParsedConfig effective;
    if (base_path) {
        effective = parse_config(*base_path, true);
    }
    ParsedConfig character = parse_config(character_path, false);
    overlay(effective.patch, character.patch);
    overlay(effective.prompt_variables, std::move(character.prompt_variables));
    return {
        .config = build_config(
            std::move(effective.patch), character.patch, character_path, base_path),
        .prompt_variables = std::move(effective.prompt_variables),
    };
}

} // namespace cha
