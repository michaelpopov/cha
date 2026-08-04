#pragma once

#include "session/session_change.h"

#include <string>
#include <string_view>

namespace cha {

class SessionController;

// Text-layer outcome: this grammar decides local widget and navigation effects
// while preserving the controller's semantic result for each frontend.
struct TextInputResult {
    SessionChange session;
    bool clear_input{};
    bool exit_requested{};
};

// Translates the shared textual command grammar into typed session-layer calls.
[[nodiscard]] TextInputResult handle_text_input(
    SessionController& controller,
    std::string_view author_id,
    std::string input);

} // namespace cha
