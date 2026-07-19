#pragma once

namespace cha {

class Pipe;

// Present platform input readiness as application-level events to keep callers independent of poll details.
class UserEvents {
public:
    [[nodiscard]] bool interrupted() const;
    [[nodiscard]] bool failed() const;
    [[nodiscard]] bool terminal_input_ready() const;
    [[nodiscard]] bool terminal_closed() const;
    [[nodiscard]] bool pipe_input_ready() const;

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
        bool pipe_input = false);

    Status _status;
    bool _terminal_input;
    bool _terminal_closed;
    bool _pipe_input;

    friend UserEvents wait_for_user_events(const Pipe& pipe_in);
};

[[nodiscard]] UserEvents wait_for_user_events(const Pipe& pipe_in);

} // namespace cha
