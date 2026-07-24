#pragma once

#include "agents/agent_definition.h"

#include <filesystem>
#include <vector>

namespace cha {

// Loads persona configuration and combines its system prompt with room instructions.
[[nodiscard]] AgentDefinition load_agent_definition(
    const std::filesystem::path& persona_directory,
    const std::filesystem::path& room_directory);

[[nodiscard]] std::vector<AgentDefinition> load_agent_definitions(
    const std::vector<std::filesystem::path>& persona_directories,
    const std::filesystem::path& room_directory);

} // namespace cha
