#pragma once

#include "chat/character.h"
#include "util/text_template.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

// TOML table name for prompt-template variables and as
// TemplateOptions::scope_table_name when expanding character/forum prompts.
inline constexpr std::string_view prompt_scope_table = "prompt";

enum class Mode {
    net,
    test,
};

enum class ReasoningFormat {
    automatic,
    none,
    reasoning_content,
    reasoning,
};

// Which OpenAI-compatible wire protocol ProviderClient should speak.
enum class ProviderApi {
    chat_completions,
    responses,
};

// Hosted web-search policy for the Responses API. TOML spelling "auto" maps to
// automatic; Chat Completions rejects any value other than off.
enum class WebSearchMode {
    off,
    automatic,
    required,
};

inline constexpr ProviderApi default_provider_api = ProviderApi::responses;
inline constexpr WebSearchMode default_web_search_mode = WebSearchMode::required;

// Effective private configuration for one model backend after workspace,
// character, forum-default, and member-override layers have been applied.
struct ModelBackendConfig {
    std::string host;
    int port{};
    std::string base_path;
    Mode mode{Mode::test};
    std::string model;
    bool stream{true};
    double temperature{1.0};
    std::string api_key;
    std::string api_key_env;
    std::string reasoning_effort;
    ReasoningFormat reasoning_format{ReasoningFormat::automatic};
    bool https{};
    ProviderApi api{default_provider_api};
    WebSearchMode web_search{default_web_search_mode};
};

// Provider/runtime settings supplied by one configuration layer: workspace
// [provider], a character definition, forum defaults, or a member override.
// Which keys a particular file may set is decided by that file's parser, not by
// this type; workspace [provider] still prohibits api_key. Identity and prompt
// fields are intentionally absent: they belong to character definitions only.
// 'source' is the file this layer was read from, for diagnostics.
struct ProviderConfig {
    std::filesystem::path source;
    std::optional<std::string> host;
    std::optional<int> port;
    std::optional<std::string> base_path;
    std::optional<Mode> mode;
    std::optional<std::string> model;
    std::optional<bool> stream;
    std::optional<double> temperature;
    std::optional<std::string> api_key;
    std::optional<std::string> api_key_env;
    std::optional<std::string> reasoning_effort;
    std::optional<ReasoningFormat> reasoning_format;
    std::optional<bool> https;
    std::optional<ProviderApi> api;
    std::optional<WebSearchMode> web_search;
};

// Materializes the effective provider values, leaving ModelBackendConfig's own
// defaults in place wherever a value is absent. Throws when required host or
// port values are missing; callers may validate first to provide richer context.
ModelBackendConfig make_backend_config(const ProviderConfig& effective);

// Named configuration inputs prevent callers from confusing the four overlay roles.
struct CharacterConfigPaths {
    std::optional<ProviderConfig> application_provider;
    std::filesystem::path definition;
    std::optional<std::filesystem::path> forum_defaults;
    std::optional<std::filesystem::path> member_override;
};

// Parses the required [provider] table in workspace.toml without creating a client.
ProviderConfig load_provider_config(const std::filesystem::path& workspace_config_path);
// The same parse against an already-read document, so a caller that needs more
// than one table out of workspace.toml reads the file only once.
ProviderConfig load_provider_config(
    const toml::table& workspace_config,
    const std::filesystem::path& workspace_config_path);

// The typed connection configuration and initial template scope after all layers.
struct LoadedCharacterConfig {
    CharacterMetadata character;
    ModelBackendConfig backend;
    TemplateScope prompt_variables;
};

// Loads application provider, definition, forum defaults, and member override in precedence order.
LoadedCharacterConfig load_character_config(const CharacterConfigPaths& paths);

// Loads definition-only identity and tag metadata without requiring provider settings.
CharacterMetadata load_character_metadata(
    const std::filesystem::path& definition_path);

} // namespace cha
