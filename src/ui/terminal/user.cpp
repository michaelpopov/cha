#include "ui/terminal/user.h"

#include "session/session_controller.h"
#include "ui/terminal/terminal.h"
#include "ui/terminal/tui.h"
#include "ui/terminal/user_session.h"

#include <cerrno>
#include <exception>
#include <poll.h>
#include <unistd.h>

namespace cha {

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

// Keep file-descriptor interpretation at this boundary so the user workflow remains platform-agnostic.
UserEvents wait_for_user_events(int agent_notification_fd) {
    pollfd descriptors[] = {
        {STDIN_FILENO, POLLIN, 0},
        {agent_notification_fd, POLLIN, 0},
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

// Coordinate semantic events here while leaving polling details and mutable UI state to their modules.
void run_user(
    Terminal& terminal,
    SessionController& controller) {

    std::exception_ptr failure;
    {
        Tui tui(terminal);
        UserSession session(tui, controller);
        try {
            session.render();

            while (session.running()) {
                const UserEvents ready =
                    wait_for_user_events(controller.notification_fd());
                if (ready.interrupted()) {
                    session.resize();
                    session.render_if_needed();
                    continue;
                }
                if (ready.failed()) {
                    session.report_terminal_failure();
                    session.render_if_needed();
                    break;
                }

                if (ready.terminal_closed()) {
                    session.close_terminal();
                }

                if (ready.agent_event_ready()) {
                    session.receive_responses();
                }

                if (session.running() && ready.terminal_input_ready()) {
                    session.receive_terminal_input();
                }

                session.render_if_needed();
            }

            session.shutdown();
        } catch (...) {
            failure = std::current_exception();
        }
    }

    if (failure) {
        terminal.restore();
        try {
            controller.shutdown();
        } catch (...) {
            // Preserve the operation failure that ended the session.
        }
        std::rethrow_exception(failure);
    }
}

} // namespace cha
