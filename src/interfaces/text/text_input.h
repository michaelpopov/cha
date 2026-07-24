#pragma once

#include "application/chat_coordinator.h"

#include <string>

namespace cha {

// Translates the shared textual command grammar into structured application calls.
// Return value carries render/end/clear/notice side effects the UI must apply.
[[nodiscard]] CoordinatorUpdate handle_text_input(
    ChatCoordinator& coordinator,
    std::string input);

} // namespace cha
