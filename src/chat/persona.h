#pragma once

#include "chat/character_metadata.h"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cha {

struct Persona {
    std::string id;
    std::string display_name;
    std::string prompt;
    std::optional<std::string> description;
    std::optional<std::string> style_id;
    std::optional<std::string> voice_id;
    CharacterAppearance appearance;
};

using PersonaRoster = std::vector<Persona>;
using SharedPersonaRoster = std::shared_ptr<const PersonaRoster>;

} // namespace cha
