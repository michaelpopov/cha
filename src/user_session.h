#pragma once

#include "agent_protocol.h"
#include "input_editor.h"
#include "tui.h"

#include <cwchar>
#include <string>

namespace cha {

class ChatCoordinator;
class Terminal;

// Coordinates interactive chat state, key handling, rendering, and agent notifications for one run.
class UserSession {
public:
    UserSession(
        Terminal& terminal,
        ChatCoordinator& coordinator);

    [[nodiscard]] bool running() const;
    void render();
    void render_if_needed();
    void resize();
    void close_terminal();
    void report_terminal_failure();
    void receive_responses(AgentEventChannel& events);
    void receive_keys(CompletionRequestChannel& requests);
    void shutdown(CompletionRequestChannel& requests);

private:
    void request_render();
    void handle_key(CompletionRequestChannel& requests, std::wint_t key, int key_result);
    void submit_input(CompletionRequestChannel& requests);
    void request_stop();

    Tui _tui;
    InputEditor _editor;
    ChatCoordinator& _coordinator;
    bool _running{true};
    bool _render_needed{false};
    std::string _notice;
};

} // namespace cha
