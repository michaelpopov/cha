#pragma once

namespace cha {

// Represents terminal and agent-event readiness returned by the user-event polling boundary.
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

} // namespace cha
