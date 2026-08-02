#pragma once

#include "ui/tui/input_editor.h"
#include "ui/tui/session_view.h"

#include <string>

namespace cha {

class SessionController;
struct SessionUpdate;

// The state machine of one interactive run. It turns typed input into editing, scrolling,
// submission, cancellation, or shutdown, sends submitted text on to the controller, and applies
// the SessionUpdate that comes back by redrawing, clearing the editor, or showing a notice. It
// owns the InputEditor and the pending notice, and reaches the screen only through SessionView,
// which is what makes the whole session testable without a terminal.
class PersonaSession {
public:
    PersonaSession(
        SessionView& view,
        SessionController& controller,
        std::string author_id);

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
    void apply_update(const SessionUpdate& update);
    void handle_input(const SessionInput& input);
    void submit_input();
    void request_stop();
    std::string input_target_name() const;

    SessionView& view_;
    InputEditor editor_;
    SessionController& controller_;
    std::string author_id_;
    bool running_{true};
    bool render_needed_{false};
    std::string notice_;
};

} // namespace cha
