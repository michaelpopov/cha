#pragma once

#include "session/session_controller.h"
#include "session/session_identity.h"

#include <memory>

namespace cha {

// Couples the identity of an opened stored session to its owner-thread-only
// controller. The descriptor may be copied separately for presentation.
struct OpenedSession {
    SessionDescriptor descriptor;
    std::unique_ptr<SessionController> controller;
};

} // namespace cha
