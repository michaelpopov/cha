#pragma once

#include "chat/ids.h"

#include <string>

namespace cha {

// Identifies one validated stored session without exposing its storage path.
struct SessionIdentity {
    ForumId forum_id;
    SessionId session_id;

    bool operator==(const SessionIdentity&) const = default;
    bool operator<(const SessionIdentity& other) const noexcept {
        return forum_id == other.forum_id
            ? session_id < other.session_id
            : forum_id < other.forum_id;
    }
};

// Presentation-safe metadata that remains valid for an opened live session.
struct SessionDescriptor {
    SessionIdentity identity;
    std::string forum_display_name;
    std::string session_label;
    CharacterId forum_default_character_id;
    std::string forum_default_persona_id;

    bool operator==(const SessionDescriptor&) const = default;
};

} // namespace cha
