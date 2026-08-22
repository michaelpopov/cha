#pragma once

#include "agents/character.h"
#include "chat/persona.h"

#include <string_view>
#include <vector>

namespace cha {

inline constexpr std::string_view guest_name = "Guest";
inline constexpr std::string_view assistant_name = "Assistant";
inline constexpr std::string_view entrance_name = "Entrance";
inline constexpr std::string_view welcome_name = "Welcome";
inline constexpr std::string_view guest_id = "builtin-guest";
inline constexpr std::string_view assistant_id = "builtin-assistant";
inline constexpr std::string_view entrance_id = "builtin-entrance";
inline constexpr std::string_view welcome_id = "builtin-welcome";

const Persona& builtin_guest();
std::string_view application_guide();
std::vector<CharacterDefinition> builtin_assistant_definitions(
    ProviderSelection provider,
    const std::string& inventory,
    const PersonaRoster& personas);

} // namespace cha
