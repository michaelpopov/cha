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

// Everything needed to reach one character's provider and shape its replies: identity, endpoint and
// credentials, model, streaming, and reasoning settings. The character directory
// provides the stable ID; its config provides the display name.
struct Config {
    std::string id{"assistant"};
    // Reserved for a future character-name concept distinct from display name.
    std::string name{"Assistant"};
    std::string display_name{"Assistant"};
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
    std::vector<std::string> tags;
};

// Named configuration paths prevent callers from confusing the three overlay roles.
struct CharacterConfigPaths {
    std::filesystem::path definition;
    std::optional<std::filesystem::path> forum_defaults;
    std::optional<std::filesystem::path> member_override;
};

// The typed connection configuration and initial template scope after all layers.
struct LoadedConfig {
    Config config;
    TemplateScope prompt_variables;
};

// Loads definition, forum defaults, and member override in precedence order.
LoadedConfig load_config(const CharacterConfigPaths& paths);

// Loads definition-only identity and tag metadata without requiring provider settings.
CharacterDefinitionMetadata load_character_definition_metadata(
    const std::filesystem::path& definition_path);

} // namespace cha
