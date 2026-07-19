#include "config.h"

#include <toml++/toml.hpp>

#include <stdexcept>

namespace cha {

Config Config::load(const std::filesystem::path& path) {
    const toml::table table = toml::parse_file(path.string());
    const auto host = table["host"].value<std::string>();
    const auto port = table["port"].value<int>();
    const auto model = table["model"].value<std::string>();

    if (!host || !port || !model) {
        throw std::runtime_error(
            "Config file '" + path.string() + "' requires string 'host', integer 'port', and string 'model' values"
        );
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

    std::filesystem::path system_prompt;
    if (table.contains("system_prompt")) {
        const auto value = table["system_prompt"].value<std::string>();
        if (!value) {
            throw std::runtime_error("Config file '" + path.string() + "' requires string 'system_prompt' value");
        }

        system_prompt = *value;
        if (!system_prompt.empty() && system_prompt.is_relative()) {
            system_prompt = (path.parent_path() / system_prompt).lexically_normal();
        }
    }

    return Config{
        *host,
        *port,
        mode,
        *model,
        stream,
        temperature,
        api_key,
        system_prompt,
    };
}

} // namespace cha
