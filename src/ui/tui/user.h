#pragma once

namespace cha {

class SessionController;
class Terminal;

// A free function keeps the top-level user workflow stateless and easy to compose in main.
void run_user(
    Terminal& terminal,
    SessionController& controller);

} // namespace cha
