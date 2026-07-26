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

TEST(AgentDefinitions, LoadsOnePersonaAndCombinesRequiredPrompts) {
    const std::filesystem::path root = unique_definition_directory();
    const std::filesystem::path persona = root / "persona";
    const std::filesystem::path forum = root / "forum";
    std::filesystem::create_directories(persona);
    std::filesystem::create_directories(forum);
    {
        std::ofstream config(persona / "config.toml");
        config << "id = \"guide-id\"\n"
               << "name = \"Guide\"\n"
               << "host = \"127.0.0.1\"\n"
               << "port = 8080\n";
        std::ofstream system_prompt(persona / "SYSTEM.md");
        system_prompt << "Persona instructions";
        std::ofstream forum_prompt(forum / "USER.md");
        forum_prompt << "Forum instructions";
    }

    const std::vector<AgentDefinition> definitions =
        load_agent_definitions({persona}, forum);

    ASSERT_EQ(definitions.size(), 1U);
    const AgentDefinition& definition = definitions.front();
    EXPECT_EQ(definition.config.id, "guide-id");
    EXPECT_EQ(definition.config.name, "Guide");
    expect_forum_context(
        definition,
        "Persona instructions\n\nForum instructions\n\nForum context\n\n",
        "Guide",
        "[]");
    std::filesystem::remove_all(root);
}

TEST(AgentDefinitions, RequiresBothPromptFiles) {
    const std::filesystem::path root = unique_definition_directory();
    const std::filesystem::path persona = root / "persona";
    const std::filesystem::path forum = root / "forum";
    std::filesystem::create_directories(persona);
    std::filesystem::create_directories(forum);
    {
        std::ofstream config(persona / "config.toml");
        config << "id = \"guide-id\"\n"
               << "name = \"Guide\"\n"
               << "host = \"127.0.0.1\"\n"
               << "port = 8080\n";
        std::ofstream system_prompt(persona / "SYSTEM.md");
        system_prompt << "Persona instructions";
    }

    EXPECT_THROW(
        (void)load_agent_definitions({persona}, forum),
        std::runtime_error);

    {
        std::ofstream forum_prompt(forum / "USER.md");
        forum_prompt << "Forum instructions";
    }
    std::filesystem::remove(persona / "SYSTEM.md");
    EXPECT_THROW(
        (void)load_agent_definitions({persona}, forum),
        std::runtime_error);

    std::filesystem::remove_all(root);
}

// Writes one persona directory whose config declares the given id and name.
std::filesystem::path make_persona(
    const std::filesystem::path& root,
    std::string_view directory_name,
    std::string_view id,
    std::string_view name) {
    const std::filesystem::path persona = root / directory_name;
    std::filesystem::create_directories(persona);
    std::ofstream config(persona / "config.toml");
    config << "id = \"" << id << "\"\n"
           << "name = \"" << name << "\"\n"
           << "host = \"127.0.0.1\"\n"
           << "port = 8080\n";
    std::ofstream system_prompt(persona / "SYSTEM.md");
    system_prompt << name << " instructions";
    return persona;
}

std::filesystem::path make_forum(const std::filesystem::path& root) {
    const std::filesystem::path forum = root / "forum";
    std::filesystem::create_directories(forum);
    std::ofstream forum_prompt(forum / "USER.md");
    forum_prompt << "Forum instructions";
    return forum;
}

TEST(AgentDefinitions, LoadsEveryPersonaInTheDeclaredOrder) {
    const std::filesystem::path root = unique_definition_directory();
    const std::filesystem::path forum = make_forum(root);
    const std::filesystem::path first = make_persona(root, "cheburashka", "cheburashka", "Cheburashka");
    const std::filesystem::path second = make_persona(root, "ismael", "ismael", "Ismael");

    const std::vector<AgentDefinition> definitions =
        load_agent_definitions({first, second}, forum);

    ASSERT_EQ(definitions.size(), 2U);
    EXPECT_EQ(definitions.front().config.id, "cheburashka");
    expect_forum_context(
        definitions.front(),
        "Cheburashka instructions\n\nForum instructions\n\nForum context\n\n",
        "Cheburashka",
        R"(["Ismael"])");
    EXPECT_EQ(definitions.back().config.id, "ismael");
    expect_forum_context(
        definitions.back(),
        "Ismael instructions\n\nForum instructions\n\nForum context\n\n",
        "Ismael",
        R"(["Cheburashka"])");
    std::filesystem::remove_all(root);
}

TEST(AgentDefinitions, RefusesToOpenAForumWithMissingPersonaDefinitions) {
    const std::filesystem::path root = unique_definition_directory();
    const std::filesystem::path forum = make_forum(root);
    const std::filesystem::path healthy = make_persona(root, "healthy", "healthy", "Healthy");
    const std::filesystem::path broken = make_persona(root, "broken", "broken", "Broken");
    std::filesystem::remove(broken / "SYSTEM.md");

    try {
        (void)load_agent_definitions({healthy, broken}, forum);
        FAIL() << "expected the failing persona to be named";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("broken"), std::string::npos)
            << error.what();
    }
    std::filesystem::remove_all(root);
}

} // namespace
} // namespace cha
