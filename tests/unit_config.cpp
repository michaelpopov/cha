#include "config.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>

namespace cha {
namespace {

TEST(Config, LoadsHostAndPortFromToml) {
    const auto directory = std::filesystem::temp_directory_path()
        / ("cha_config_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    const auto path = directory / "config.toml";
    const auto prompt_path = directory / "SYSTEM.md";
    {
        std::ofstream prompt_file(prompt_path);
        prompt_file << "You are concise.";

        std::ofstream config_file(path);
        config_file
            << "host = \"example.com\"\n"
            << "port = 8080\n"
            << "mode = \"net\"\n"
            << "model = \"example-model\"\n"
            << "stream = false\n"
            << "temperature = 0.25\n"
            << "api_key = \"secret\"\n";
    }

    const Config config = Config::load(path);

    EXPECT_EQ(config.host, "example.com");
    EXPECT_EQ(config.port, 8080);
    EXPECT_EQ(config.mode, Mode::net);
    EXPECT_EQ(config.model, "example-model");
    EXPECT_FALSE(config.stream);
    ASSERT_TRUE(config.temperature);
    EXPECT_DOUBLE_EQ(*config.temperature, 0.25);
    EXPECT_EQ(config.api_key, "secret");
    EXPECT_EQ(config.system_prompt, "You are concise.");

    std::filesystem::remove_all(directory);
}

TEST(Config, LoadsFirstServerFromDirectory) {
    const auto directory = std::filesystem::temp_directory_path()
        / ("cha_servers_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto server_directory = directory / "local";
    std::filesystem::create_directories(server_directory);
    {
        std::ofstream servers_file(directory / "servers");
        servers_file << "\n# Prefer local development\n  local  \nbackup\n";

        std::ofstream config_file(server_directory / "config.toml");
        config_file
            << "host = \"127.0.0.1\"\n"
            << "port = 8080\n"
            << "model = \"local-model\"\n";
    }

    const Config config = Config::load_from_directory(directory);

    EXPECT_EQ(config.host, "127.0.0.1");
    EXPECT_EQ(config.model, "local-model");

    std::filesystem::remove_all(directory);
}

TEST(Config, AllowsMissingModel) {
    const auto path = std::filesystem::temp_directory_path()
        / ("cha_no_model_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".toml");
    {
        std::ofstream config_file(path);
        config_file << "host = \"example.com\"\nport = 8080\n";
    }

    const Config config = Config::load(path);

    EXPECT_TRUE(config.model.empty());
    EXPECT_TRUE(config.system_prompt.empty());
    std::filesystem::remove(path);
}

TEST(Config, RejectsOutOfRangePort) {
    const auto path = std::filesystem::temp_directory_path()
        / ("cha_invalid_port_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".toml");
    {
        std::ofstream config_file(path);
        config_file
            << "host = \"example.com\"\n"
            << "port = 65536\n"
            << "model = \"example-model\"\n";
    }

    EXPECT_THROW(
        {
            const auto config = Config::load(path);
            (void)config;
        },
        std::runtime_error);
    std::filesystem::remove(path);
}

} // namespace
} // namespace cha
