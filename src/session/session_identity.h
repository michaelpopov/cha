#pragma once

#include "chat/session_identity.h"

#include <string>

namespace cha {

// Presentation-safe metadata that remains valid for an opened live session.
struct SessionDescriptor {
    SessionIdentity identity;
    std::string forum_display_name;
    std::string session_label;
    CharacterId forum_default_character_id;
    // No persona here: a session's current persona is controller state that
    // /!Name changes, so it is read from the controller view instead.

    bool operator==(const SessionDescriptor&) const = default;
};

} // namespace cha
