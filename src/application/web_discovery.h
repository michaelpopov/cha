#pragma once

#include "agents/config.h"
#include "application/effective_personas.h"
#include "application/workspace_inventory.h"
#include "session/workspace.h"

#include <string_view>
#include <vector>

namespace cha {

// Immutable startup discovery for the browser surface. It wraps the one
// validated workspace snapshot and adds the application built-ins without
// rescanning the workspace.
class WebDiscovery {
public:
    explicit WebDiscovery(const Workspace& workspace);

    const WorkspaceSnapshot& workspace() const noexcept { return workspace_; }
    const EffectivePersonas& effective_personas() const noexcept { return personas_; }
    const WorkspaceInventory& inventory() const noexcept { return inventory_; }
    const PersonaRoster& personas() const noexcept { return *personas_.roster(); }
    const std::vector<CharacterDefinitionMetadata>& characters() const noexcept { return characters_; }
    const std::vector<Forum>& forums() const noexcept { return forums_; }
    const Persona* find_persona(std::string_view id) const;
    const CharacterDefinitionMetadata* find_character(std::string_view id) const;
    const Forum* find_forum(std::string_view id) const;

private:
    WorkspaceSnapshot workspace_;
    EffectivePersonas personas_;
    WorkspaceInventory inventory_;
    std::vector<CharacterDefinitionMetadata> characters_;
    std::vector<Forum> forums_;
};

} // namespace cha
