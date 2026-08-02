#pragma once

#include "session/session_update.h"

#include <string>
#include <string_view>

namespace cha {

class SessionController;

// Translates the shared textual command grammar into typed session-layer calls.
// Return value carries render/end/clear/notice side effects the UI must apply.
[[nodiscard]] SessionUpdate handle_text_input(
    SessionController& controller,
    std::string_view author_id,
    std::string input);

} // namespace cha
