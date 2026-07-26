#pragma once

namespace cha {

class SessionController;
class Terminal;
class EventFdNotifier;

// A free function keeps the top-level user workflow stateless and easy to compose in main.
void run_user(
    Terminal& terminal,
    SessionController& controller,
    EventFdNotifier& notifier);

} // namespace cha
