#include "user.h"

#include "command.h"
#include "input_editor.h"
#include "pipe.h"
#include "transcript.h"
#include "tui.h"

#include <cerrno>
#include <cwctype>
#include <optional>
#include <poll.h>
#include <string>
#include <unistd.h>

namespace cha {

User::User(const Config& config, std::atomic_bool& cancellation) : _config(config), _cancellation(cancellation) {
}

void User::run(Pipe& pipe_in, Pipe& pipe_out) {
    Tui tui(_config.model);
    Transcript transcript;
    InputEditor editor;
    bool generating = false;
    bool running = true;
    bool awaiting_model_confirmation = false;
    std::string command_reply;
    std::string notice;

    tui.render(transcript, editor, generating);

    while (running) {
        pollfd descriptors[] = {
            {STDIN_FILENO, POLLIN, 0},
            {pipe_in.notification_fd(), POLLIN, 0},
        };

        const int poll_result = ::poll(descriptors, 2, -1);
        if (poll_result == -1) {
            if (errno == EINTR) {
                tui.resize();
                tui.render(transcript, editor, generating, notice);
                continue;
            }
            transcript.add_system("Terminal input failed.");
            break;
        }

        bool render = false;

        if ((descriptors[0].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
            running = false;
        }

        if ((descriptors[1].revents & POLLIN) != 0) {
            PipeEvent response{PipeEventKind::eom, {}};
            while (pipe_in.try_get(response)) {
                render = true;
                if (response.kind == PipeEventKind::data) {
                    transcript.append_assistant(response.data);
                    if (awaiting_model_confirmation) {
                        command_reply += response.data;
                    }
                    continue;
                }

                if (response.kind == PipeEventKind::closed) {
                    running = false;
                    break;
                }

                transcript.finish_assistant();
                if (awaiting_model_confirmation) {
                    if (const auto model = confirmed_model(command_reply)) {
                        tui.set_model(*model);
                    }
                    awaiting_model_confirmation = false;
                    command_reply.clear();
                }
                generating = false;
                notice = _cancellation.load(std::memory_order_acquire) ? "Generation stopped" : "";
                _cancellation.store(false, std::memory_order_release);
            }
        }

        if (running && (descriptors[0].revents & POLLIN) != 0) {
            wint_t key = 0;
            int key_result = ERR;
            bool received_key = false;
            while ((key_result = tui.read_key(key)) != ERR) {
                received_key = true;
                render = true;

                if (key == KEY_RESIZE) {
                    tui.resize();
                } else if (key == KEY_PPAGE) {
                    tui.scroll_up();
                } else if (key == KEY_NPAGE) {
                    tui.scroll_down();
                } else if (key == KEY_LEFT) {
                    editor.move_left();
                } else if (key == KEY_RIGHT) {
                    editor.move_right();
                } else if (key == KEY_UP) {
                    editor.move_up();
                } else if (key == KEY_DOWN) {
                    editor.move_down();
                } else if (key == KEY_HOME) {
                    editor.move_home();
                } else if (key == KEY_END) {
                    editor.move_end();
                } else if (key == KEY_DC) {
                    editor.erase();
                } else if (key == KEY_BACKSPACE || key == 127 || key == L'\b') {
                    editor.backspace();
                } else if (key == 27 || key == 3) {
                    if (generating) {
                        _cancellation.store(true, std::memory_order_release);
                        notice = "Stopping generation...";
                    } else if (key == 3) {
                        running = false;
                        break;
                    } else {
                        editor.clear();
                        notice.clear();
                    }
                } else if (key == L'\n' || key == L'\r' || key == KEY_ENTER) {
                    if (editor.ends_with_continuation()) {
                        editor.continue_line();
                        continue;
                    }

                    const std::string input = editor.value();
                    if (input.empty()) {
                        continue;
                    }
                    const Command command = parse_command(input);

                    if (generating) {
                        if (command.kind == CommandKind::stop && command.argument.empty()) {
                            editor.clear();
                            _cancellation.store(true, std::memory_order_release);
                            notice = "Stopping generation...";
                        } else {
                            notice = "Generation in progress; use .stop, Esc, or Ctrl-C";
                        }
                        continue;
                    }

                    editor.clear();

                    if (command.kind == CommandKind::exit && command.argument.empty()) {
                        running = false;
                        break;
                    }
                    if (command.kind == CommandKind::stop && command.argument.empty()) {
                        notice = "No generation is active";
                        continue;
                    }

                    _cancellation.store(false, std::memory_order_release);
                    awaiting_model_confirmation = command.kind == CommandKind::model && !command.argument.empty();
                    command_reply.clear();
                    transcript.add_user(input);
                    transcript.begin_assistant();
                    pipe_out.put(input);
                    pipe_out.eom();
                    generating = true;
                    notice.clear();
                } else if (key_result == OK && std::iswprint(static_cast<wint_t>(key)) != 0) {
                    editor.insert(static_cast<wchar_t>(key));
                    notice.clear();
                }
            }

            if (!received_key && !::isatty(STDIN_FILENO)) {
                running = false;
            }
        }

        if (render) {
            tui.render(transcript, editor, generating, notice);
        }
    }

    if (generating) {
        _cancellation.store(true, std::memory_order_release);
    }
    pipe_out.close();
}

} // namespace cha
