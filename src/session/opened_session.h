#pragma once

#include "session/session_controller.h"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace cha {

// Everything prepared while opening a stored session for its owner thread.
struct OpenedSession {
    std::string label;
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
