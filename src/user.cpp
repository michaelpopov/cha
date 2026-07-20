#include "user.h"

#include "user_events.h"
#include "user_session.h"

namespace cha {

// Coordinate semantic events here while leaving polling details and mutable UI state to their modules.
void run_user(
    std::atomic_bool& cancellation,
    Conversation& conversation,
    Pipe& pipe_in,
    Pipe& pipe_out) {

    UserSession session(cancellation, conversation);
    session.render();

    while (session.running()) {
        const UserEvents events = wait_for_user_events(pipe_in);
        if (events.interrupted()) {
            session.resize();
            session.render_if_needed();
            continue;
        }
        if (events.failed()) {
            session.report_terminal_failure();
            break;
        }

        if (events.terminal_closed()) {
            session.close_terminal();
        }

        if (events.pipe_input_ready()) {
            session.receive_responses(pipe_in);
        }

        if (session.running() && events.terminal_input_ready()) {
            session.receive_keys(pipe_out);
        }

        session.render_if_needed();
    }

    session.shutdown(pipe_out);
}

} // namespace cha
