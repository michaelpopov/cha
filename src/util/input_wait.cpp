#include "util/input_wait.h"

#include <cerrno>
#include <poll.h>
#include <unistd.h>

namespace cha {

InputEvents::InputEvents(
    Status status,
    bool input,
    bool input_closed,
    bool notification,
    bool signal)
    : status_(status),
      input_(input),
      input_closed_(input_closed),
      notification_(notification),
      signal_(signal) {
}

InputEvents InputEvents::ready(
    bool input,
    bool input_closed,
    bool notification,
    bool signal) {
    return InputEvents(
        Status::ready,
        input,
        input_closed,
        notification,
        signal);
}

InputEvents InputEvents::failure() {
    return InputEvents(Status::failed);
}

bool InputEvents::interrupted() const {
    return status_ == Status::interrupted;
}

bool InputEvents::failed() const {
    return status_ == Status::failed;
}

bool InputEvents::input_ready() const {
    return input_;
}

bool InputEvents::input_closed() const {
    return input_closed_;
}

bool InputEvents::notification_ready() const {
    return notification_;
}

bool InputEvents::signal_ready() const {
    return signal_;
}

InputEvents wait_for_input_events(
    int notification_fd,
    int signal_fd,
    bool include_input) {
    pollfd descriptors[] = {
        {include_input ? STDIN_FILENO : -1, POLLIN, 0},
        {notification_fd, POLLIN, 0},
        {signal_fd, POLLIN, 0},
    };
    const nfds_t count = signal_fd < 0 ? 2 : 3;

    if (::poll(descriptors, count, -1) == -1) {
        const InputEvents::Status status =
            errno == EINTR
            ? InputEvents::Status::interrupted
            : InputEvents::Status::failed;
        return InputEvents(status);
    }

    return InputEvents(
        InputEvents::Status::ready,
        (descriptors[0].revents & POLLIN) != 0,
        (descriptors[0].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0,
        (descriptors[1].revents & POLLIN) != 0,
        signal_fd >= 0 && (descriptors[2].revents & POLLIN) != 0);
}

} // namespace cha
