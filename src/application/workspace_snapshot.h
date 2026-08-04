#pragma once

#include "agents/config.h"
#include "agents/persona.h"
#include "session/workspace.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cha {

// Immutable public-name view of one already-validated workspace. Private
// storage keys remain available only on resolved values for internal opening.
class WorkspaceSnapshot {
public:
    explicit WorkspaceSnapshot(const Workspace& workspace);

    const PersonaRoster& personas() const noexcept { return personas_; }
    const std::vector<CharacterDefinitionMetadata>& characters() const noexcept { return characters_; }
    const std::vector<Forum>& forums() const noexcept { return forums_; }
    const Persona* find_persona(std::string_view public_name) const;
    const Forum* find_forum(std::string_view public_name) const;

private:
    PersonaRoster personas_;
    std::vector<CharacterDefinitionMetadata> characters_;
    std::vector<Forum> forums_;
    std::unordered_map<std::string, std::size_t> persona_index_;
    std::unordered_map<std::string, std::size_t> forum_index_;
};

} // namespace cha
