#pragma once

#include "agents/agent.h"

#include <string>
#include <string_view>
#include <vector>

namespace cha {

enum class HandleMatch { resolved, unknown, ambiguous };

// The outcome of resolving a handle typed by the user.
struct HandleResolution {
    HandleMatch match{HandleMatch::unknown};
    const PersonaInfo* persona{};
    std::vector<const PersonaInfo*> candidates;
};

// The ordered personas participating in one room. Construction guarantees that the collection is
// non-empty and that persona IDs and case-folded names are unique.
class RoomPersonas {
public:
    explicit RoomPersonas(std::vector<PersonaInfo> personas);

    const std::vector<PersonaInfo>& all() const noexcept;
    const PersonaInfo& first() const;
    const PersonaInfo* find(std::string_view id) const;
    HandleResolution resolve_handle(std::string_view handle) const;
    std::string handle_list() const;

private:
    std::vector<PersonaInfo> personas_;
};

} // namespace cha
