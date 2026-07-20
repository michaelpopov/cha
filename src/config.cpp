#include "config.h"

#include <toml++/toml.hpp>

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace cha {
namespace {

bool is_participant_id(std::string_view id) {
    for (const char character : id) {
        const bool ascii_letter = (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z');
        const bool digit = character >= '0' && character <= '9';
        if (!ascii_letter && !digit && character != '_' && character != '-') {
            return false;
        }
    }
    return !id.empty();
}

} // namespace

Config Config::load(const std::filesystem::path& path) {
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
    if (!is_participant_id(*id)) {
        throw std::runtime_error(
            "Config file '" + path.string()
            + "' requires 'id' to contain only ASCII letters, digits, underscores, and hyphens"
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

    bool https = false;
    if (table.contains("https")) {
        const auto value = table["https"].value<bool>();
        if (!value) {
            throw std::runtime_error("Config file '" + path.string() + "' requires boolean 'https' value");
        }
        https = *value;
    }

    std::string system_prompt;
    std::ifstream prompt_file(path.parent_path() / "SYSTEM.md", std::ios::binary);
    if (prompt_file) {
        std::ostringstream contents;
        contents << prompt_file.rdbuf();
        system_prompt = contents.str();
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
        .https = https,
        .system_prompt = std::move(system_prompt),
    };
}

} // namespace cha
