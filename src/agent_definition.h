#pragma once

#include "config.h"

#include <filesystem>
#include <string>
#include <vector>

namespace cha {

// Holds the complete startup configuration and effective prompt for one agent.
struct AgentDefinition {
    Config config;
    std::string system_prompt;
};

// Loads persona configuration and combines its system prompt with room instructions.
[[nodiscard]] AgentDefinition load_agent_definition(
    const std::filesystem::path& persona_directory,
    const std::filesystem::path& room_directory);

[[nodiscard]] std::vector<AgentDefinition> load_agent_definitions(
    const std::vector<std::filesystem::path>& persona_directories,
    const std::filesystem::path& room_directory);

} // namespace cha
