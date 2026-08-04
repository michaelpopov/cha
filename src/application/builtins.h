#pragma once

#include "agents/agent.h"
#include "agents/persona.h"
#include "session/workspace.h"

#include <string_view>
#include <vector>

namespace cha {

inline constexpr std::string_view guest_name = "Guest";
inline constexpr std::string_view assistant_name = "Assistant";
inline constexpr std::string_view entrance_name = "Entrance";
inline constexpr std::string_view welcome_name = "Welcome";

const Persona& builtin_guest();
const Forum& builtin_entrance();
std::string_view application_guide();
std::vector<AgentDefinition> builtin_assistant_definitions(
    const ProviderConfig& provider,
    const std::string& inventory,
    const PersonaRoster& personas);

} // namespace cha
