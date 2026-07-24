#pragma once

#include "application/chat_coordinator.h"

#include <string>

namespace cha {

// Translates the shared textual command grammar into structured application calls.
[[nodiscard]] CoordinatorUpdate handle_text_input(
    ChatCoordinator& coordinator,
    std::string input);

} // namespace cha
