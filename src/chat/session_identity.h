#pragma once

#include "chat/ids.h"

namespace cha {

// Identifies one validated stored session without exposing its storage path.
struct FullSessionId {
    ForumId forum_id;
    SessionId session_id;

    bool operator==(const FullSessionId&) const = default;
    bool operator<(const FullSessionId& other) const noexcept {
        return forum_id == other.forum_id
            ? session_id < other.session_id
            : forum_id < other.forum_id;
    }
};

} // namespace cha
