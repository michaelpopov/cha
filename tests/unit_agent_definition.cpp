#include "agent_definition.h"

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

TEST(AgentDefinition, LoadsConfigAndCombinesRequiredPrompts) {
    const std::filesystem::path root = unique_definition_directory();
    const std::filesystem::path persona = root / "persona";
    const std::filesystem::path room = root / "room";
    std::filesystem::create_directories(persona);
    std::filesystem::create_directories(room);
    {
        std::ofstream config(persona / "config.toml");
        config << "id = \"guide-id\"\n"
               << "name = \"Guide\"\n"
               << "host = \"127.0.0.1\"\n"
               << "port = 8080\n";
        std::ofstream system_prompt(persona / "SYSTEM.md");
        system_prompt << "Persona instructions";
        std::ofstream room_prompt(room / "USER.md");
        room_prompt << "Room instructions";
    }

    const AgentDefinition definition = load_agent_definition(persona, room);

    EXPECT_EQ(definition.config.id, "guide-id");
    EXPECT_EQ(definition.config.name, "Guide");
    EXPECT_EQ(
        definition.system_prompt,
        "Persona instructions\n\nRoom instructions");
    std::filesystem::remove_all(root);
}

TEST(AgentDefinition, RequiresBothPromptFiles) {
    const std::filesystem::path root = unique_definition_directory();
    const std::filesystem::path persona = root / "persona";
    const std::filesystem::path room = root / "room";
    std::filesystem::create_directories(persona);
    std::filesystem::create_directories(room);
    {
        std::ofstream config(persona / "config.toml");
        config << "id = \"guide-id\"\n"
               << "name = \"Guide\"\n"
               << "host = \"127.0.0.1\"\n"
               << "port = 8080\n";
        std::ofstream system_prompt(persona / "SYSTEM.md");
        system_prompt << "Persona instructions";
    }

    EXPECT_THROW((void)load_agent_definition(persona, room), std::runtime_error);

    {
        std::ofstream room_prompt(room / "USER.md");
        room_prompt << "Room instructions";
    }
    std::filesystem::remove(persona / "SYSTEM.md");
    EXPECT_THROW((void)load_agent_definition(persona, room), std::runtime_error);

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

std::filesystem::path make_room(const std::filesystem::path& root) {
    const std::filesystem::path room = root / "room";
    std::filesystem::create_directories(room);
    std::ofstream room_prompt(room / "USER.md");
    room_prompt << "Room instructions";
    return room;
}

TEST(AgentDefinitions, LoadsEveryPersonaInTheDeclaredOrder) {
    const std::filesystem::path root = unique_definition_directory();
    const std::filesystem::path room = make_room(root);
    const std::filesystem::path first = make_persona(root, "cheburashka", "cheburashka", "Cheburashka");
    const std::filesystem::path second = make_persona(root, "ismael", "ismael", "Ismael");

    const std::vector<AgentDefinition> definitions =
        load_agent_definitions({first, second}, room);

    ASSERT_EQ(definitions.size(), 2U);
    EXPECT_EQ(definitions.front().config.id, "cheburashka");
    EXPECT_EQ(definitions.front().system_prompt, "Cheburashka instructions\n\nRoom instructions");
    EXPECT_EQ(definitions.back().config.id, "ismael");
    EXPECT_EQ(definitions.back().system_prompt, "Ismael instructions\n\nRoom instructions");
    std::filesystem::remove_all(root);
}

TEST(AgentDefinitions, RejectsTwoPersonasDeclaringTheSameAgentId) {
    const std::filesystem::path root = unique_definition_directory();
    const std::filesystem::path room = make_room(root);
    const std::filesystem::path first = make_persona(root, "one", "shared", "First");
    const std::filesystem::path second = make_persona(root, "two", "shared", "Second");

    try {
        (void)load_agent_definitions({first, second}, room);
        FAIL() << "expected a duplicate agent id diagnostic";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("'one'"), std::string::npos) << message;
        EXPECT_NE(message.find("'two'"), std::string::npos) << message;
        EXPECT_NE(message.find("same agent id 'shared'"), std::string::npos) << message;
    }
    std::filesystem::remove_all(root);
}

TEST(AgentDefinitions, RejectsTwoPersonasDeclaringTheSameDisplayNameIgnoringCase) {
    const std::filesystem::path root = unique_definition_directory();
    const std::filesystem::path room = make_room(root);
    const std::filesystem::path first = make_persona(root, "one", "first-id", "Ismael");
    const std::filesystem::path second = make_persona(root, "two", "second-id", "ISMAEL");

    try {
        (void)load_agent_definitions({first, second}, room);
        FAIL() << "expected a duplicate agent name diagnostic";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("'one'"), std::string::npos) << message;
        EXPECT_NE(message.find("'two'"), std::string::npos) << message;
        EXPECT_NE(message.find("same agent name"), std::string::npos) << message;
    }
    std::filesystem::remove_all(root);
}

TEST(AgentDefinitions, RefusesToOpenARoomWithAPartialRoster) {
    const std::filesystem::path root = unique_definition_directory();
    const std::filesystem::path room = make_room(root);
    const std::filesystem::path healthy = make_persona(root, "healthy", "healthy", "Healthy");
    const std::filesystem::path broken = make_persona(root, "broken", "broken", "Broken");
    std::filesystem::remove(broken / "SYSTEM.md");

    try {
        (void)load_agent_definitions({healthy, broken}, room);
        FAIL() << "expected the failing persona to be named";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("broken"), std::string::npos)
            << error.what();
    }
    std::filesystem::remove_all(root);
}

TEST(AgentDefinitions, RequiresAtLeastOnePersona) {
    const std::filesystem::path root = unique_definition_directory();
    const std::filesystem::path room = make_room(root);

    EXPECT_THROW((void)load_agent_definitions({}, room), std::invalid_argument);
    std::filesystem::remove_all(root);
}

} // namespace
} // namespace cha
