#include "agents/config.h"

#include "util/text.h"
#include "util/utf8_path.h"

#include <toml++/toml.hpp>

#include <cmath>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cha {
namespace {

enum class ConfigLayer { definition, forum_defaults, member_override };

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
    std::vector<std::string> tags;
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
    const auto value = read_optional<std::string>(table, path, "mode", "string");
    if (!value) {
        return std::nullopt;
    }
    if (*value == "net") {
        return Mode::net;
    }
    if (*value == "test") {
        return Mode::test;
    }
    throw std::runtime_error("Config file '" + utf8_path(path)
        + "' has unsupported mode '" + *value + "'");
}

std::optional<ReasoningFormat> read_reasoning_format(
    const toml::table& table,
    const std::filesystem::path& path) {
    const auto value = read_optional<std::string>(table, path, "reasoning_format", "string");
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
    throw std::runtime_error("Config file '" + utf8_path(path)
        + "' has unsupported reasoning_format '" + *value + "'");
}

std::vector<std::string> read_tags(
    const toml::table& table,
    const std::filesystem::path& path) {
    if (!table.contains("tags")) {
        return {};
    }
    const toml::array* array = table["tags"].as_array();
    if (!array) {
        throw std::runtime_error("Config file '" + utf8_path(path)
            + "' requires array 'tags' value");
    }
    std::vector<std::string> tags;
    std::unordered_set<std::string> seen;
    for (const toml::node& node : *array) {
        const auto value = node.value<std::string>();
        if (!value) {
            throw std::runtime_error("Config file '" + utf8_path(path)
                + "' requires string elements in 'tags'");
        }
        const std::string tag(trim_view(*value));
        if (tag.empty() || tag.find_first_of("\n\r") != std::string::npos) {
            throw std::runtime_error("Config file '" + utf8_path(path)
                + "' has invalid tag '" + tag + "'");
        }
        for (const unsigned char character : tag) {
            if (character < 0x20 || character == 0x7f) {
                throw std::runtime_error("Config file '" + utf8_path(path)
                    + "' has invalid tag '" + tag + "'");
            }
        }
        if (!seen.insert(fold_ascii(tag)).second) {
            throw std::runtime_error("Config file '" + utf8_path(path)
                + "' has duplicate tag '" + tag + "'");
        }
        tags.push_back(tag);
    }
    return tags;
}

ParsedConfig parse_config(const std::filesystem::path& path, ConfigLayer layer) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to read config file '" + utf8_path(path) + "'");
    }
    const toml::table table = toml::parse(file, utf8_path(path));
    if (table.contains("id") || table.contains("name")) {
        throw std::runtime_error("Config file '" + utf8_path(path)
            + "' uses removed identity field '" + (table.contains("id") ? "id" : "name")
            + "'; use the directory name and 'display_name'");
    }
    const bool definition = layer == ConfigLayer::definition;
    if (!definition && table.contains("display_name")) {
        throw std::runtime_error("Config file '" + utf8_path(path)
            + "' cannot define definition-only field 'display_name'");
    }
    if (!definition && table.contains("tags")) {
        throw std::runtime_error("Config file '" + utf8_path(path)
            + "' cannot define definition-only field 'tags'");
    }
    const auto display_name = read_optional<std::string>(table, path, "display_name", "string");
    if (definition && (!display_name || display_name->empty())) {
        throw std::runtime_error("Character config file '" + utf8_path(path)
            + "' requires a non-empty string 'display_name' value");
    }
    return {
        .patch = {
            .display_name = display_name,
            .host = read_optional<std::string>(table, path, "host", "string"),
            .port = read_optional<int>(table, path, "port", "integer"),
            .mode = read_mode(table, path),
            .model = read_optional<std::string>(table, path, "model", "string"),
            .stream = read_optional<bool>(table, path, "stream", "boolean"),
            .temperature = read_optional<double>(table, path, "temperature", "numeric"),
            .api_key = read_optional<std::string>(table, path, "api_key", "string"),
            .api_key_env = read_optional<std::string>(table, path, "api_key_env", "string"),
            .reasoning_effort = read_optional<std::string>(table, path, "reasoning_effort", "string"),
            .reasoning_format = read_reasoning_format(table, path),
            .https = read_optional<bool>(table, path, "https", "boolean"),
        },
        .prompt_variables = template_scope_from_toml(table, prompt_scope_table, utf8_path(path)),
        .tags = definition ? read_tags(table, path) : std::vector<std::string>{},
    };
}

template<typename Value>
void override_with(std::optional<Value>& effective, const std::optional<Value>& upper) {
    if (upper) {
        effective = upper;
    }
}

void overlay(ConfigPatch& effective, const ConfigPatch& upper) {
    override_with(effective.host, upper.host);
    override_with(effective.port, upper.port);
    override_with(effective.mode, upper.mode);
    override_with(effective.model, upper.model);
    override_with(effective.stream, upper.stream);
    override_with(effective.temperature, upper.temperature);
    override_with(effective.api_key, upper.api_key);
    override_with(effective.api_key_env, upper.api_key_env);
    override_with(effective.reasoning_effort, upper.reasoning_effort);
    override_with(effective.reasoning_format, upper.reasoning_format);
    override_with(effective.https, upper.https);
}

void overlay(TemplateScope& effective, TemplateScope upper) {
    for (auto& [key, value] : upper) {
        effective.insert_or_assign(std::move(key), std::move(value));
    }
}

void validate_effective(
    const ConfigPatch& effective,
    const std::filesystem::path& definition,
    const std::filesystem::path& port_source,
    const std::filesystem::path& temperature_source) {
    if (!effective.host || !effective.port) {
        throw std::runtime_error("Effective config for character file '"
            + utf8_path(definition) + "' requires string 'host' and integer 'port' values");
    }
    if (*effective.port < 1 || *effective.port > 65535) {
        throw std::runtime_error("Config file '"
            + utf8_path(port_source) + "' requires 'port' between 1 and 65535");
    }
    if (effective.temperature
        && (!std::isfinite(*effective.temperature)
            || *effective.temperature < 0.0
            || *effective.temperature > 2.0)) {
        throw std::runtime_error("Config file '" + utf8_path(temperature_source)
            + "' requires 'temperature' between 0 and 2");
    }
}

} // namespace

CharacterDefinitionMetadata load_character_definition_metadata(const std::filesystem::path& definition_path) {
    const ParsedConfig definition = parse_config(definition_path, ConfigLayer::definition);
    return {
        .id = utf8_path(definition_path.parent_path().filename()),
        .display_name = *definition.patch.display_name,
        .tags = definition.tags,
    };
}

LoadedConfig load_config(const CharacterConfigPaths& paths) {
    const ParsedConfig definition = parse_config(paths.definition, ConfigLayer::definition);
    ConfigPatch effective = definition.patch;
    TemplateScope prompt_variables = definition.prompt_variables;
    std::filesystem::path port_source = paths.definition;
    std::filesystem::path temperature_source = paths.definition;
    const auto apply = [&](const std::optional<std::filesystem::path>& path, ConfigLayer layer) {
        if (!path) {
            return;
        }
        const ParsedConfig parsed = parse_config(*path, layer);
        if (parsed.patch.port) port_source = *path;
        if (parsed.patch.temperature) temperature_source = *path;
        overlay(effective, parsed.patch);
        overlay(prompt_variables, parsed.prompt_variables);
    };
    apply(paths.forum_defaults, ConfigLayer::forum_defaults);
    apply(paths.member_override, ConfigLayer::member_override);
    validate_effective(effective, paths.definition, port_source, temperature_source);
    Config config;
    config.id = utf8_path(paths.definition.parent_path().filename());
    config.display_name = *definition.patch.display_name;
    config.host = *effective.host;
    config.port = *effective.port;
    if (effective.mode) {
        config.mode = *effective.mode;
    }
    if (effective.model) {
        config.model = *effective.model;
    }
    if (effective.stream) {
        config.stream = *effective.stream;
    }
    if (effective.temperature) {
        config.temperature = *effective.temperature;
    }
    if (effective.api_key) {
        config.api_key = *effective.api_key;
    }
    if (effective.api_key_env) {
        config.api_key_env = *effective.api_key_env;
    }
    if (effective.reasoning_effort) {
        config.reasoning_effort = *effective.reasoning_effort;
    }
    if (effective.reasoning_format) {
        config.reasoning_format = *effective.reasoning_format;
    }
    if (effective.https) {
        config.https = *effective.https;
    }
    return {.config = std::move(config), .prompt_variables = std::move(prompt_variables)};
}

} // namespace cha
