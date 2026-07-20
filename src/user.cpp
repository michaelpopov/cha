#include "user.h"

#include "agent_protocol.h"
#include "chat_coordinator.h"
#include "tui.h"
#include "user_events.h"
#include "user_session.h"

namespace cha {

// Coordinate semantic events here while leaving polling details and mutable UI state to their modules.
void run_user(
    Terminal& terminal,
    ChatCoordinator& coordinator,
    AgentEventChannel& agent_events,
    CompletionRequestChannel& requests) {

    Tui tui(terminal);
    UserSession session(tui, coordinator);
    session.render();

    while (session.running()) {
        const UserEvents ready = wait_for_user_events(agent_events);
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
            session.receive_responses(agent_events);
        }

        if (session.running() && ready.terminal_input_ready()) {
            session.receive_terminal_input(requests);
        }

        session.render_if_needed();
    }

    session.shutdown(requests);
}

} // namespace cha
