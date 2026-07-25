#include "agents/config.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace cha {
namespace {

TEST(Config, LoadsHostAndPortFromToml) {
    const auto directory = std::filesystem::temp_directory_path()
        / ("cha_config_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    const auto path = directory / "config.toml";
    {
        std::ofstream config_file(path);
        config_file
            << "id = \"example-id\"\n"
            << "name = \"Example\"\n"
            << "host = \"example.com\"\n"
            << "port = 8080\n"
            << "mode = \"net\"\n"
            << "model = \"example-model\"\n"
            << "stream = false\n"
            << "temperature = 0.25\n"
            << "api_key = \"secret\"\n"
            << "api_key_env = \"OPENAI_API_KEY\"\n"
            << "reasoning_effort = \"medium\"\n"
            << "reasoning_format = \"reasoning_content\"\n"
            << "https = true\n";
    }

    const Config config = load_config(path);

    EXPECT_EQ(config.id, "example-id");
    EXPECT_EQ(config.name, "Example");
    EXPECT_EQ(config.host, "example.com");
    EXPECT_EQ(config.port, 8080);
    EXPECT_EQ(config.mode, Mode::net);
    EXPECT_EQ(config.model, "example-model");
    EXPECT_FALSE(config.stream);
    ASSERT_TRUE(config.temperature);
    EXPECT_DOUBLE_EQ(*config.temperature, 0.25);
    EXPECT_EQ(config.api_key, "secret");
    EXPECT_EQ(config.api_key_env, "OPENAI_API_KEY");
    EXPECT_EQ(config.reasoning_effort, "medium");
    EXPECT_EQ(config.reasoning_format, ReasoningFormat::reasoning_content);
    EXPECT_TRUE(config.https);

    std::filesystem::remove_all(directory);
}

TEST(Config, DefaultsAndValidatesReasoningFormat) {
    const auto path = std::filesystem::temp_directory_path()
        / ("cha_reasoning_format_"
           + std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count())
           + ".toml");
    {
        std::ofstream config_file(path);
        config_file << "id = \"example-id\"\n"
                    << "name = \"Example\"\n"
                    << "host = \"example.com\"\n"
                    << "port = 8080\n";
    }
    EXPECT_EQ(
        load_config(path).reasoning_format,
        ReasoningFormat::automatic);

    {
        std::ofstream config_file(path);
        config_file << "id = \"example-id\"\n"
                    << "name = \"Example\"\n"
                    << "host = \"example.com\"\n"
                    << "port = 8080\n"
                    << "reasoning_format = \"tags\"\n";
    }
    try {
        (void)load_config(path);
        FAIL() << "Expected invalid reasoning_format to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find(path.string()),
            std::string::npos);
        EXPECT_NE(
            std::string(error.what()).find("reasoning_format"),
            std::string::npos);
    }
    std::filesystem::remove(path);
}

TEST(Config, AllowsMissingModel) {
    const auto path = std::filesystem::temp_directory_path()
        / ("cha_no_model_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".toml");
    {
        std::ofstream config_file(path);
        config_file << "id = \"example-id\"\nname = \"Example\"\n"
                    << "host = \"example.com\"\nport = 8080\n";
    }

    const Config config = load_config(path);

    EXPECT_TRUE(config.model.empty());
    std::filesystem::remove(path);
}

TEST(Config, RejectsOutOfRangePort) {
    const auto path = std::filesystem::temp_directory_path()
        / ("cha_invalid_port_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".toml");
    {
        std::ofstream config_file(path);
        config_file
            << "id = \"example-id\"\n"
            << "name = \"Example\"\n"
            << "host = \"example.com\"\n"
            << "port = 65536\n"
            << "model = \"example-model\"\n";
    }

    EXPECT_THROW(
        {
            const auto config = load_config(path);
            (void)config;
        },
        std::runtime_error);
    std::filesystem::remove(path);
}

TEST(Config, RequiresStableIdentityAndDisplayName) {
    const auto path = std::filesystem::temp_directory_path()
        / ("cha_missing_identity_"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
           + ".toml");
    {
        std::ofstream config_file(path);
        config_file << "host = \"example.com\"\nport = 8080\n";
    }

    EXPECT_THROW((void)load_config(path), std::runtime_error);
    std::filesystem::remove(path);
}

} // namespace
} // namespace cha
