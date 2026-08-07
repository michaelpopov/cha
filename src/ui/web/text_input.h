#pragma once

#include "ui/web/protocol.h"

#include <string>
#include <string_view>

namespace cha {

class SessionController;

} // namespace cha

namespace cha::web {

// Translates the web chat grammar into typed session-layer calls and the
// command result returned by the HTTP endpoint.
[[nodiscard]] CommandResult handle_text_input(
    SessionController& controller,
    std::string_view author_id,
    std::string input);

} // namespace cha::web
