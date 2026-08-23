#pragma once

#include "session/session_controller.h"
#include "session/session_identity.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace cha {

// Couples the identity of an opened stored session to its owner-thread-only
// controller. The descriptor may be copied separately for presentation.
struct OpenedSession {
    SessionDescriptor descriptor;
    std::unique_ptr<SessionController> controller;
    // Saves a changed default character to the forum's config, on the owner
    // thread. Every forum gets one; built-in forums have no config file, so the
    // call fails there and the session reports the change as unsaved.
    std::function<void(std::string_view)> persist_default_character;
    std::function<void(std::string_view)> persist_default_persona;
    // Optional initial presentation notice. Production currently leaves this
    // empty; test openers use it to exercise startup-notice behavior.
    std::optional<std::string> notice;
};

} // namespace cha
