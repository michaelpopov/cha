#include "agents/character_config.h"

#include "util/text.h"
#include "util/public_name.h"
#include "util/path_name.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cha {
namespace {

enum class ConfigLayer { application, definition, forum_defaults, member_override };

struct ParsedConfig {
    std::optional<std::string> display_name;
    ProviderConfig provider;
    TemplateScope prompt_variables;
    std::vector<std::string> tags;
    std::optional<std::string> description;
    CharacterAppearance appearance;
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

// Appearance fields are closed vocabularies, so a misspelling is reported with
// the words that would have worked rather than silently leaving the default.
std::size_t read_choice(
    const toml::table& table,
    const std::filesystem::path& path,
    std::string_view key,
    std::span<const std::string_view> allowed,
    std::size_t fallback) {
    const auto value = read_optional<std::string>(table, path, key, "string");
    if (!value) return fallback;
    const auto found = std::ranges::find(allowed, *value);
    if (found == allowed.end()) {
        std::string names;
        for (const std::string_view name : allowed) {
            if (!names.empty()) names += ", ";
            names += name;
        }
        throw std::runtime_error("Config file '" + utf8_path(path)
            + "' [appearance] has unsupported " + std::string(key) + " '" + *value
            + "'; expected one of " + names);
    }
    return static_cast<std::size_t>(found - allowed.begin());
}

CharacterAppearance read_appearance(
    const toml::table& table,
    const std::filesystem::path& path) {
    const toml::node* node = table.get("appearance");
    if (!node) return {};
    const toml::table* appearance = node->as_table();
    if (!appearance) {
        throw std::runtime_error("Config file '" + utf8_path(path)
            + "' requires 'appearance' to be a table");
    }
    static constexpr std::string_view fields[]{"font", "style", "weight", "size"};
    for (const auto& [key, value] : *appearance) {
        (void)value;
        if (std::ranges::find(fields, key.str()) == std::end(fields)) {
            throw std::runtime_error("Config file '" + utf8_path(path)
                + "' [appearance] has unsupported field '" + std::string(key.str()) + "'");
        }
    }
    static constexpr std::string_view fonts[]{"sans", "serif", "mono"};
    static constexpr std::string_view slants[]{"normal", "italic"};
    static constexpr std::string_view weights[]{"normal", "bold"};
    static constexpr std::string_view scales[]{"small", "normal", "large"};
    return {
        .font = static_cast<CharacterFont>(read_choice(*appearance, path, "font", fonts, 0)),
        .style = static_cast<CharacterSlant>(read_choice(*appearance, path, "style", slants, 0)),
        .weight = static_cast<CharacterWeight>(read_choice(*appearance, path, "weight", weights, 0)),
        .size = static_cast<CharacterScale>(read_choice(*appearance, path, "size", scales, 1)),
    };
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

std::optional<ProviderApi> read_provider_api(
    const toml::table& table,
    const std::filesystem::path& path) {
    const auto value = read_optional<std::string>(table, path, "api", "string");
    if (!value) {
        return std::nullopt;
    }
    if (*value == "chat_completions") {
        return ProviderApi::chat_completions;
    }
    if (*value == "responses") {
        return ProviderApi::responses;
    }
    throw std::runtime_error("Config file '" + utf8_path(path)
        + "' has unsupported api '" + *value
        + "'; expected one of chat_completions, responses");
}

std::optional<WebSearchMode> read_web_search_mode(
    const toml::table& table,
    const std::filesystem::path& path) {
    const auto value = read_optional<std::string>(table, path, "web_search", "string");
    if (!value) {
        return std::nullopt;
    }
    if (*value == "off") {
        return WebSearchMode::off;
    }
    if (*value == "auto") {
        return WebSearchMode::automatic;
    }
    if (*value == "required") {
        return WebSearchMode::required;
    }
    throw std::runtime_error("Config file '" + utf8_path(path)
        + "' has unsupported web_search '" + *value
        + "'; expected one of off, auto, required");
}

bool is_valid_base_path(std::string_view base_path) {
    return base_path.empty()
        || (base_path.starts_with('/')
            && !base_path.ends_with('/')
            && base_path.find_first_of("?# \t\r\n") == std::string_view::npos);
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
        if (tag.empty()) {
            throw std::runtime_error("Config file '" + utf8_path(path)
                + "' has empty element in 'tags'");
        }
        for (const unsigned char character : tag) {
            if (character < 0x20 || character == 0x7f) {
                throw std::runtime_error("Config file '" + utf8_path(path)
                    + "' has control character in 'tags'");
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
    if (layer == ConfigLayer::application) {
        throw std::logic_error("application provider must be parsed from workspace.toml");
    }
    if (!definition && table.contains("display_name")) {
        throw std::runtime_error("Config file '" + utf8_path(path)
            + "' cannot define definition-only field 'display_name'");
    }
    if (!definition && table.contains("tags")) {
        throw std::runtime_error("Config file '" + utf8_path(path)
            + "' cannot define definition-only field 'tags'");
    }
    if (!definition && table.contains("description")) {
        throw std::runtime_error("Config file '" + utf8_path(path)
            + "' cannot define definition-only field 'description'");
    }
    if (!definition && table.contains("appearance")) {
        throw std::runtime_error("Config file '" + utf8_path(path)
            + "' cannot define definition-only field 'appearance'");
    }
    const auto display_name = read_optional<std::string>(table, path, "display_name", "string");
    if (definition && (!display_name || display_name->empty())) {
        throw std::runtime_error("Character config file '" + utf8_path(path)
            + "' requires a non-empty string 'display_name' value");
    }
    const auto description = read_optional<std::string>(table, path, "description", "string");
    if (description) validate_description(*description, "Character", path);
    return {
        .display_name = display_name,
        .provider = {
            .source = path,
            .host = read_optional<std::string>(table, path, "host", "string"),
            .port = read_optional<int>(table, path, "port", "integer"),
            .base_path = read_optional<std::string>(table, path, "base_path", "string"),
            .mode = read_mode(table, path),
            .model = read_optional<std::string>(table, path, "model", "string"),
            .stream = read_optional<bool>(table, path, "stream", "boolean"),
            .temperature = read_optional<double>(table, path, "temperature", "numeric"),
            .api_key = read_optional<std::string>(table, path, "api_key", "string"),
            .api_key_env = read_optional<std::string>(table, path, "api_key_env", "string"),
            .reasoning_effort = read_optional<std::string>(table, path, "reasoning_effort", "string"),
            .reasoning_format = read_reasoning_format(table, path),
            .https = read_optional<bool>(table, path, "https", "boolean"),
            .api = read_provider_api(table, path),
            .web_search = read_web_search_mode(table, path),
        },
        .prompt_variables = template_scope_from_toml(table, prompt_scope_table, utf8_path(path)),
        .tags = definition ? read_tags(table, path) : std::vector<std::string>{},
        .description = description,
        .appearance = definition ? read_appearance(table, path) : CharacterAppearance{},
    };
}

template<typename Value>
void override_with(std::optional<Value>& effective, const std::optional<Value>& upper) {
    if (upper) {
        effective = upper;
    }
}

// A present upper value replaces the lower one, field by field. 'source' is
// deliberately not overlaid: no single path describes a mixed configuration, so
// per-value attribution is tracked by the caller where it is needed.
void overlay_provider(ProviderConfig& effective, const ProviderConfig& upper) {
    override_with(effective.host, upper.host);
    override_with(effective.port, upper.port);
    override_with(effective.base_path, upper.base_path);
    override_with(effective.mode, upper.mode);
    override_with(effective.model, upper.model);
    override_with(effective.stream, upper.stream);
    override_with(effective.temperature, upper.temperature);
    override_with(effective.api_key, upper.api_key);
    override_with(effective.api_key_env, upper.api_key_env);
    override_with(effective.reasoning_effort, upper.reasoning_effort);
    override_with(effective.reasoning_format, upper.reasoning_format);
    override_with(effective.https, upper.https);
    override_with(effective.api, upper.api);
    override_with(effective.web_search, upper.web_search);
}

void overlay(TemplateScope& effective, TemplateScope upper) {
    for (auto& [key, value] : upper) {
        effective.insert_or_assign(std::move(key), std::move(value));
    }
}

void validate_effective(
    const ProviderConfig& effective,
    const std::filesystem::path& definition,
    const std::filesystem::path& port_source,
    const std::filesystem::path& temperature_source,
    const std::filesystem::path& web_search_source,
    const std::filesystem::path& base_path_source) {
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
    if (effective.base_path && !is_valid_base_path(*effective.base_path)) {
        throw std::runtime_error("Config file '" + utf8_path(base_path_source)
            + "' requires 'base_path' to start with '/', not end with '/', and contain no query, fragment, or whitespace");
    }
    const WebSearchMode search =
        effective.web_search.value_or(default_web_search_mode);
    if (search != WebSearchMode::off) {
        const ProviderApi api = effective.api.value_or(default_provider_api);
        if (api != ProviderApi::responses) {
            throw std::runtime_error("Config file '" + utf8_path(web_search_source)
                + "' enables web_search but web search requires api = \"responses\"");
        }
    }
}

} // namespace

CharacterMetadata load_character_metadata(
    const std::filesystem::path& definition_path) {
    const ParsedConfig definition = parse_config(definition_path, ConfigLayer::definition);
    return {
        .id = utf8_path(definition_path.parent_path().filename()),
        .display_name = *definition.display_name,
        .description = definition.description,
        .tags = definition.tags,
        .appearance = definition.appearance,
    };
}

ModelBackendConfig make_backend_config(const ProviderConfig& effective) {
    if (!effective.host || !effective.port) {
        throw std::invalid_argument(
            "Provider configuration requires host and port before materialization");
    }
    ModelBackendConfig backend;
    backend.host = *effective.host;
    backend.port = *effective.port;
    if (effective.base_path) backend.base_path = *effective.base_path;
    if (effective.mode) backend.mode = *effective.mode;
    if (effective.model) backend.model = *effective.model;
    if (effective.stream) backend.stream = *effective.stream;
    if (effective.temperature) backend.temperature = *effective.temperature;
    if (effective.api_key) backend.api_key = *effective.api_key;
    if (effective.api_key_env) backend.api_key_env = *effective.api_key_env;
    if (effective.reasoning_effort) backend.reasoning_effort = *effective.reasoning_effort;
    if (effective.reasoning_format) backend.reasoning_format = *effective.reasoning_format;
    if (effective.https) backend.https = *effective.https;
    if (effective.api) backend.api = *effective.api;
    if (effective.web_search) backend.web_search = *effective.web_search;
    return backend;
}

ProviderConfig load_provider_config(const std::filesystem::path& workspace_config_path) {
    std::ifstream file(workspace_config_path, std::ios::binary);
    if (!file) throw std::runtime_error("Failed to read workspace config '" + utf8_path(workspace_config_path) + "'");
    const toml::table root = toml::parse(file, utf8_path(workspace_config_path));
    return load_provider_config(root, workspace_config_path);
}

ProviderConfig load_provider_config(
    const toml::table& workspace_config,
    const std::filesystem::path& workspace_config_path) {
    const toml::table* table = workspace_config["provider"].as_table();
    if (!table) throw std::runtime_error("Workspace config '" + utf8_path(workspace_config_path) + "' requires a [provider] table");
    static const std::unordered_set<std::string_view> allowed{
        "host", "port", "base_path", "mode", "model", "stream", "temperature",
        "api_key_env", "reasoning_effort", "reasoning_format", "https",
        "api", "web_search"};
    for (const auto& [key, value] : *table) {
        (void)value;
        const std::string_view name = key.str();
        if (!allowed.contains(name)) {
            throw std::runtime_error("Workspace config '" + utf8_path(workspace_config_path)
                + "' [provider] has unsupported field '" + std::string(name) + "'");
        }
    }
    ProviderConfig provider{.source = workspace_config_path,
        .host = read_optional<std::string>(*table, workspace_config_path, "host", "string"),
        .port = read_optional<int>(*table, workspace_config_path, "port", "integer"),
        .base_path = read_optional<std::string>(*table, workspace_config_path, "base_path", "string"),
        .mode = read_mode(*table, workspace_config_path),
        .model = read_optional<std::string>(*table, workspace_config_path, "model", "string"),
        .stream = read_optional<bool>(*table, workspace_config_path, "stream", "boolean"),
        .temperature = read_optional<double>(*table, workspace_config_path, "temperature", "numeric"),
        .api_key_env = read_optional<std::string>(*table, workspace_config_path, "api_key_env", "string"),
        .reasoning_effort = read_optional<std::string>(*table, workspace_config_path, "reasoning_effort", "string"),
        .reasoning_format = read_reasoning_format(*table, workspace_config_path),
        .https = read_optional<bool>(*table, workspace_config_path, "https", "boolean"),
        .api = read_provider_api(*table, workspace_config_path),
        .web_search = read_web_search_mode(*table, workspace_config_path)};
    if (!provider.host || provider.host->empty() || !provider.port) {
        throw std::runtime_error("Workspace config '" + utf8_path(workspace_config_path)
            + "' [provider] requires non-empty string 'host' and integer 'port'");
    }
    if (*provider.port < 1 || *provider.port > 65535) {
        throw std::runtime_error("Workspace config '" + utf8_path(workspace_config_path)
            + "' [provider] requires 'port' between 1 and 65535");
    }
    if (provider.temperature && (!std::isfinite(*provider.temperature)
        || *provider.temperature < 0.0 || *provider.temperature > 2.0)) {
        throw std::runtime_error("Workspace config '" + utf8_path(workspace_config_path)
            + "' [provider] requires 'temperature' between 0 and 2");
    }
    if (provider.base_path && !is_valid_base_path(*provider.base_path)) {
        throw std::runtime_error("Workspace config '" + utf8_path(workspace_config_path)
            + "' [provider] requires 'base_path' to start with '/', not end with '/', and contain no query, fragment, or whitespace");
    }
    return provider;
}

LoadedCharacterConfig load_character_config(const CharacterConfigPaths& paths) {
    const ParsedConfig definition = parse_config(paths.definition, ConfigLayer::definition);
    ProviderConfig effective;
    TemplateScope prompt_variables = definition.prompt_variables;
    std::filesystem::path port_source = paths.definition;
    std::filesystem::path temperature_source = paths.definition;
    std::filesystem::path web_search_source = paths.definition;
    std::filesystem::path base_path_source = paths.definition;
    if (paths.application_provider) {
        overlay_provider(effective, *paths.application_provider);
        if (effective.port) port_source = paths.application_provider->source;
        if (effective.temperature) temperature_source = paths.application_provider->source;
        if (effective.web_search) web_search_source = paths.application_provider->source;
        if (effective.base_path) base_path_source = paths.application_provider->source;
    }
    if (definition.provider.port) port_source = paths.definition;
    if (definition.provider.temperature) temperature_source = paths.definition;
    if (definition.provider.web_search) web_search_source = paths.definition;
    if (definition.provider.base_path) base_path_source = paths.definition;
    overlay_provider(effective, definition.provider);
    const auto apply = [&](const std::optional<std::filesystem::path>& path, ConfigLayer layer) {
        if (!path) {
            return;
        }
        const ParsedConfig parsed = parse_config(*path, layer);
        if (parsed.provider.port) port_source = *path;
        if (parsed.provider.temperature) temperature_source = *path;
        if (parsed.provider.web_search) web_search_source = *path;
        if (parsed.provider.base_path) base_path_source = *path;
        overlay_provider(effective, parsed.provider);
        overlay(prompt_variables, parsed.prompt_variables);
    };
    apply(paths.forum_defaults, ConfigLayer::forum_defaults);
    apply(paths.member_override, ConfigLayer::member_override);
    validate_effective(
        effective, paths.definition, port_source, temperature_source, web_search_source,
        base_path_source);
    CharacterMetadata character{
        .id = utf8_path(paths.definition.parent_path().filename()),
        .display_name = *definition.display_name,
        .description = definition.description,
        .tags = definition.tags,
        .appearance = definition.appearance,
    };
    return {
        .character = std::move(character),
        .backend = make_backend_config(effective),
        .prompt_variables = std::move(prompt_variables),
    };
}

} // namespace cha
