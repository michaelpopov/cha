#include "user_session.h"

#include "command.h"
#include "pipe.h"

#include <cwctype>
#include <unistd.h>

namespace cha {

// Coalesce session mutations behind this class so rendering happens consistently and only when needed.
UserSession::UserSession(const std::string& model, std::atomic_bool& cancellation, Conversation& conversation)
  : _tui(model), _cancellation(cancellation), _conversation(conversation) {
}

bool UserSession::running() const {
    return _running;
}

void UserSession::render() {
    _tui.render(_conversation, _editor, _generating, _notice);
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

void UserSession::receive_responses(Pipe& pipe_in) {
    PipeEvent response{PipeEventKind::eom, {}};
    while (pipe_in.try_get(response)) {
        if (response.kind == PipeEventKind::conversation_updated) {
            request_render();
        } else if (response.kind == PipeEventKind::model_changed) {
            _tui.set_model(response.data);
            request_render();
        } else if (response.kind == PipeEventKind::closed) {
            _running = false;
            break;
        } else if (response.kind == PipeEventKind::eom) {
            finish_generation();
            request_render();
        }
    }
}

void UserSession::receive_keys(Pipe& pipe_out) {
    wint_t key = 0;
    int key_result = ERR;
    bool received_key = false;
    while ((key_result = _tui.read_key(key)) != ERR) {
        received_key = true;
        request_render();
        handle_key(pipe_out, key, key_result);
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

void UserSession::handle_key(Pipe& pipe_out, std::wint_t key, int key_result) {
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
        if (_generating) {
            request_stop();
        } else if (key == 3) {
            _running = false;
        } else {
            _editor.clear();
            _notice.clear();
        }
    } else if (key == L'\n' || key == L'\r' || key == KEY_ENTER) {
        submit_input(pipe_out);
    } else if (key_result == OK && std::iswprint(key) != 0) {
        _editor.insert(static_cast<wchar_t>(key));
        _notice.clear();
    }
}

void UserSession::submit_input(Pipe& pipe_out) {
    if (_editor.ends_with_continuation()) {
        _editor.continue_line();
        return;
    }

    const std::string input = _editor.value();
    if (input.empty()) {
        return;
    }
    const Command command = parse_command(input);

    if (_generating) {
        if (command.kind == CommandKind::stop && command.argument.empty()) {
            _editor.clear();
            request_stop();
        } else {
            _notice = "Generation in progress; use .stop, Esc, or Ctrl-C";
        }
        return;
    }

    _editor.clear();

    if (command.kind == CommandKind::exit && command.argument.empty()) {
        _running = false;
        return;
    }
    if (command.kind == CommandKind::stop && command.argument.empty()) {
        _notice = "No generation is active";
        return;
    }

    _cancellation.store(false, std::memory_order_release);
    _conversation.add_message(std::string(user_author), input);
    pipe_out.put(input);
    pipe_out.eom();
    _generating = true;
    _notice.clear();
}

void UserSession::finish_generation() {
    _generating = false;
    _notice = _cancellation.load(std::memory_order_acquire) ? "Generation stopped" : "";
    _cancellation.store(false, std::memory_order_release);
}

void UserSession::request_stop() {
    _cancellation.store(true, std::memory_order_release);
    _notice = "Stopping generation...";
}

void UserSession::shutdown(Pipe& pipe_out) {
    if (_generating) {
        _cancellation.store(true, std::memory_order_release);
    }
    pipe_out.close();
}

} // namespace cha
