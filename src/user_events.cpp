#include "user_events.h"

#include "agent_protocol.h"

#include <cerrno>
#include <poll.h>
#include <unistd.h>

namespace cha {

// Keep file-descriptor interpretation at this boundary so the user workflow remains platform-agnostic.
UserEvents::UserEvents(Status status, bool terminal_input, bool terminal_closed, bool agent_event)
  : _status(status),
    _terminal_input(terminal_input),
    _terminal_closed(terminal_closed),
    _agent_event(agent_event) {
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

bool UserEvents::agent_event_ready() const {
    return _agent_event;
}

UserEvents wait_for_user_events(const AgentEventChannel& events) {
    pollfd descriptors[] = {
        {STDIN_FILENO, POLLIN, 0},
        {events.notification_fd(), POLLIN, 0},
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
