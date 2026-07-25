#include "agents/config.h"

#include <toml++/toml.hpp>

#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace cha {
namespace {

struct ConfigPatch {
    std::optional<std::string> id;
    std::optional<std::string> name;
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
            "Config file '" + path.string() + "' requires "
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
        "Config file '" + path.string()
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
        "Config file '" + path.string()
        + "' has unsupported reasoning_format '" + *value + "'");
}

ConfigPatch parse_config(
    const std::filesystem::path& path,
    bool base) {
    const toml::table table = toml::parse_file(path.string());
    if (base && (table.contains("id") || table.contains("name"))) {
        throw std::runtime_error(
            "Base config file '" + path.string()
            + "' cannot define persona 'id' or 'name'");
    }

    return {
        .id = read_optional<std::string>(table, path, "id", "string"),
        .name = read_optional<std::string>(table, path, "name", "string"),
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
    };
}

template<typename Value>
void override_with(
    std::optional<Value>& effective,
    const std::optional<Value>& persona) {
    if (persona) {
        effective = persona;
    }
}

void overlay(ConfigPatch& effective, const ConfigPatch& persona) {
    override_with(effective.id, persona.id);
    override_with(effective.name, persona.name);
    override_with(effective.host, persona.host);
    override_with(effective.port, persona.port);
    override_with(effective.mode, persona.mode);
    override_with(effective.model, persona.model);
    override_with(effective.stream, persona.stream);
    override_with(effective.temperature, persona.temperature);
    override_with(effective.api_key, persona.api_key);
    override_with(effective.api_key_env, persona.api_key_env);
    override_with(effective.reasoning_effort, persona.reasoning_effort);
    override_with(effective.reasoning_format, persona.reasoning_format);
    override_with(effective.https, persona.https);
}

Config build_config(
    ConfigPatch effective,
    const ConfigPatch& persona,
    const std::filesystem::path& persona_path,
    const std::optional<std::filesystem::path>& base_path) {
    if (!persona.id || !persona.name
        || persona.id->empty() || persona.name->empty()) {
        throw std::runtime_error(
            "Persona config file '" + persona_path.string()
            + "' requires non-empty string 'id' and 'name' values");
    }
    if (!effective.host || !effective.port) {
        throw std::runtime_error(
            "Effective config for persona file '" + persona_path.string()
            + "' requires string 'host' and integer 'port' values");
    }
    if (*effective.port < 1 || *effective.port > 65535) {
        const std::filesystem::path& source =
            persona.port || !base_path ? persona_path : *base_path;
        throw std::runtime_error(
            "Config file '" + source.string()
            + "' requires 'port' between 1 and 65535");
    }
    if (effective.temperature
        && (!std::isfinite(*effective.temperature)
            || *effective.temperature < 0.0
            || *effective.temperature > 2.0)) {
        const std::filesystem::path& source =
            persona.temperature || !base_path ? persona_path : *base_path;
        throw std::runtime_error(
            "Config file '" + source.string()
            + "' requires 'temperature' between 0 and 2");
    }

    Config config;
    config.id = *persona.id;
    config.name = *persona.name;
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

Config load_config(
    const std::filesystem::path& persona_path,
    std::optional<std::filesystem::path> base_path) {
    ConfigPatch effective;
    if (base_path) {
        effective = parse_config(*base_path, true);
    }
    const ConfigPatch persona = parse_config(persona_path, false);
    overlay(effective, persona);
    return build_config(
        std::move(effective), persona, persona_path, base_path);
}

} // namespace cha
