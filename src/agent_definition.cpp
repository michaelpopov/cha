#include "agent_definition.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace cha {
namespace {

std::string read_prompt(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to read prompt file '" + path.string() + "'");
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

} // namespace

AgentDefinition load_agent_definition(
    const std::filesystem::path& persona_directory,
    const std::filesystem::path& room_directory) {
    const std::string persona_name = persona_directory.filename().string();
    Config config;
    try {
        config = Config::load(persona_directory / "config.toml");
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "Persona '" + persona_name
            + "' has invalid configuration: " + error.what());
    }
    std::string persona_prompt;
    try {
        persona_prompt = read_prompt(persona_directory / "SYSTEM.md");
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "Persona '" + persona_name
            + "' failed to read SYSTEM.md: " + error.what());
    }
    std::string room_prompt;
    try {
        room_prompt = read_prompt(room_directory / "USER.md");
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "Room '" + room_directory.filename().string()
            + "' failed to read USER.md: " + error.what());
    }
    return {
        .config = std::move(config),
        .system_prompt = std::move(persona_prompt)
            + "\n\n" + std::move(room_prompt),
    };
}

std::vector<AgentDefinition> load_agent_definitions(
    const std::vector<std::filesystem::path>& persona_directories,
    const std::filesystem::path& room_directory) {
    std::vector<AgentDefinition> definitions;
    definitions.reserve(persona_directories.size());
    for (const auto& directory : persona_directories) {
        definitions.push_back(
            load_agent_definition(directory, room_directory));
    }
    return definitions;
}

} // namespace cha
