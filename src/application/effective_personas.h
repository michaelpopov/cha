#pragma once

#include "agents/persona.h"
#include "application/workspace_snapshot.h"

#include <string_view>
#include <vector>

namespace cha {

class EffectivePersonas {
public:
    explicit EffectivePersonas(const WorkspaceSnapshot& snapshot);
    const SharedPersonaRoster& roster() const noexcept { return roster_; }
    const Persona* find(std::string_view public_name) const;
    std::vector<std::string> custom_names() const;

private:
    SharedPersonaRoster roster_;
};
} // namespace cha
