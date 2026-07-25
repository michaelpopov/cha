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
// credentials, model, streaming, and reasoning settings. Loaded from a persona's TOML file by
// load_config(); every field has a usable default, so a partial file stays valid.
struct Config {
    std::string id{"assistant"};
    std::string name{"Assistant"};
    std::string host;
    int port{};
    Mode mode{Mode::test};
    std::string model;
    bool stream{true};
    std::optional<double> temperature;
    std::string api_key;
    std::string api_key_env;
    std::string reasoning_effort;
    ReasoningFormat reasoning_format{ReasoningFormat::automatic};
    bool https{};
};

// Loads one persisted agent configuration from TOML.
Config load_config(const std::filesystem::path& path);

} // namespace cha
