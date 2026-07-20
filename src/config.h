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
    bool https{};
    std::string system_prompt;

    [[nodiscard]] static Config load();
    [[nodiscard]] static Config load(const std::filesystem::path& path);
    [[nodiscard]] static Config load_from_directory(const std::filesystem::path& directory);
};

} // namespace cha
