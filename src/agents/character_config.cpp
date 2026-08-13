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

enum class ConfigLayer { definition, forum_defaults, member_override };

struct ParsedConfig {
    std::optional<std::string> display_name;
    std::optional<std::string> provider_name;
    std::optional<std::string> style_name;
    TemplateScope prompt_variables;
    std::vector<std::string> tags;
    std::optional<std::string> description;
};

// The only fields a provider config may set. Every other configuration file
// selects a provider by ID instead of repeating any of these.
constexpr std::string_view provider_setting_names[]{
    "host", "port", "base_path", "mode", "model", "stream", "temperature",
    "api_key", "api_key_env", "reasoning_effort", "reasoning_format", "https",
    "api", "web_search"};

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
        throw std::runtime_error("Style config '" + utf8_path(path)
            + "' has unsupported " + std::string(key) + " '" + *value
            + "'; expected one of " + names);
    }
    return static_cast<std::size_t>(found - allowed.begin());
}

CharacterAppearance read_style_config(
    const toml::table& table,
    const std::filesystem::path& path) {
    static constexpr std::string_view fields[]{"font", "style", "weight", "size"};
    for (const auto& [key, value] : table) {
        (void)value;
        if (std::ranges::find(fields, key.str()) == std::end(fields)) {
            throw std::runtime_error("Style config '" + utf8_path(path)
                + "' has unsupported field '" + std::string(key.str()) + "'");
        }
    }
    static constexpr std::string_view fonts[]{"sans", "serif", "mono"};
    static constexpr std::string_view slants[]{"normal", "italic"};
    static constexpr std::string_view weights[]{"normal", "bold"};
    static constexpr std::string_view scales[]{"small", "normal", "large"};
    return {
        .font = static_cast<CharacterFont>(read_choice(table, path, "font", fonts, 0)),
        .style = static_cast<CharacterSlant>(read_choice(table, path, "style", slants, 0)),
        .weight = static_cast<CharacterWeight>(read_choice(table, path, "weight", weights, 0)),
        .size = static_cast<CharacterScale>(read_choice(table, path, "size", scales, 1)),
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

std::optional<std::string> read_provider_name(
    const toml::table& table,
    const std::filesystem::path& path) {
    const auto name = read_optional<std::string>(table, path, "provider", "string");
    if (!name) return std::nullopt;
    require_path_component(*name, path);
    return name;
}

std::optional<std::string> read_style_name(
    const toml::table& table,
    const std::filesystem::path& path) {
    const auto name = read_optional<std::string>(table, path, "style", "string");
    if (!name) return std::nullopt;
    require_path_component(*name, path);
    return name;
}

// Provider settings are centralized, so any file other than a provider config
// that still spells one out is reporting a stale configuration rather than
// quietly losing to the provider it references.
void reject_provider_settings(
    const toml::table& table,
    const std::filesystem::path& path,
    std::string_view subject) {
    for (const std::string_view name : provider_setting_names) {
        if (table.contains(name)) {
            throw std::runtime_error(std::string(subject) + " '" + utf8_path(path)
                + "' sets provider setting '" + std::string(name)
                + "'; provider settings belong in a provider config selected with 'provider'");
        }
    }
}

void validate_provider_config(
    const ProviderConfig& provider,
    const std::filesystem::path& path) {
    if (!provider.host || provider.host->empty() || !provider.port) {
        throw std::runtime_error("Provider config '" + utf8_path(path)
            + "' requires non-empty string 'host' and integer 'port'");
    }
    if (*provider.port < 1 || *provider.port > 65535) {
        throw std::runtime_error("Provider config '" + utf8_path(path)
            + "' requires 'port' between 1 and 65535");
    }
    if (provider.temperature && (!std::isfinite(*provider.temperature)
        || *provider.temperature < 0.0 || *provider.temperature > 2.0)) {
        throw std::runtime_error("Provider config '" + utf8_path(path)
            + "' requires 'temperature' between 0 and 2");
    }
    if (provider.base_path && !is_valid_base_path(*provider.base_path)) {
        throw std::runtime_error("Provider config '" + utf8_path(path)
            + "' requires 'base_path' to start with '/', not end with '/', and contain no query, fragment, or whitespace");
    }
    const WebSearchMode search = provider.web_search.value_or(default_web_search_mode);
    if (search != WebSearchMode::off
        && provider.api.value_or(default_provider_api) != ProviderApi::responses) {
        throw std::runtime_error("Provider config '" + utf8_path(path)
            + "' enables web_search but web search requires api = \"responses\"");
    }
}

ProviderConfig load_named_provider(
    const std::filesystem::path& directory,
    std::string_view name,
    const std::filesystem::path& reference_path) {
    const std::filesystem::path path = directory / path_from_utf8(name) / "config.toml";
    if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error("Config file '" + utf8_path(reference_path)
            + "' references provider '" + std::string(name)
            + "' without config file '" + utf8_path(path) + "'");
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to read provider config file '" + utf8_path(path) + "'");
    }
    const toml::table table = toml::parse(file, utf8_path(path));
    for (const auto& [key, value] : table) {
        (void)value;
        if (std::ranges::find(provider_setting_names, key.str())
            == std::end(provider_setting_names)) {
            throw std::runtime_error("Provider config '" + utf8_path(path)
                + "' has unsupported field '" + std::string(key.str()) + "'");
        }
    }
    // A checked-in provider config is the wrong home for a secret; the key is
    // named here and read from the environment at request time.
    if (table.contains("api_key")) {
        throw std::runtime_error("Provider config '" + utf8_path(path)
            + "' cannot set 'api_key'; name the variable holding it with 'api_key_env'");
    }
    ProviderConfig provider{
        .host = read_optional<std::string>(table, path, "host", "string"),
        .port = read_optional<int>(table, path, "port", "integer"),
        .base_path = read_optional<std::string>(table, path, "base_path", "string"),
        .mode = read_mode(table, path),
        .model = read_optional<std::string>(table, path, "model", "string"),
        .stream = read_optional<bool>(table, path, "stream", "boolean"),
        .temperature = read_optional<double>(table, path, "temperature", "numeric"),
        .api_key_env = read_optional<std::string>(table, path, "api_key_env", "string"),
        .reasoning_effort = read_optional<std::string>(table, path, "reasoning_effort", "string"),
        .reasoning_format = read_reasoning_format(table, path),
        .https = read_optional<bool>(table, path, "https", "boolean"),
        .api = read_provider_api(table, path),
        .web_search = read_web_search_mode(table, path)};
    validate_provider_config(provider, path);
    return provider;
}

// Nothing when the layer names no provider, so callers can tell "inherit the
// layer below" apart from "use this backend".
std::optional<ProviderConfig> resolve_provider(
    const std::optional<std::string>& provider_name,
    const std::optional<std::filesystem::path>& directory,
    const std::filesystem::path& reference_path) {
    if (!provider_name) return std::nullopt;
    if (!directory) {
        throw std::runtime_error("Config file '" + utf8_path(reference_path)
            + "' references provider '" + *provider_name
            + "' but no providers directory was supplied");
    }
    return load_named_provider(*directory, *provider_name, reference_path);
}

CharacterAppearance load_named_style(
    const std::filesystem::path& directory,
    std::string_view name,
    const std::filesystem::path& reference_path) {
    const std::filesystem::path path = directory / path_from_utf8(name) / "config.toml";
    if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error("Config file '" + utf8_path(reference_path)
            + "' references style '" + std::string(name)
            + "' without config file '" + utf8_path(path) + "'");
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to read style config file '" + utf8_path(path) + "'");
    }
    return read_style_config(toml::parse(file, utf8_path(path)), path);
}

// Styles do not layer, so a definition that names none simply gets the
// standard appearance rather than inheriting anything.
CharacterAppearance resolve_style(
    const std::optional<std::string>& style_name,
    const std::optional<std::filesystem::path>& directory,
    const std::filesystem::path& reference_path) {
    if (!style_name) return {};
    if (!directory) {
        throw std::runtime_error("Config file '" + utf8_path(reference_path)
            + "' references style '" + *style_name
            + "' but no styles directory was supplied");
    }
    return load_named_style(*directory, *style_name, reference_path);
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
    // Appearance stays definition-only, so a forum-local layer is told what it
    // may not do rather than pointed at a field it also may not set.
    if (!definition && (table.contains("appearance") || table.contains("style"))) {
        throw std::runtime_error("Config file '" + utf8_path(path)
            + "' cannot define definition-only field '"
            + (table.contains("appearance") ? "appearance" : "style") + "'");
    }
    if (table.contains("appearance")) {
        throw std::runtime_error("Config file '" + utf8_path(path)
            + "' sets legacy 'appearance'; select a system style with 'style'");
    }
    const auto display_name = read_optional<std::string>(table, path, "display_name", "string");
    if (definition && (!display_name || display_name->empty())) {
        throw std::runtime_error("Character config file '" + utf8_path(path)
            + "' requires a non-empty string 'display_name' value");
    }
    const auto description = read_optional<std::string>(table, path, "description", "string");
    if (description) validate_description(*description, "Character", path);
    reject_provider_settings(table, path, "Config file");
    return {
        .display_name = display_name,
        .provider_name = read_provider_name(table, path),
        .style_name = definition ? read_style_name(table, path) : std::nullopt,
        .prompt_variables = template_scope_from_toml(table, prompt_scope_table, utf8_path(path)),
        .tags = definition ? read_tags(table, path) : std::vector<std::string>{},
        .description = description,
    };
}

void overlay(TemplateScope& effective, TemplateScope upper) {
    for (auto& [key, value] : upper) {
        effective.insert_or_assign(std::move(key), std::move(value));
    }
}

} // namespace

CharacterMetadata load_character_metadata(
    const std::filesystem::path& definition_path,
    std::optional<std::filesystem::path> providers_directory,
    std::optional<std::filesystem::path> styles_directory) {
    const ParsedConfig definition = parse_config(definition_path, ConfigLayer::definition);
    // Metadata needs no backend, but a broken reference should stop startup
    // here rather than when someone first opens a forum.
    (void)resolve_provider(definition.provider_name, providers_directory, definition_path);
    const CharacterAppearance appearance =
        resolve_style(definition.style_name, styles_directory, definition_path);
    return {
        .id = utf8_path(definition_path.parent_path().filename()),
        .display_name = *definition.display_name,
        .description = definition.description,
        .tags = definition.tags,
        .appearance = appearance,
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
    reject_provider_settings(*table, workspace_config_path, "Workspace config");
    for (const auto& [key, value] : *table) {
        (void)value;
        const std::string_view name = key.str();
        if (name != "provider") {
            throw std::runtime_error("Workspace config '" + utf8_path(workspace_config_path)
                + "' [provider] has unsupported field '" + std::string(name) + "'");
        }
    }
    const auto provider_name = read_provider_name(*table, workspace_config_path);
    if (!provider_name) {
        throw std::runtime_error("Workspace config '" + utf8_path(workspace_config_path)
            + "' [provider] requires a 'provider' name selecting a provider config");
    }
    return load_named_provider(
        providers_directory(workspace_config_path.parent_path()),
        *provider_name,
        workspace_config_path);
}

std::filesystem::path providers_directory(const std::filesystem::path& workspace_root) {
    return workspace_root / "system" / "providers";
}

std::filesystem::path styles_directory(const std::filesystem::path& workspace_root) {
    return workspace_root / "system" / "styles";
}

LoadedCharacterConfig load_character_config(const CharacterConfigPaths& paths) {
    const ParsedConfig definition = parse_config(paths.definition, ConfigLayer::definition);
    const CharacterAppearance appearance =
        resolve_style(definition.style_name, paths.styles_directory, paths.definition);
    // A provider config is a whole backend, so the highest layer naming one
    // wins outright instead of merging field by field with the layers below.
    std::optional<ProviderConfig> effective = paths.providers.application;
    TemplateScope prompt_variables = definition.prompt_variables;
    if (auto named = resolve_provider(
            definition.provider_name, paths.providers.directory, paths.definition)) {
        effective = std::move(named);
    }
    const auto apply = [&](const std::optional<std::filesystem::path>& path, ConfigLayer layer) {
        if (!path) {
            return;
        }
        const ParsedConfig parsed = parse_config(*path, layer);
        if (auto named = resolve_provider(
                parsed.provider_name, paths.providers.directory, *path)) {
            effective = std::move(named);
        }
        overlay(prompt_variables, parsed.prompt_variables);
    };
    apply(paths.forum_defaults, ConfigLayer::forum_defaults);
    apply(paths.member_override, ConfigLayer::member_override);
    if (!effective) {
        throw std::runtime_error("Character config file '" + utf8_path(paths.definition)
            + "' selects no provider and no layer below it supplies one");
    }
    CharacterMetadata character{
        .id = utf8_path(paths.definition.parent_path().filename()),
        .display_name = *definition.display_name,
        .description = definition.description,
        .tags = definition.tags,
        .appearance = appearance,
    };
    return {
        .character = std::move(character),
        .backend = make_backend_config(*effective),
        .prompt_variables = std::move(prompt_variables),
    };
}

void validate_forum_provider_defaults(
    const std::filesystem::path& path,
    const std::optional<std::filesystem::path>& directory) {
    const ParsedConfig defaults = parse_config(path, ConfigLayer::forum_defaults);
    (void)resolve_provider(defaults.provider_name, directory, path);
}

} // namespace cha
