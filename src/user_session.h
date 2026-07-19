#pragma once

#include "input_editor.h"
#include "transcript.h"
#include "tui.h"

#include <atomic>
#include <cwchar>
#include <string>

namespace cha {

class Pipe;

// Own all mutable state for one interactive run so callers use cohesive session operations.
class UserSession {
public:
    UserSession(const std::string& model, std::atomic_bool& cancellation);

    [[nodiscard]] bool running() const;
    void render();
    void render_if_needed();
    void resize();
    void close_terminal();
    void report_terminal_failure();
    void receive_responses(Pipe& pipe_in);
    void receive_keys(Pipe& pipe_out);
    void shutdown(Pipe& pipe_out);

private:
    void request_render();
    void handle_key(Pipe& pipe_out, std::wint_t key, int key_result);
    void submit_input(Pipe& pipe_out);
    void finish_generation();
    void request_stop();

    Tui _tui;
    Transcript _transcript;
    InputEditor _editor;
    std::atomic_bool& _cancellation;
    bool _generating{false};
    bool _running{true};
    bool _awaiting_model_confirmation{false};
    bool _render_needed{false};
    std::string _command_reply;
    std::string _notice;
};

} // namespace cha
