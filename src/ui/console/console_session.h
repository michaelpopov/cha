#pragma once

#include "session/session_controller.h"
#include "ui/console/console_port.h"
#include "ui/console/transcript_emitter.h"

#include <cstddef>
#include <deque>
#include <string>

namespace cha {

// Large enough for an ordinary paste or small prompt file, while still
// applying pipe backpressure before queued work grows without bound.
inline constexpr std::size_t default_console_queue_limit = 64;

struct ConsoleSessionOptions {
    bool show_prompt{};
    bool backpressure_stdin{};
    std::size_t queue_limit{default_console_queue_limit};
};

class ConsoleSession {
public:
    ConsoleSession(
        ConsolePort& port,
        SessionController& controller,
        TranscriptEmitter& emitter,
        ConsoleSessionOptions options = {});

    [[nodiscard]] int run();

private:
    void apply(SessionUpdate update);
    void enqueue(std::vector<std::string> lines);
    void pump();
    bool emit();
    int finish(int exit_code);

    ConsolePort& port_;
    SessionController& controller_;
    TranscriptEmitter& emitter_;
    ConsoleSessionOptions options_;
    std::deque<std::string> queue_;
    bool input_done_{};
    bool end_session_{};
    bool prompt_needed_{true};
};

} // namespace cha
