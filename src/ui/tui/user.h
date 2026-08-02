#pragma once

#include "session/workspace.h"

namespace cha {

class SessionController;
class Terminal;
class UvEventLoop;

// A free function keeps the top-level user workflow stateless and easy to compose in main.
void run_user(
    Terminal& terminal,
    SessionController& controller,
    UvEventLoop& event_loop,
    User user);

} // namespace cha
