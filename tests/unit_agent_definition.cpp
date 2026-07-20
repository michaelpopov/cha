#include "agent_definition.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>

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

} // namespace
} // namespace cha
