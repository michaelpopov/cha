#include "interfaces/terminal/user.h"

#include "application/chat_coordinator.h"
#include "interfaces/terminal/terminal.h"
#include "interfaces/terminal/tui.h"
#include "interfaces/terminal/user_events.h"
#include "interfaces/terminal/user_session.h"

#include <exception>

namespace cha {

// Coordinate semantic events here while leaving polling details and mutable UI state to their modules.
void run_user(
    Terminal& terminal,
    ChatCoordinator& coordinator) {

    std::exception_ptr failure;
    {
        Tui tui(terminal);
        UserSession session(tui, coordinator);
        try {
            session.render();

            while (session.running()) {
                const UserEvents ready =
                    wait_for_user_events(coordinator.notification_fd());
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
            coordinator.shutdown();
        } catch (...) {
            // Preserve the operation failure that ended the session.
        }
        std::rethrow_exception(failure);
    }
}

} // namespace cha
