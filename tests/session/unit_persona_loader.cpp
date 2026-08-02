#include "session/workspace.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace cha {
namespace {

class PersonaLoaderTest : public testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path()
            / ("cha_personas_" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root_ / "forums");
        std::ofstream(root_ / "app.toml")
            << "host = \"127.0.0.1\"\n"
               "port = 8080\n"
               "[logging]\n"
               "file = \"cha.log\"\n"
               "level = \"off\"\n";
    }

    void TearDown() override { std::filesystem::remove_all(root_); }

    void add_persona(
        std::string_view id,
        std::string_view display_name,
        std::string_view prompt = "") {
        const std::filesystem::path directory = root_ / "personas" / id;
        std::filesystem::create_directories(directory);
        std::ofstream(directory / "persona.toml")
            << "display_name = \"" << display_name << "\"\n";
        if (!prompt.empty()) std::ofstream(directory / "PERSONA.md") << prompt;
    }

    std::filesystem::path root_;
};

TEST_F(PersonaLoaderTest, LoadsPersonasInLexicographicIdOrderAndReadsOptionalPrompt) {
    add_persona("zebra", "Zebra", "Verbatim\ntext");
    add_persona("alpha", "Alpha");

    const PersonaRoster personas = Workspace(root_).load_personas();

    ASSERT_EQ(personas.size(), 2U);
    EXPECT_EQ(personas[0].id, "alpha");
    EXPECT_EQ(personas[0].display_name, "Alpha");
    EXPECT_TRUE(personas[0].prompt.empty());
    EXPECT_EQ(personas[1].id, "zebra");
    EXPECT_EQ(personas[1].display_name, "Zebra");
    EXPECT_EQ(personas[1].prompt, "Verbatim\ntext");
}

TEST_F(PersonaLoaderTest, ReloadsTheRosterOnEveryCall) {
    add_persona("ada", "Ada");
    Workspace workspace(root_);

    EXPECT_EQ(workspace.load_personas().size(), 1U);

    add_persona("bert", "Bert");
    const PersonaRoster personas = workspace.load_personas();
    ASSERT_EQ(personas.size(), 2U);
    EXPECT_EQ(personas[1].id, "bert");
}

TEST_F(PersonaLoaderTest, RequiresPersonasDirectoryAndAtLeastOnePersona) {
    EXPECT_THROW((void)Workspace(root_).load_personas(), std::runtime_error);

    std::filesystem::create_directories(root_ / "personas");
    EXPECT_THROW((void)Workspace(root_).load_personas(), std::runtime_error);
}

TEST_F(PersonaLoaderTest, RequiresAPersonaTomlForEveryPersonaDirectory) {
    std::filesystem::create_directories(root_ / "personas" / "missing");

    EXPECT_THROW((void)Workspace(root_).load_personas(), std::runtime_error);
}

TEST_F(PersonaLoaderTest, RequiresPersonaPromptToBeARegularFile) {
    add_persona("ada", "Ada");
    std::filesystem::create_directory(root_ / "personas" / "ada" / "PERSONA.md");

    EXPECT_THROW((void)Workspace(root_).load_personas(), std::runtime_error);
}

TEST_F(PersonaLoaderTest, RejectsUnknownOrMissingOrWrongTypedConfigFields) {
    add_persona("ada", "Ada");
    {
        std::ofstream config(root_ / "personas" / "ada" / "persona.toml");
        config << "display_name = \"Ada\"\nextra = true\n";
    }
    EXPECT_THROW((void)Workspace(root_).load_personas(), std::runtime_error);

    {
        std::ofstream config(root_ / "personas" / "ada" / "persona.toml");
        config << "other = \"Ada\"\n";
    }
    EXPECT_THROW((void)Workspace(root_).load_personas(), std::runtime_error);

    {
        std::ofstream config(root_ / "personas" / "ada" / "persona.toml");
        config << "display_name = 42\n";
    }
    EXPECT_THROW((void)Workspace(root_).load_personas(), std::runtime_error);
}

TEST_F(PersonaLoaderTest, RejectsInvalidAndReservedIds) {
    add_persona("bad-id", "Ada");
    EXPECT_THROW((void)Workspace(root_).load_personas(), std::runtime_error);

    std::filesystem::remove_all(root_ / "personas");
    add_persona("9ada", "Ada");
    EXPECT_THROW((void)Workspace(root_).load_personas(), std::runtime_error);

    std::filesystem::remove_all(root_ / "personas");
    add_persona("System", "Ada");
    EXPECT_THROW((void)Workspace(root_).load_personas(), std::runtime_error);
}

TEST_F(PersonaLoaderTest, RejectsInvalidReservedAndDuplicateDisplayNames) {
    add_persona("ada", " Ada");
    EXPECT_THROW((void)Workspace(root_).load_personas(), std::runtime_error);

    {
        std::ofstream config(root_ / "personas" / "ada" / "persona.toml");
        config << "display_name = \"@Ada\"\n";
    }
    EXPECT_THROW((void)Workspace(root_).load_personas(), std::runtime_error);

    {
        std::ofstream config(root_ / "personas" / "ada" / "persona.toml");
        config << "display_name = \"System\"\n";
    }
    EXPECT_THROW((void)Workspace(root_).load_personas(), std::runtime_error);

    {
        std::ofstream config(root_ / "personas" / "ada" / "persona.toml");
        config << "display_name = \"Ada\\u0001\"\n";
    }
    EXPECT_THROW((void)Workspace(root_).load_personas(), std::runtime_error);

    {
        std::ofstream config(root_ / "personas" / "ada" / "persona.toml");
        config << "display_name = \"Ada\"\n";
    }
    add_persona("other", "aDA");
    EXPECT_THROW((void)Workspace(root_).load_personas(), std::runtime_error);
}

TEST_F(PersonaLoaderTest, RejectsUnicodeControlsLineBreaksAndBoundaryWhitespace) {
    add_persona("ada", "Ada");
    const std::filesystem::path config_path =
        root_ / "personas" / "ada" / "persona.toml";

    std::ofstream(config_path)
        << "display_name = \"Ada\\u0085Lovelace\"\n";
    EXPECT_THROW((void)Workspace(root_).load_personas(), std::runtime_error);

    std::ofstream(config_path)
        << "display_name = \"Ada\\u2028Lovelace\"\n";
    EXPECT_THROW((void)Workspace(root_).load_personas(), std::runtime_error);

    std::ofstream(config_path)
        << "display_name = \"\\u00a0Ada\"\n";
    EXPECT_THROW((void)Workspace(root_).load_personas(), std::runtime_error);

    std::ofstream(config_path)
        << "display_name = \"Ada\\u3000\"\n";
    EXPECT_THROW((void)Workspace(root_).load_personas(), std::runtime_error);

    std::ofstream(config_path)
        << "display_name = \"Ren\\u00e9e \\u674e\"\n";
    const PersonaRoster personas = Workspace(root_).load_personas();
    ASSERT_EQ(personas.size(), 1U);
    EXPECT_EQ(personas.front().display_name, "Renée 李");
}

} // namespace
} // namespace cha
