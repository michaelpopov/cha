#include "ui/terminal/user_session.h"

#include "session/session_controller.h"
#include "ui/terminal/transcript_renderer.h"
#include "ui/text/text_input.h"

#include <utility>

namespace cha {

// Coalesce session mutations behind this class so rendering happens consistently and only when needed.
UserSession::UserSession(
    SessionView& view,
    SessionController& controller)
  : view_(view),
    controller_(controller) {
}

bool UserSession::running() const {
    return running_;
}

void UserSession::render() {
    view_.render(
        controller_.conversation(),
        editor_,
        controller_.generation_status(),
        show_addressing(controller_.roster(), controller_.conversation()),
        notice_);
    render_needed_ = false;
}

void UserSession::render_if_needed() {
    if (render_needed_) {
        render();
    }
}

void UserSession::resize() {
    view_.resize();
    request_render();
}

void UserSession::close_terminal() {
    running_ = false;
}

void UserSession::report_terminal_failure() {
    notice_ = "Terminal input failed.";
    running_ = false;
    request_render();
}

void UserSession::receive_responses() {
    const SessionUpdate update = controller_.receive();
    apply_update(update);
}

void UserSession::apply_update(const SessionUpdate& update) {
    if (update.clear_input) {
        editor_.clear();
    }
    if (update.render_needed) {
        request_render();
    }
    if (update.notice) {
        notice_ = *update.notice;
    }
    if (update.end_session) {
        running_ = false;
    }
}

void UserSession::receive_terminal_input() {
    while (const std::optional<SessionInput> input = view_.read_input()) {
        request_render();
        handle_input(*input);
        if (!running_) {
            break;
        }
    }
}

void UserSession::request_render() {
    render_needed_ = true;
}

void UserSession::handle_input(const SessionInput& input) {
    switch (input.kind) {
    case SessionInputKind::resize:
        view_.resize();
        break;
    case SessionInputKind::page_up:
        view_.scroll_up();
        break;
    case SessionInputKind::page_down:
        view_.scroll_down();
        break;
    case SessionInputKind::left:
        editor_.move_left();
        break;
    case SessionInputKind::right:
        editor_.move_right();
        break;
    case SessionInputKind::up:
        editor_.move_up();
        break;
    case SessionInputKind::down:
        editor_.move_down();
        break;
    case SessionInputKind::home:
        editor_.move_home();
        break;
    case SessionInputKind::end:
        editor_.move_end();
        break;
    case SessionInputKind::erase:
        editor_.erase();
        break;
    case SessionInputKind::backspace:
        editor_.backspace();
        break;
    case SessionInputKind::escape:
    case SessionInputKind::interrupt:
        if (controller_.generation_status().active) {
            request_stop();
        } else if (input.kind == SessionInputKind::interrupt) {
            running_ = false;
        } else {
            editor_.clear();
            notice_.clear();
        }
        break;
    case SessionInputKind::enter:
        submit_input();
        break;
    case SessionInputKind::character:
        editor_.insert(input.character);
        notice_.clear();
        break;
    case SessionInputKind::ignored:
        break;
    }
}

void UserSession::submit_input() {
    if (editor_.ends_with_continuation()) {
        editor_.continue_line();
        return;
    }

    const std::string input = editor_.value();
    apply_update(handle_text_input(controller_, input));
}

void UserSession::request_stop() {
    apply_update(controller_.request_stop());
}

void UserSession::shutdown() {
    controller_.shutdown();
}

} // namespace cha
