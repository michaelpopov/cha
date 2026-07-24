#pragma once

#include "interfaces/terminal/input_editor.h"
#include "interfaces/terminal/session_view.h"

#include <string>

namespace cha {

class ChatCoordinator;
struct CoordinatorUpdate;

// Coordinates testable session state, typed input, rendering, and agent notifications for one run.
class UserSession {
public:
    UserSession(
        SessionView& view,
        ChatCoordinator& coordinator);

    bool running() const;
    void render();
    void render_if_needed();
    void resize();
    void close_terminal();
    void report_terminal_failure();
    void receive_responses();
    void receive_terminal_input();
    void shutdown();

private:
    void request_render();
    void apply_update(const CoordinatorUpdate& update);
    void handle_input(const SessionInput& input);
    void submit_input();
    void request_stop();

    SessionView& view_;
    InputEditor editor_;
    ChatCoordinator& coordinator_;
    bool running_{true};
    bool render_needed_{false};
    std::string notice_;
};

} // namespace cha
