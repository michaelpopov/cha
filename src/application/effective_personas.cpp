#include "application/effective_personas.h"

#include "application/builtins.h"
#include "util/text.h"

#include <algorithm>

namespace cha {
EffectivePersonas::EffectivePersonas(const WorkspaceSnapshot& snapshot) {
    PersonaRoster values;
    values.push_back(builtin_guest());
    for (const Persona& persona : snapshot.personas()) values.push_back(persona);
    std::sort(values.begin() + 1, values.end(), [](const Persona& left, const Persona& right) {
        return fold_ascii(left.display_name) < fold_ascii(right.display_name);
    });
    roster_ = std::make_shared<const PersonaRoster>(std::move(values));
}

const Persona* EffectivePersonas::find(std::string_view public_name) const {
    const std::string folded = fold_ascii(public_name);
    for (const Persona& persona : *roster_) {
        if (fold_ascii(persona.display_name) == folded) return &persona;
    }
    return nullptr;
}

std::vector<std::string> EffectivePersonas::custom_names() const {
    std::vector<std::string> names;
    for (const Persona& persona : *roster_) {
        if (persona.id != builtin_guest().id) names.push_back(persona.display_name);
    }
    return names;
}
} // namespace cha
