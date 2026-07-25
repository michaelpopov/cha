#include "agents/config.h"

#include <toml++/toml.hpp>

#include <stdexcept>

namespace cha {
Config load_config(const std::filesystem::path& path) {
    const toml::table table = toml::parse_file(path.string());
    const auto host = table["host"].value<std::string>();
    const auto port = table["port"].value<int>();
    const auto model = table["model"].value<std::string>();
    const auto id = table["id"].value<std::string>();
    const auto name = table["name"].value<std::string>();

    if (!host || !port || !id || !name || id->empty() || name->empty()) {
        throw std::runtime_error(
            "Config file '" + path.string()
            + "' requires non-empty string 'id' and 'name', string 'host', and integer 'port' values"
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

    std::string api_key_env;
    if (table.contains("api_key_env")) {
        const auto value = table["api_key_env"].value<std::string>();
        if (!value) {
            throw std::runtime_error("Config file '" + path.string() + "' requires string 'api_key_env' value");
        }
        api_key_env = *value;
    }

    std::string reasoning_effort;
    if (table.contains("reasoning_effort")) {
        const auto value = table["reasoning_effort"].value<std::string>();
        if (!value) {
            throw std::runtime_error(
                "Config file '" + path.string() + "' requires string 'reasoning_effort' value"
            );
        }
        reasoning_effort = *value;
    }

    ReasoningFormat reasoning_format = ReasoningFormat::automatic;
    if (table.contains("reasoning_format")) {
        const auto value = table["reasoning_format"].value<std::string>();
        if (!value) {
            throw std::runtime_error(
                "Config file '" + path.string()
                + "' requires string 'reasoning_format' value");
        }
        if (*value == "auto") {
            reasoning_format = ReasoningFormat::automatic;
        } else if (*value == "none") {
            reasoning_format = ReasoningFormat::none;
        } else if (*value == "reasoning_content") {
            reasoning_format = ReasoningFormat::reasoning_content;
        } else if (*value == "reasoning") {
            reasoning_format = ReasoningFormat::reasoning;
        } else {
            throw std::runtime_error(
                "Config file '" + path.string()
                + "' has unsupported reasoning_format '" + *value + "'");
        }
    }

    bool https = false;
    if (table.contains("https")) {
        const auto value = table["https"].value<bool>();
        if (!value) {
            throw std::runtime_error("Config file '" + path.string() + "' requires boolean 'https' value");
        }
        https = *value;
    }

    return Config{
        .id = *id,
        .name = *name,
        .host = *host,
        .port = *port,
        .mode = mode,
        .model = model.value_or(""),
        .stream = stream,
        .temperature = temperature,
        .api_key = api_key,
        .api_key_env = api_key_env,
        .reasoning_effort = reasoning_effort,
        .reasoning_format = reasoning_format,
        .https = https,
    };
}

} // namespace cha
