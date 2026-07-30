#pragma once

#include "util/text_template.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace cha {

// TOML table name for prompt-template variables and as
// TemplateOptions::scope_table_name when expanding persona/forum prompts.
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

// Everything needed to reach one persona's provider and shape its replies: identity, endpoint and
// credentials, model, streaming, and reasoning settings. The persona directory
// provides the stable ID; its config provides the display name.
struct Config {
    std::string id{"assistant"};
    std::string name{"Assistant"};
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

// The typed connection configuration and initial template scope loaded from
// one persona file. The optional base file supplies defaults to both.
struct LoadedConfig {
    Config config;
    TemplateScope prompt_variables;
};

// Loads one persona configuration and its initial [prompt] scope from the
// same TOML parses. The parent directory name becomes its ID and display_name
// must come from persona_path. When base_path is present, its other values
// become defaults that the persona file may override.
LoadedConfig load_config(
    const std::filesystem::path& persona_path,
    std::optional<std::filesystem::path> base_path = std::nullopt);

} // namespace cha
