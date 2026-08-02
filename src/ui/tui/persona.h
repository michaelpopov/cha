#pragma once

#include <string>

namespace cha {

class SessionController;
class Terminal;
class UvEventLoop;

// A free function keeps the top-level persona workflow stateless and easy to compose in main.
void run_persona(
    Terminal& terminal,
    SessionController& controller,
    UvEventLoop& event_loop,
    std::string author_id);

} // namespace cha
