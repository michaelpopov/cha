#include "config.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

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
            << "https = true\n";
    }

    const Config config = Config::load(path);

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
    EXPECT_TRUE(config.https);

    std::filesystem::remove_all(directory);
}

TEST(Config, AllowsMissingModel) {
    const auto path = std::filesystem::temp_directory_path()
        / ("cha_no_model_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".toml");
    {
        std::ofstream config_file(path);
        config_file << "id = \"example-id\"\nname = \"Example\"\n"
                    << "host = \"example.com\"\nport = 8080\n";
    }

    const Config config = Config::load(path);

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
            const auto config = Config::load(path);
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

    EXPECT_THROW((void)Config::load(path), std::runtime_error);
    std::filesystem::remove(path);
}

TEST(Config, RejectsParticipantIdsThatAreUnsafeForProtocolUse) {
    const auto path = std::filesystem::temp_directory_path()
        / ("cha_invalid_identity_"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
           + ".toml");
    {
        std::ofstream config_file(path);
        config_file
            << "id = \"example agent\"\n"
            << "name = \"Example\"\n"
            << "host = \"example.com\"\n"
            << "port = 8080\n";
    }

    EXPECT_THROW((void)Config::load(path), std::runtime_error);
    std::filesystem::remove(path);
}

// Writes a persona config carrying the given display name and returns its path.
std::filesystem::path config_named(std::string_view name) {
    static int counter = 0;
    const auto path = std::filesystem::temp_directory_path()
        / ("cha_named_"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
           + "_" + std::to_string(++counter) + ".toml");
    std::ofstream config_file(path);
    config_file
        << "id = \"example-id\"\n"
        << "name = \"" << name << "\"\n"
        << "host = \"example.com\"\n"
        << "port = 8080\n";
    return path;
}

TEST(Config, RejectsDisplayNamesThatCannotBeUsedAsAMention) {
    for (const std::string_view name :
         {"Local assistant", "Two\tWords", "@Ismael", "/Ismael", "User", "uSeR"}) {
        const auto path = config_named(name);
        EXPECT_THROW((void)Config::load(path), std::runtime_error)
            << "accepted unusable display name '" << name << "'";
        std::filesystem::remove(path);
    }
}

TEST(Config, AcceptsDisplayNamesThatOnlyResembleTheReservedLabel) {
    for (const std::string_view name : {"Users", "Use", "User.", "Ismael."}) {
        const auto path = config_named(name);
        EXPECT_NO_THROW((void)Config::load(path))
            << "rejected usable display name '" << name << "'";
        std::filesystem::remove(path);
    }
}

TEST(Config, ExplainsIdentityFailuresWithTheOffendingConfigPath) {
    const auto path = config_named("Local assistant");

    try {
        (void)Config::load(path);
        FAIL() << "expected an identity diagnostic";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find(path.string()), std::string::npos) << message;
        EXPECT_NE(message.find("whitespace"), std::string::npos) << message;
    }
    std::filesystem::remove(path);
}

} // namespace
} // namespace cha
