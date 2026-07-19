#include "user_events.h"

#include "pipe.h"

#include <cerrno>
#include <poll.h>
#include <unistd.h>

namespace cha {

// Keep file-descriptor interpretation at this boundary so the user workflow remains platform-agnostic.
UserEvents::UserEvents(Status status, bool terminal_input, bool terminal_closed, bool pipe_input)
  : _status(status),
    _terminal_input(terminal_input),
    _terminal_closed(terminal_closed),
    _pipe_input(pipe_input) {
}

bool UserEvents::interrupted() const {
    return _status == Status::interrupted;
}

bool UserEvents::failed() const {
    return _status == Status::failed;
}

bool UserEvents::terminal_input_ready() const {
    return _terminal_input;
}

bool UserEvents::terminal_closed() const {
    return _terminal_closed;
}

bool UserEvents::pipe_input_ready() const {
    return _pipe_input;
}

UserEvents wait_for_user_events(const Pipe& pipe_in) {
    pollfd descriptors[] = {
        {STDIN_FILENO, POLLIN, 0},
        {pipe_in.notification_fd(), POLLIN, 0},
    };

    if (::poll(descriptors, 2, -1) == -1) {
        const UserEvents::Status status =
            errno == EINTR ? UserEvents::Status::interrupted : UserEvents::Status::failed;
        return UserEvents(status);
    }

    return UserEvents(
        UserEvents::Status::ready,
        (descriptors[0].revents & POLLIN) != 0,
        (descriptors[0].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0,
        (descriptors[1].revents & POLLIN) != 0);
}

} // namespace cha
