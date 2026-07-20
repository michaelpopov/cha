#pragma once

#include "agent_protocol.h"
#include "input_editor.h"
#include "session_view.h"

#include <string>

namespace cha {

class ChatCoordinator;

// Coordinates testable session state, typed input, rendering, and agent notifications for one run.
class UserSession {
public:
    UserSession(
        SessionView& view,
        ChatCoordinator& coordinator);

    [[nodiscard]] bool running() const;
    void render();
    void render_if_needed();
    void resize();
    void close_terminal();
    void report_terminal_failure();
    void receive_responses(AgentEventChannel& events);
    void receive_terminal_input(CompletionRequestChannel& requests);
    void shutdown(CompletionRequestChannel& requests);

private:
    void request_render();
    void handle_input(CompletionRequestChannel& requests, const SessionInput& input);
    void submit_input(CompletionRequestChannel& requests);
    void request_stop();

    SessionView& view_;
    InputEditor editor_;
    ChatCoordinator& coordinator_;
    bool running_{true};
    bool render_needed_{false};
    std::string notice_;
};

} // namespace cha
