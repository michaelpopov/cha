#include "ui/tui/user.h"

#include "session/session_controller.h"
#include "ui/tui/terminal.h"
#include "ui/tui/tui.h"
#include "ui/tui/user_session.h"
#include "util/event_fd_notifier.h"
#include "util/input_wait.h"

#include <exception>

namespace cha {

// Coordinate semantic events here while leaving polling details and mutable UI state to their modules.
void run_user(
    Terminal& terminal,
    SessionController& controller,
    EventFdNotifier& notifier) {

    std::exception_ptr failure;
    {
        Tui tui(terminal);
        UserSession session(tui, controller);
        try {
            session.render();

            while (session.running()) {
                const InputEvents ready =
                    wait_for_input_events(notifier.descriptor());
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

                if (ready.input_closed()) {
                    session.close_terminal();
                }

                if (ready.notification_ready()) {
                    notifier.acknowledge();
                    session.receive_responses();
                }

                if (session.running() && ready.input_ready()) {
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
