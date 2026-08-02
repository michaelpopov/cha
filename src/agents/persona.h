#pragma once

#include <string>
#include <vector>

namespace cha {

// One workspace persona. prompt is the verbatim contents of PERSONA.md.
struct Persona {
    std::string id;
    std::string display_name;
    std::string prompt;
};

// Every workspace persona in lexicographic ID order.
using PersonaRoster = std::vector<Persona>;

} // namespace cha
