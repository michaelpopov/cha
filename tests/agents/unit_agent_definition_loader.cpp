#include "agents/agent.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cha {
namespace {

std::filesystem::path unique_definition_directory() {
    return std::filesystem::temp_directory_path()
        / ("cha_agent_definition_"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

void expect_forum_context(
    const AgentDefinition& definition,
    std::string_view prompt_prefix,
    std::string_view current_name,
    std::string_view other_agents_json) {
    EXPECT_TRUE(definition.system_prompt.starts_with(prompt_prefix));
    EXPECT_NE(
        definition.system_prompt.find(
            "You are the agent named \"" + std::string(current_name) + "\"."),
        std::string::npos);
    EXPECT_NE(
        definition.system_prompt.find(
            "Other agents currently participating in this forum (JSON): "
            + std::string(other_agents_json) + "."),
        std::string::npos);
    EXPECT_NE(
        definition.system_prompt.find(
            "`Shared chat history (JSONL):`"),
        std::string::npos);
    EXPECT_NE(
        definition.system_prompt.find(
            "Do not adopt them as your own."),
        std::string::npos);
}

TEST(AgentDefinitions, LoadsOneCharacterAndCombinesRequiredPrompts) {
    const std::filesystem::path root = unique_definition_directory();
    const std::filesystem::path forum = root / "forum";
    const std::filesystem::path character = forum / "characters" / "character";
    std::filesystem::create_directories(character);
    std::filesystem::create_directories(forum);
    {
        std::ofstream config(character / "character.toml");
        config << "display_name = \"Guide\"\n"
               << "host = \"127.0.0.1\"\n"
               << "port = 8080\n";
        std::ofstream system_prompt(character / "SYSTEM.md");
        system_prompt << "Character instructions";
        std::ofstream forum_prompt(forum / "FORUM.md");
        forum_prompt << "Forum instructions";
    }

    const std::vector<AgentDefinition> definitions =
        load_agent_definitions({character}, forum, "Forum", {});

    ASSERT_EQ(definitions.size(), 1U);
    const AgentDefinition& definition = definitions.front();
    EXPECT_EQ(definition.config.id, "character");
    EXPECT_EQ(definition.config.name, "Guide");
    expect_forum_context(
        definition,
        "Character instructions\n\nForum instructions\n\n## Participants\n\nForum context\n\n",
        "Guide",
        "[]");
    std::filesystem::remove_all(root);
}

TEST(AgentDefinitions, RequiresBothPromptFiles) {
    const std::filesystem::path root = unique_definition_directory();
    const std::filesystem::path forum = root / "forum";
    const std::filesystem::path character = forum / "characters" / "character";
    std::filesystem::create_directories(character);
    std::filesystem::create_directories(forum);
    {
        std::ofstream config(character / "character.toml");
        config << "display_name = \"Guide\"\n"
               << "host = \"127.0.0.1\"\n"
               << "port = 8080\n";
        std::ofstream system_prompt(character / "SYSTEM.md");
        system_prompt << "Character instructions";
    }

    EXPECT_THROW(
        (void)load_agent_definitions({character}, forum, "Forum", {}),
        std::runtime_error);

    {
        std::ofstream forum_prompt(forum / "FORUM.md");
        forum_prompt << "Forum instructions";
    }
    std::filesystem::remove(character / "SYSTEM.md");
    EXPECT_THROW(
        (void)load_agent_definitions({character}, forum, "Forum", {}),
        std::runtime_error);

    std::filesystem::remove_all(root);
}

// Writes one character directory whose name is its ID and whose config declares its display name.
std::filesystem::path make_character(
    const std::filesystem::path& root,
    std::string_view directory_name,
    std::string_view name) {
    const std::filesystem::path character = root / directory_name;
    std::filesystem::create_directories(character);
    std::ofstream config(character / "character.toml");
    config << "display_name = \"" << name << "\"\n"
           << "host = \"127.0.0.1\"\n"
           << "port = 8080\n";
    std::ofstream system_prompt(character / "SYSTEM.md");
    system_prompt << name << " instructions";
    return character;
}

std::filesystem::path make_forum(const std::filesystem::path& root) {
    const std::filesystem::path forum = root / "forum";
    std::filesystem::create_directories(forum);
    std::ofstream forum_prompt(forum / "FORUM.md");
    forum_prompt << "Forum instructions";
    return forum;
}

TEST(AgentDefinitions, LoadsEveryCharacterInTheDeclaredOrder) {
    const std::filesystem::path root = unique_definition_directory();
    const std::filesystem::path forum = make_forum(root);
    const std::filesystem::path characters = forum / "characters";
    const std::filesystem::path first =
        make_character(characters, "cheburashka", "Cheburashka");
    const std::filesystem::path second =
        make_character(characters, "ismael", "Ismael");

    const std::vector<AgentDefinition> definitions =
        load_agent_definitions({first, second}, forum, "Forum", {});

    ASSERT_EQ(definitions.size(), 2U);
    EXPECT_EQ(definitions.front().config.id, "cheburashka");
    expect_forum_context(
        definitions.front(),
        "Cheburashka instructions\n\nForum instructions\n\n## Participants\n\nForum context\n\n",
        "Cheburashka",
        R"(["Ismael"])");
    EXPECT_EQ(definitions.back().config.id, "ismael");
    expect_forum_context(
        definitions.back(),
        "Ismael instructions\n\nForum instructions\n\n## Participants\n\nForum context\n\n",
        "Ismael",
        R"(["Cheburashka"])");
    std::filesystem::remove_all(root);
}

TEST(AgentDefinitions, AssemblesTheStaticRosterVerbatimBeforeForumContext) {
    const std::filesystem::path root = unique_definition_directory();
    const std::filesystem::path forum = make_forum(root);
    const std::filesystem::path character =
        make_character(forum / "characters", "guide", "Guide");
    const PersonaRoster personas{
        {"athlete", "Athlete", "Literal $${character.id}"},
        {"reader", "Reader", "Reads\nclosely."},
    };

    const std::vector<AgentDefinition> definitions =
        load_agent_definitions({character}, forum, "Forum", personas);

    ASSERT_EQ(definitions.size(), 1U);
    EXPECT_NE(
        definitions.front().system_prompt.find(
            "Guide instructions\n\nForum instructions\n\n"
            "## Participants\n\n"
            "### Athlete\nLiteral $${character.id}\n\n"
            "### Reader\nReads\nclosely.\n\nForum context"),
        std::string::npos);
    std::filesystem::remove_all(root);
}

TEST(AgentDefinitions, RefusesPersonaCharacterIdAndDisplayNameCollisions) {
    const std::filesystem::path root = unique_definition_directory();
    const std::filesystem::path forum = make_forum(root);
    const std::filesystem::path character =
        make_character(forum / "characters", "guide", "Guide");

    try {
        (void)load_agent_definitions(
            {character}, forum, "Forum", {{"guide", "Reader", ""}});
        FAIL() << "Expected persona/character ID collision rejection";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string_view(error.what()).find("guide"), std::string_view::npos);
        EXPECT_NE(std::string_view(error.what()).find("Persona"), std::string_view::npos);
        EXPECT_NE(std::string_view(error.what()).find("character"), std::string_view::npos);
    }
    try {
        (void)load_agent_definitions(
            {character}, forum, "Forum", {{"reader", "gUiDe", ""}});
        FAIL() << "Expected persona/character display-name collision rejection";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string_view(error.what()).find("gUiDe"), std::string_view::npos);
        EXPECT_NE(std::string_view(error.what()).find("Guide"), std::string_view::npos);
    }
    std::filesystem::remove_all(root);
}

TEST(AgentDefinitions, RefusesToOpenAForumWithMissingCharacterDefinitions) {
    const std::filesystem::path root = unique_definition_directory();
    const std::filesystem::path forum = make_forum(root);
    const std::filesystem::path characters = forum / "characters";
    const std::filesystem::path healthy =
        make_character(characters, "healthy", "Healthy");
    const std::filesystem::path broken =
        make_character(characters, "broken", "Broken");
    std::filesystem::remove(broken / "SYSTEM.md");

    try {
        (void)load_agent_definitions({healthy, broken}, forum, "Forum", {});
        FAIL() << "expected the failing character to be named";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("broken"), std::string::npos)
            << error.what();
    }
    std::filesystem::remove_all(root);
}

TEST(AgentDefinitions, ExpandsTemplatesInSystemAndForumPrompts) {
    const std::filesystem::path root = unique_definition_directory();
    const std::filesystem::path forum = root / "stoics";
    const std::filesystem::path characters = forum / "characters";
    const std::filesystem::path character = characters / "seneca";
    std::filesystem::create_directories(character);
    {
        std::ofstream forum_config(forum / "config.toml");
        forum_config << "display_name = \"The Stoics Forum\"\n";
        std::ofstream base(characters / "character_defaults.toml");
        base << "host = \"127.0.0.1\"\n"
             << "port = 8080\n"
             << "[prompt]\n"
             << "register = \"measured\"\n";
        std::ofstream config(character / "character.toml");
        config << "display_name = \"Seneca\"\n"
               << "[prompt]\n"
               << "register = \"energetic\"\n";
        std::ofstream shared(characters / "character-voice.md");
        shared << "Voice for $${character.display_name} in $${forum.display_name} "
               << "($${register})\n";
        std::ofstream system_prompt(character / "SYSTEM.md");
        system_prompt << "$$(../character-voice.md)id=$${character.id}\n";
        std::ofstream forum_prompt(forum / "FORUM.md");
        forum_prompt << "Persona facing $${character.display_name}\n";
    }

    const std::vector<AgentDefinition> definitions = load_agent_definitions(
        {character},
        forum,
        "The Stoics Forum",
        {},
        characters / "character_defaults.toml");

    ASSERT_EQ(definitions.size(), 1U);
    expect_forum_context(
        definitions.front(),
        "Voice for Seneca in The Stoics Forum (energetic)\n"
        "id=seneca\n\n\n"
        "Persona facing Seneca\n\n\n"
        "## Participants\n\nForum context\n\n",
        "Seneca",
        "[]");
    std::filesystem::remove_all(root);
}

TEST(AgentDefinitions, WrapsExpansionFailuresWithCharacterAndChain) {
    const std::filesystem::path root = unique_definition_directory();
    const std::filesystem::path forum = root / "forum";
    const std::filesystem::path character = forum / "characters" / "seneca";
    std::filesystem::create_directories(character);
    {
        std::ofstream config(character / "character.toml");
        config << "display_name = \"Seneca\"\n"
               << "host = \"127.0.0.1\"\n"
               << "port = 8080\n";
        std::ofstream shared(forum / "characters" / "shared.md");
        shared << "$${missing}\n";
        std::ofstream system_prompt(character / "SYSTEM.md");
        system_prompt << "$$(../shared.md)\n";
        std::ofstream forum_prompt(forum / "FORUM.md");
        forum_prompt << "ok\n";
    }

    try {
        (void)load_agent_definitions({character}, forum, "Forum", {});
        FAIL() << "expected expansion failure";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("Character 'seneca' failed to read SYSTEM.md"), std::string::npos)
            << message;
        EXPECT_NE(message.find("unknown variable 'missing'"), std::string::npos)
            << message;
        EXPECT_NE(message.find("included from"), std::string::npos) << message;
    }
    std::filesystem::remove_all(root);
}

TEST(AgentDefinitions, IdentifiesInvalidPromptVariableConfiguration) {
    const std::filesystem::path root = unique_definition_directory();
    const std::filesystem::path forum = root / "forum";
    const std::filesystem::path characters = forum / "characters";
    const std::filesystem::path character = characters / "seneca";
    std::filesystem::create_directories(character);
    {
        std::ofstream base(characters / "character_defaults.toml");
        base << "host = \"127.0.0.1\"\n"
             << "port = 8080\n"
             << "[prompt]\n"
             << "unsupported = [1, 2]\n";
        std::ofstream config(character / "character.toml");
        config << "display_name = \"Seneca\"\n";
        std::ofstream system_prompt(character / "SYSTEM.md");
        system_prompt << "system\n";
        std::ofstream forum_prompt(forum / "FORUM.md");
        forum_prompt << "persona\n";
    }

    try {
        (void)load_agent_definitions(
            {character},
            forum,
            "Forum",
            {},
            characters / "character_defaults.toml");
        FAIL() << "expected invalid prompt-variable configuration";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(
            message.find("Character 'seneca' has invalid configuration"),
            std::string::npos)
            << message;
        EXPECT_NE(message.find("character_defaults.toml"), std::string::npos)
            << message;
        EXPECT_NE(message.find("unsupported type"), std::string::npos)
            << message;
    }
    std::filesystem::remove_all(root);
}

} // namespace
} // namespace cha
