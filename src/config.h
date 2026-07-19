#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace cha {

enum class Mode {
    net,
    test,
};

struct Config {
    std::string host;
    int port{};
    Mode mode{Mode::test};
    std::string model;
    bool stream{true};
    std::optional<double> temperature;
    std::string api_key;
    std::filesystem::path system_prompt;

    [[nodiscard]] static Config load(const std::filesystem::path& path = "config.toml");
};

} // namespace cha
