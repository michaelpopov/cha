#pragma once

#include <string>

namespace cha {

class SessionController;
class Terminal;
class UvEventLoop;

// A free function keeps the top-level user workflow stateless and easy to compose in main.
void run_user(
    Terminal& terminal,
    SessionController& controller,
    UvEventLoop& event_loop,
    std::string author_id);

} // namespace cha
