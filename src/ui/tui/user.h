#pragma once

namespace cha {

class SessionController;
class Terminal;

// The outcome of one wait in the terminal event loop: whether the wait was interrupted or failed,
// and which sources are ready — keyboard input, a closed terminal, or a pending agent event. It
// exists so file-descriptor handling stays inside wait_for_user_events(), its only producer, and
// the loop above reads semantic conditions instead of poll flags.
class UserEvents {
public:
    bool interrupted() const;
    bool failed() const;
    bool terminal_input_ready() const;
    bool terminal_closed() const;
    bool agent_event_ready() const;

private:
    enum class Status {
        ready,
        interrupted,
        failed,
    };

    explicit UserEvents(
        Status status,
        bool terminal_input = false,
        bool terminal_closed = false,
        bool agent_event = false);

    Status _status;
    bool _terminal_input;
    bool _terminal_closed;
    bool _agent_event;

    friend UserEvents wait_for_user_events(int agent_notification_fd);
};

UserEvents wait_for_user_events(int agent_notification_fd);

// A free function keeps the top-level user workflow stateless and easy to compose in main.
void run_user(
    Terminal& terminal,
    SessionController& controller);

} // namespace cha
