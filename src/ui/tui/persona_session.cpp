#include "ui/tui/persona_session.h"

#include "application/chat_application.h"
#include "session/session_controller.h"
#include "ui/render/transcript_writer.h"
#include "ui/text/text_input.h"
#include "ui/text/application_dispatcher.h"
#include "ui/text/mention.h"

#include <algorithm>
#include <utility>

namespace cha {

// Coalesce session mutations behind this class so rendering happens consistently and only when needed.
PersonaSession::PersonaSession(
    SessionView& view,
    SessionController& controller,
    std::string author_id)
  : view_(view),
    controller_(&controller),
    author_id_(std::move(author_id)) {
}

PersonaSession::PersonaSession(SessionView& view, ChatApplication& application)
  : view_(view), controller_(&application.controller()), application_(&application) {}

bool PersonaSession::running() const {
    return running_;
}

void PersonaSession::render() {
    SessionController& controller = application_ ? application_->controller() : *controller_;
    const TranscriptView transcript = controller.transcript().view();
    view_.render(
        transcript,
        editor_,
        controller.generation_status(),
        show_addressing(controller.characters(), transcript),
        input_target_name(),
        notice_,
        overlay_ ? &*overlay_ : nullptr);
    render_needed_ = false;
}

void PersonaSession::render_if_needed() {
    if (render_needed_) {
        render();
    }
}

void PersonaSession::resize() {
    view_.resize();
    request_render();
}

void PersonaSession::close_terminal() {
    running_ = false;
}

void PersonaSession::report_terminal_failure() {
    notice_ = "Terminal input failed.";
    running_ = false;
    request_render();
}

void PersonaSession::receive_responses() {
    SessionController& controller = application_ ? application_->controller() : *controller_;
    const SessionChange change = controller.receive();
    apply_change(change);
}

void PersonaSession::apply_application_result(ApplicationResult result) {
    if (result.input_consumed) editor_.clear();
    if (result.session_changed) {
        controller_ = &application_->controller();
        overlay_.reset();
        view_.reset_session_view();
    }
    if (result.list) {
        overlay_ = ApplicationOverlay{.title = std::move(result.list->title),
                                      .rows = std::move(result.list->rows)};
    }
    if (result.notice && notice_ != *result.notice) {
        notice_ = std::move(*result.notice);
    }
    apply_change(result.session);
    if (result.exit_requested) running_ = false;
    request_render();
}

void PersonaSession::apply_change(const SessionChange& change) {
    if (change.state_changed) {
        request_render();
    }
    // A notice can change without any session state changing: /info and the
    // handle commands report through this field alone. Redraw on it here
    // rather than relying on the caller having already asked for one.
    if (change.notice && notice_ != *change.notice) {
        notice_ = *change.notice;
        request_render();
    }
    if (change.controller_ended) {
        running_ = false;
    }
}

void PersonaSession::apply_text_input(const TextInputResult& result) {
    if (result.clear_input) editor_.clear();
    apply_change(result.session);
    if (result.exit_requested) running_ = false;
}

void PersonaSession::receive_terminal_input() {
    while (const std::optional<SessionInput> input = view_.read_input()) {
        request_render();
        handle_input(*input);
        if (!running_) {
            break;
        }
    }
}

void PersonaSession::request_render() {
    render_needed_ = true;
}

void PersonaSession::handle_input(const SessionInput& input) {
    const SessionController& current = application_ ? application_->controller() : *controller_;
    if (overlay_ && !(input.kind == SessionInputKind::escape && current.is_generating())) {
        switch (input.kind) {
        case SessionInputKind::escape:
            overlay_.reset();
            request_render();
            return;
        case SessionInputKind::page_up:
            overlay_->first_visible = overlay_->first_visible > 5
                ? overlay_->first_visible - 5 : 0;
            request_render();
            return;
        case SessionInputKind::up:
            if (overlay_->first_visible > 0) --overlay_->first_visible;
            request_render();
            return;
        case SessionInputKind::page_down:
            if (!overlay_->rows.empty()) {
                overlay_->first_visible = std::min(
                    overlay_->rows.size() - 1, overlay_->first_visible + 5);
            }
            request_render();
            return;
        case SessionInputKind::down:
            if (!overlay_->rows.empty()
                && overlay_->first_visible < overlay_->rows.size() - 1) {
                ++overlay_->first_visible;
            }
            request_render();
            return;
        case SessionInputKind::resize:
            view_.resize();
            request_render();
            return;
        case SessionInputKind::interrupt:
            break;
        default:
            // An overlay is modal for editor input.  It must not let hidden
            // keystrokes alter the draft while it is being scrolled.
            return;
        }
    }
    SessionController& controller = application_ ? application_->controller() : *controller_;
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
        if (controller.is_generating()) {
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

void PersonaSession::submit_input() {
    if (editor_.ends_with_continuation()) {
        editor_.continue_line();
        return;
    }

    const std::string input = editor_.value();
    if (application_) {
        ApplicationDispatcher dispatcher(*application_);
        apply_application_result(dispatcher.handle(input));
    } else {
        apply_text_input(handle_text_input(*controller_, author_id_, input));
    }
}

void PersonaSession::request_stop() {
    SessionController& controller = application_ ? application_->controller() : *controller_;
    apply_change(controller.request_stop());
}

std::string PersonaSession::input_target_name() const {
    const SessionController& controller = application_ ? application_->controller() : *controller_;
    const CharacterInfo* target =
        controller.characters().find(controller.default_agent_id());
    const AddressedPrompt prompt = parse_addressed_prompt(editor_.value());
    if (!prompt.handle.empty()) {
        const HandleResolution resolution =
            controller.characters().resolve_handle(prompt.handle);
        if (resolution.match == HandleMatch::resolved) {
            target = resolution.character;
        }
    }
    return target ? target->name : std::string{};
}

void PersonaSession::shutdown() {
    if (application_) application_->shutdown();
    else controller_->shutdown();
}

} // namespace cha
