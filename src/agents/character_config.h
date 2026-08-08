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

// Effective private configuration for one model backend after workspace,
// character, forum-default, and member-override layers have been applied.
struct ModelBackendConfig {
    std::string host;
    int port{};
    Mode mode{Mode::test};
    std::string model;
    bool stream{true};
    double temperature{1.0};
    std::string api_key;
    std::string api_key_env;
    std::string reasoning_effort;
    ReasoningFormat reasoning_format{ReasoningFormat::automatic};
    bool https{};
};

// Provider/runtime settings supplied by workspace.toml. Identity and prompt fields
// are intentionally absent: they belong to character definitions only.
struct ProviderConfig {
    std::filesystem::path source;
    std::optional<std::string> host;
    std::optional<int> port;
    std::optional<Mode> mode;
    std::optional<std::string> model;
    std::optional<bool> stream;
    std::optional<double> temperature;
    std::optional<std::string> api_key_env;
    std::optional<std::string> reasoning_effort;
    std::optional<ReasoningFormat> reasoning_format;
    std::optional<bool> https;
};

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
