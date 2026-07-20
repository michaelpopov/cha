#include "user_session.h"

#include "chat_coordinator.h"
#include "command.h"

#include <cwctype>
#include <sstream>
#include <utility>
#include <unistd.h>

namespace cha {

// Coalesce session mutations behind this class so rendering happens consistently and only when needed.
UserSession::UserSession(
    Terminal& terminal,
    ChatCoordinator& coordinator)
  : _tui(terminal),
    _coordinator(coordinator) {
}

bool UserSession::running() const {
    return _running;
}

void UserSession::render() {
    _tui.render(_coordinator.conversation(), _editor, _coordinator.generating(), _notice);
    _render_needed = false;
}

void UserSession::render_if_needed() {
    if (_render_needed) {
        render();
    }
}

void UserSession::resize() {
    _tui.resize();
    request_render();
}

void UserSession::close_terminal() {
    _running = false;
}

void UserSession::report_terminal_failure() {
    _notice = "Terminal input failed.";
    request_render();
}

void UserSession::receive_responses(AgentEventChannel& events) {
    const CoordinatorUpdate update = _coordinator.receive(events);
    if (update.render_needed) {
        request_render();
    }
    if (update.notice) {
        _notice = *update.notice;
    }
    if (update.channel_closed) {
        _running = false;
    }
}

void UserSession::receive_keys(CompletionRequestChannel& requests) {
    wint_t key = 0;
    int key_result = ERR;
    bool received_key = false;
    while ((key_result = _tui.read_key(key)) != ERR) {
        received_key = true;
        request_render();
        handle_key(requests, key, key_result);
        if (!_running) {
            break;
        }
    }

    if (!received_key && !::isatty(STDIN_FILENO)) {
        _running = false;
    }
}

void UserSession::request_render() {
    _render_needed = true;
}

void UserSession::handle_key(CompletionRequestChannel& requests, std::wint_t key, int key_result) {
    if (key == KEY_RESIZE) {
        _tui.resize();
    } else if (key == KEY_PPAGE) {
        _tui.scroll_up();
    } else if (key == KEY_NPAGE) {
        _tui.scroll_down();
    } else if (key == KEY_LEFT) {
        _editor.move_left();
    } else if (key == KEY_RIGHT) {
        _editor.move_right();
    } else if (key == KEY_UP) {
        _editor.move_up();
    } else if (key == KEY_DOWN) {
        _editor.move_down();
    } else if (key == KEY_HOME) {
        _editor.move_home();
    } else if (key == KEY_END) {
        _editor.move_end();
    } else if (key == KEY_DC) {
        _editor.erase();
    } else if (key == KEY_BACKSPACE || key == 127 || key == L'\b') {
        _editor.backspace();
    } else if (key == 27 || key == 3) {
        if (_coordinator.generating()) {
            request_stop();
        } else if (key == 3) {
            _running = false;
        } else {
            _editor.clear();
            _notice.clear();
        }
    } else if (key == L'\n' || key == L'\r' || key == KEY_ENTER) {
        submit_input(requests);
    } else if (key_result == OK && std::iswprint(key) != 0) {
        _editor.insert(static_cast<wchar_t>(key));
        _notice.clear();
    }
}

void UserSession::submit_input(CompletionRequestChannel& requests) {
    if (_editor.ends_with_continuation()) {
        _editor.continue_line();
        return;
    }

    const std::string input = _editor.value();
    if (input.empty()) {
        return;
    }
    const Command command = parse_command(input);

    if (_coordinator.generating()) {
        if (command.kind == CommandKind::stop && command.argument.empty()) {
            _editor.clear();
            request_stop();
        } else {
            _notice = "Generation in progress; use /stop, Esc, or Ctrl-C";
        }
        return;
    }

    _editor.clear();

    if (command.kind != CommandKind::text) {
        if (!command.argument.empty() && command.kind != CommandKind::unknown) {
            _notice = "Command does not accept arguments";
            return;
        }

        switch (command.kind) {
        case CommandKind::clear:
            _coordinator.clear();
            _notice = "Conversation cleared";
            return;
        case CommandKind::info: {
            const std::size_t message_count = _coordinator.conversation().snapshot().messages.size();
            const AgentInfo& agent_info = _coordinator.agent_info();
            std::ostringstream info;
            info << "Model: " << agent_info.model << '\n'
                 << "API: " << agent_info.api << '\n'
                 << "Streaming: " << (agent_info.streaming ? "yes" : "no") << '\n'
                 << "Transcript messages: " << message_count;
            _coordinator.add_system_message(info.str());
            _notice.clear();
            return;
        }
        case CommandKind::stop:
            _notice = "No generation is active";
            return;
        case CommandKind::exit:
            _running = false;
            return;
        case CommandKind::unknown:
            _notice = "Unknown command. Commands: /clear, /info, /stop, /exit";
            return;
        case CommandKind::text:
            break;
        }
    }

    _notice = _coordinator.submit(input, requests);
}

void UserSession::request_stop() {
    _coordinator.request_stop();
    _notice = "Stopping generation...";
}

void UserSession::shutdown(CompletionRequestChannel& requests) {
    _coordinator.shutdown(requests);
}

} // namespace cha
