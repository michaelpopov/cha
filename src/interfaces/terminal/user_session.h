#pragma once

#include "interfaces/terminal/input_editor.h"
#include "interfaces/terminal/session_view.h"

#include <string>

namespace cha {

class ChatCoordinator;
struct CoordinatorUpdate;

// The state machine of one interactive run. It turns typed input into editing, scrolling,
// submission, cancellation, or shutdown, sends submitted text on to the coordinator, and applies
// the CoordinatorUpdate that comes back by redrawing, clearing the editor, or showing a notice. It
// owns the InputEditor and the pending notice, and reaches the screen only through SessionView,
// which is what makes the whole session testable without a terminal.
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
