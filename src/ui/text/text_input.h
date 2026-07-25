#pragma once

#include "session/session_controller.h"

#include <string>

namespace cha {

// Translates the shared textual command grammar into typed session-layer calls.
// Return value carries render/end/clear/notice side effects the UI must apply.
[[nodiscard]] SessionUpdate handle_text_input(
    SessionController& controller,
    std::string input);

} // namespace cha
