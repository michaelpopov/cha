#pragma once

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
};

using PersonaRoster = std::vector<Persona>;
using SharedPersonaRoster = std::shared_ptr<const PersonaRoster>;

} // namespace cha
