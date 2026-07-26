#pragma once

namespace cha {

// The outcome of one wait: whether it was interrupted or failed, and which
// sources are ready — standard input, closed input, a pending notification, or
// a pending signal.
class InputEvents {
public:
    static InputEvents ready(
        bool input = false,
        bool input_closed = false,
        bool notification = false,
        bool signal = false);
    static InputEvents failure();

    bool interrupted() const;
    bool failed() const;
    bool input_ready() const;
    bool input_closed() const;
    bool notification_ready() const;
    bool signal_ready() const;

private:
    enum class Status {
        ready,
        interrupted,
        failed,
    };

    explicit InputEvents(
        Status status,
        bool input = false,
        bool input_closed = false,
        bool notification = false,
        bool signal = false);

    Status status_;
    bool input_;
    bool input_closed_;
    bool notification_;
    bool signal_;

    friend InputEvents wait_for_input_events(
        int notification_fd,
        int signal_fd,
        bool include_input);
};

// Waits for standard input, a notification descriptor, and, when non-negative,
// a signal descriptor.
InputEvents wait_for_input_events(
    int notification_fd,
    int signal_fd = -1,
    bool include_input = true);

} // namespace cha
