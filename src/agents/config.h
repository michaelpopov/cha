#pragma once

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

// How one character's words are set on screen, so a reader can tell who is
// speaking without reading the name above every reply. Deliberately a closed
// vocabulary rather than free CSS: the browser has to pair each choice with a
// dark-mode variant, and a workspace must not be able to reach into the page's
// styling. Every field defaults to the interface's own settings, so a character
// that says nothing about its appearance looks exactly as it does today.
enum class CharacterFont { sans, serif, mono };
enum class CharacterSlant { normal, italic };
enum class CharacterWeight { normal, bold };
enum class CharacterScale { small, normal, large };

struct CharacterAppearance {
    CharacterFont font{CharacterFont::sans};
    CharacterSlant style{CharacterSlant::normal};
    CharacterWeight weight{CharacterWeight::normal};
    CharacterScale size{CharacterScale::normal};

    bool operator==(const CharacterAppearance&) const = default;
};

std::string_view to_string(CharacterFont value);
std::string_view to_string(CharacterSlant value);
std::string_view to_string(CharacterWeight value);
std::string_view to_string(CharacterScale value);

// Everything needed to initialize one character: public metadata plus endpoint,
// credentials, model, streaming, and reasoning settings. The character directory
// provides the stable ID; its config provides display metadata.
struct Config {
    std::string id{"assistant"};
    // Reserved for a future character-name concept distinct from display name.
    std::string name{"Assistant"};
    std::string display_name{"Assistant"};
    std::optional<std::string> description;
    CharacterAppearance appearance;
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

// Definition-only data used for workspace validation and character browsing.
struct CharacterDefinitionMetadata {
    std::string id;
    std::string display_name;
    std::optional<std::string> description;
    std::vector<std::string> tags;
    CharacterAppearance appearance;
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
struct LoadedConfig {
    Config config;
    TemplateScope prompt_variables;
};

// Loads application provider, definition, forum defaults, and member override in precedence order.
LoadedConfig load_config(const CharacterConfigPaths& paths);

// Loads definition-only identity and tag metadata without requiring provider settings.
CharacterDefinitionMetadata load_character_definition_metadata(
    const std::filesystem::path& definition_path);

} // namespace cha
