#include "agents/config.h"
#include "util/utf8_path.h"

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
    EXPECT_DOUBLE_EQ(config.temperature, 0.25);
    EXPECT_EQ(config.api_key, "secret");
    EXPECT_EQ(config.api_key_env, "OPENAI_API_KEY");
    EXPECT_EQ(config.reasoning_effort, "medium");
    EXPECT_EQ(config.reasoning_format, ReasoningFormat::reasoning_content);
    EXPECT_TRUE(config.https);

    std::filesystem::remove_all(directory);
}

TEST(Config, LoadsTomlFromANonAsciiPath) {
    const auto directory = std::filesystem::temp_directory_path()
        / path_from_utf8(
            "cha_config_na\xc3\xafve_\xe6\x9d\xb1\xe4\xba\xac_"
            + std::to_string(
                std::chrono::steady_clock::now()
                    .time_since_epoch()
                    .count()));
    std::filesystem::create_directory(directory);
    const auto path = directory / "config.toml";
    {
        std::ofstream config_file(path);
        config_file << "id = \"example-id\"\n"
                    << "name = \"Example\"\n"
                    << "host = \"example.com\"\n"
                    << "port = 8080\n";
    }

    EXPECT_EQ(load_config(path).id, "example-id");
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
            std::string(error.what()).find(utf8_path(path)),
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
    EXPECT_DOUBLE_EQ(config.temperature, 1.0);
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

TEST(Config, RejectsOutOfRangeTemperature) {
    const auto path = std::filesystem::temp_directory_path()
        / ("cha_invalid_temperature_"
           + std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count())
           + ".toml");
    {
        std::ofstream config_file(path);
        config_file
            << "id = \"example-id\"\n"
            << "name = \"Example\"\n"
            << "host = \"example.com\"\n"
            << "port = 8080\n"
            << "temperature = 2.1\n";
    }

    try {
        (void)load_config(path);
        FAIL() << "Expected invalid temperature to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find(utf8_path(path)),
            std::string::npos);
        EXPECT_NE(
            std::string(error.what()).find("temperature"),
            std::string::npos);
    }
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

TEST(Config, OverlaysPersonaValuesOnWorkspaceDefaults) {
    const auto directory = std::filesystem::temp_directory_path()
        / ("cha_config_overlay_"
           + std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    const auto base_path = directory / "base_config.toml";
    const auto persona_path = directory / "config.toml";
    {
        std::ofstream base(base_path);
        base << "host = \"shared.example\"\n"
             << "port = 443\n"
             << "https = true\n"
             << "mode = \"net\"\n"
             << "model = \"shared-model\"\n"
             << "stream = true\n"
             << "temperature = 0.5\n"
             << "api_key_env = \"SHARED_API_KEY\"\n"
             << "reasoning_effort = \"medium\"\n"
             << "reasoning_format = \"reasoning\"\n";
        std::ofstream persona(persona_path);
        persona << "id = \"example-id\"\n"
                << "name = \"Example\"\n"
                << "model = \"persona-model\"\n"
                << "stream = false\n";
    }

    const Config config = load_config(persona_path, base_path);

    EXPECT_EQ(config.id, "example-id");
    EXPECT_EQ(config.name, "Example");
    EXPECT_EQ(config.host, "shared.example");
    EXPECT_EQ(config.port, 443);
    EXPECT_TRUE(config.https);
    EXPECT_EQ(config.mode, Mode::net);
    EXPECT_EQ(config.model, "persona-model");
    EXPECT_FALSE(config.stream);
    EXPECT_DOUBLE_EQ(config.temperature, 0.5);
    EXPECT_EQ(config.api_key_env, "SHARED_API_KEY");
    EXPECT_EQ(config.reasoning_effort, "medium");
    EXPECT_EQ(config.reasoning_format, ReasoningFormat::reasoning);

    std::filesystem::remove_all(directory);
}

TEST(Config, RejectsPersonaIdentityInWorkspaceDefaults) {
    const auto directory = std::filesystem::temp_directory_path()
        / ("cha_config_base_identity_"
           + std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    const auto base_path = directory / "base_config.toml";
    const auto persona_path = directory / "config.toml";
    {
        std::ofstream base(base_path);
        base << "id = \"shared\"\n"
             << "host = \"shared.example\"\n"
             << "port = 443\n";
        std::ofstream persona(persona_path);
        persona << "id = \"example-id\"\n"
                << "name = \"Example\"\n";
    }

    try {
        (void)load_config(persona_path, base_path);
        FAIL() << "Expected shared identity to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find(utf8_path(base_path)),
            std::string::npos);
        EXPECT_NE(
            std::string(error.what()).find("id"),
            std::string::npos);
    }

    std::filesystem::remove_all(directory);
}

TEST(Config, LoadPromptVariablesOverlaysBaseThenPersona) {
    const auto directory = std::filesystem::temp_directory_path()
        / ("cha_prompt_vars_"
           + std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    const auto base_path = directory / "base_config.toml";
    const auto persona_path = directory / "config.toml";
    {
        std::ofstream base(base_path);
        base << "host = \"shared.example\"\n"
             << "port = 443\n"
             << "[prompt]\n"
             << "register = \"measured\"\n"
             << "period = \"base-period\"\n";
        std::ofstream persona(persona_path);
        persona << "display_name = \"Example\"\n"
                << "[prompt]\n"
                << "register = \"energetic\"\n";
    }

    const auto persona_no_prompt = directory / "persona_no_prompt.toml";
    {
        std::ofstream file(persona_no_prompt);
        file << "display_name = \"Example\"\n";
    }

    const TemplateScope only_base =
        load_prompt_variables(persona_no_prompt, base_path);
    EXPECT_EQ(only_base.at("register"), "measured");
    EXPECT_EQ(only_base.at("period"), "base-period");

    const TemplateScope only_persona = load_prompt_variables(persona_path);
    EXPECT_EQ(only_persona.at("register"), "energetic");
    EXPECT_EQ(only_persona.find("period"), only_persona.end());

    const TemplateScope overlaid = load_prompt_variables(persona_path, base_path);
    EXPECT_EQ(overlaid.at("register"), "energetic");
    EXPECT_EQ(overlaid.at("period"), "base-period");

    std::filesystem::remove_all(directory);
}

TEST(Config, IdentifiesInvalidWorkspaceDefaultSource) {
    const auto directory = std::filesystem::temp_directory_path()
        / ("cha_config_base_value_"
           + std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    const auto base_path = directory / "base_config.toml";
    const auto persona_path = directory / "config.toml";
    {
        std::ofstream base(base_path);
        base << "host = \"shared.example\"\n"
             << "port = 65536\n";
        std::ofstream persona(persona_path);
        persona << "id = \"example-id\"\n"
                << "name = \"Example\"\n";
    }

    try {
        (void)load_config(persona_path, base_path);
        FAIL() << "Expected invalid shared port to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find(utf8_path(base_path)),
            std::string::npos);
        EXPECT_NE(
            std::string(error.what()).find("port"),
            std::string::npos);
    }

    std::filesystem::remove_all(directory);
}

} // namespace
} // namespace cha
