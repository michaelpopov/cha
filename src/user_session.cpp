#include "user_session.h"

#include "chat_coordinator.h"
#include "command.h"

#include <sstream>
#include <utility>

namespace cha {

// Coalesce session mutations behind this class so rendering happens consistently and only when needed.
UserSession::UserSession(
    SessionView& view,
    ChatCoordinator& coordinator)
  : view_(view),
    coordinator_(coordinator) {
}

bool UserSession::running() const {
    return running_;
}

void UserSession::render() {
    view_.render(coordinator_.conversation(), editor_, coordinator_.generating(), notice_);
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

void UserSession::receive_responses(AgentEventChannel& events) {
    const CoordinatorUpdate update = coordinator_.receive(events);
    if (update.render_needed) {
        request_render();
    }
    if (update.notice) {
        notice_ = *update.notice;
    }
    if (update.channel_closed) {
        running_ = false;
    }
}

void UserSession::receive_terminal_input(CompletionRequestChannel& requests) {
    bool received_input = false;
    while (const std::optional<SessionInput> input = view_.read_input()) {
        received_input = true;
        request_render();
        handle_input(requests, *input);
        if (!running_) {
            break;
        }
    }

    if (!received_input && view_.input_closed()) {
        running_ = false;
    }
}

void UserSession::request_render() {
    render_needed_ = true;
}

void UserSession::handle_input(CompletionRequestChannel& requests, const SessionInput& input) {
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
        if (coordinator_.generating()) {
            request_stop();
        } else if (input.kind == SessionInputKind::interrupt) {
            running_ = false;
        } else {
            editor_.clear();
            notice_.clear();
        }
        break;
    case SessionInputKind::enter:
        submit_input(requests);
        break;
    case SessionInputKind::character:
        editor_.insert(input.character);
        notice_.clear();
        break;
    case SessionInputKind::ignored:
        break;
    }
}

void UserSession::submit_input(CompletionRequestChannel& requests) {
    if (editor_.ends_with_continuation()) {
        editor_.continue_line();
        return;
    }

    const std::string input = editor_.value();
    if (input.empty()) {
        return;
    }
    const Command command = parse_command(input);

    if (coordinator_.generating()) {
        if (command.kind == CommandKind::stop && command.argument.empty()) {
            editor_.clear();
            request_stop();
        } else {
            notice_ = "Generation in progress; use /stop, Esc, or Ctrl-C";
        }
        return;
    }

    editor_.clear();

    if (command.kind != CommandKind::text) {
        if (!command.argument.empty() && command.kind != CommandKind::unknown) {
            notice_ = "Command does not accept arguments";
            return;
        }

        switch (command.kind) {
        case CommandKind::clear:
            coordinator_.clear();
            notice_ = "Conversation cleared";
            return;
        case CommandKind::info: {
            const std::size_t message_count = coordinator_.conversation().snapshot().entries.size();
            const AgentInfo& agent_info = coordinator_.agent_info();
            std::ostringstream info;
            info << "Model: " << agent_info.model << '\n'
                 << "API: " << agent_info.api << '\n'
                 << "Streaming: " << (agent_info.streaming ? "yes" : "no") << '\n'
                 << "Transcript entries: " << message_count;
            coordinator_.add_system_message(info.str());
            notice_.clear();
            return;
        }
        case CommandKind::stop:
            notice_ = "No generation is active";
            return;
        case CommandKind::exit:
            running_ = false;
            return;
        case CommandKind::unknown:
            notice_ = "Unknown command. Commands: /clear, /info, /stop, /exit";
            return;
        case CommandKind::text:
            break;
        }
    }

    notice_ = coordinator_.submit(input, requests);
}

void UserSession::request_stop() {
    coordinator_.request_stop();
    notice_ = "Stopping generation...";
}

void UserSession::shutdown(CompletionRequestChannel& requests) {
    coordinator_.shutdown(requests);
}

} // namespace cha
