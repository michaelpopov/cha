#pragma once

#include "chat/session_identity.h"

#include <cstdint>
#include <string>

namespace cha {

// One immutable observation of an active session row. updated_at is Unix time
// in seconds, matching the value stored in the workspace database and exposed
// by the web protocol.
struct StoredSession {
    FullSessionId identity;
    std::string label;
    std::int64_t updated_at{};

    bool operator==(const StoredSession&) const = default;
};

} // namespace cha
