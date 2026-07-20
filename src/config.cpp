#include "config.h"

#include <toml++/toml.hpp>

#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace cha {

namespace {

std::string_view trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

} // namespace

Config Config::load() {
    return load_from_directory(".");
}

Config Config::load_from_directory(const std::filesystem::path& directory) {
    const std::filesystem::path servers_path = directory / "servers";
    std::ifstream servers_file(servers_path);
    if (!servers_file) {
        throw std::runtime_error("Failed to read servers file '" + servers_path.string() + "'");
    }

    std::string line;
    while (std::getline(servers_file, line)) {
        const std::string_view server_name = trim(line);
        if (server_name.empty() || server_name.front() == '#') {
            continue;
        }

        const std::filesystem::path server_directory{server_name};
        if (server_directory.is_absolute() || server_directory.has_parent_path()
            || server_name == "." || server_name == "..") {
            throw std::runtime_error(
                "Servers file '" + servers_path.string() + "' contains invalid server name '"
                + std::string(server_name) + "'");
        }

        return load(directory / server_directory / "config.toml");
    }

    throw std::runtime_error("Servers file '" + servers_path.string() + "' does not name a server");
}

Config Config::load(const std::filesystem::path& path) {
    const toml::table table = toml::parse_file(path.string());
    const auto host = table["host"].value<std::string>();
    const auto port = table["port"].value<int>();
    const auto model = table["model"].value<std::string>();

    if (!host || !port) {
        throw std::runtime_error(
            "Config file '" + path.string() + "' requires string 'host' and integer 'port' values"
        );
    }
    if (table.contains("model") && !model) {
        throw std::runtime_error("Config file '" + path.string() + "' requires string 'model' value");
    }
    if (*port < 1 || *port > 65535) {
        throw std::runtime_error("Config file '" + path.string() + "' requires 'port' between 1 and 65535");
    }

    Mode mode = Mode::test;
    if (table.contains("mode")) {
        const auto mode_name = table["mode"].value<std::string>();

        if (!mode_name) {
            throw std::runtime_error("Config file '" + path.string() + "' requires string 'mode' value");
        }

        if (*mode_name == "net") {
            mode = Mode::net;
        } else if (*mode_name != "test") {
            throw std::runtime_error("Config file '" + path.string() + "' has unsupported mode '" + *mode_name + "'");
        }
    }

    bool stream = true;
    if (table.contains("stream")) {
        const auto value = table["stream"].value<bool>();
        if (!value) {
            throw std::runtime_error("Config file '" + path.string() + "' requires boolean 'stream' value");
        }
        stream = *value;
    }

    std::optional<double> temperature;
    if (table.contains("temperature")) {
        temperature = table["temperature"].value<double>();
        if (!temperature) {
            throw std::runtime_error("Config file '" + path.string() + "' requires numeric 'temperature' value");
        }
    }

    std::string api_key;
    if (table.contains("api_key")) {
        const auto value = table["api_key"].value<std::string>();
        if (!value) {
            throw std::runtime_error("Config file '" + path.string() + "' requires string 'api_key' value");
        }
        api_key = *value;
    }

    std::string system_prompt;
    std::ifstream prompt_file(path.parent_path() / "SYSTEM.md", std::ios::binary);
    if (prompt_file) {
        std::ostringstream contents;
        contents << prompt_file.rdbuf();
        system_prompt = contents.str();
    }

    return Config{
        *host,
        *port,
        mode,
        model.value_or(""),
        stream,
        temperature,
        api_key,
        std::move(system_prompt),
    };
}

} // namespace cha
