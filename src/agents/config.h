#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace cha {

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

// Loads one persona configuration. The parent directory name becomes its ID and
// display_name must come from persona_path. When base_path is present, its other
// values become defaults that the persona file may override.
Config load_config(
    const std::filesystem::path& persona_path,
    std::optional<std::filesystem::path> base_path = std::nullopt);

} // namespace cha
