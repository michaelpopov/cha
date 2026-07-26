#include "ui/console/console_session.h"

#include "ui/console/console_writer.h"
#include "ui/text/command.h"
#include "ui/text/text_input.h"

#include <ostream>
#include <stdexcept>
#include <utility>

namespace cha {

ConsoleSession::ConsoleSession(
    ConsolePort& port,
    SessionController& controller,
    TranscriptEmitter& emitter,
    ConsoleSessionOptions options)
    : port_(port),
      controller_(controller),
      emitter_(emitter),
      options_(options) {
}

int ConsoleSession::run() {
    if (!emit()) {
        return finish(1);
    }

    while (true) {
        if (end_session_
            || (input_done_ && queue_.empty()
                && !controller_.generation_status().active)) {
            return finish(0);
        }

        // Only arm the prompt while the session can accept a new submission.
        // Printing it as soon as stdin wakes leaves a stray ">" in front of the
        // next agent line, and never restores one after the turn goes idle.
        if (options_.show_prompt && prompt_needed_
            && !controller_.generation_status().active
            && queue_.empty()
            && !input_done_
            && !end_session_) {
            const PersonaInfo* default_agent =
                controller_.personas().find(
                    controller_.default_agent_id());
            if (!default_agent) {
                throw std::logic_error(
                    "Default agent is not a room persona");
            }
            write_console_prompt(port_.prompt(), default_agent->name);
            port_.notices() << std::flush;
            prompt_needed_ = false;
        }

        const bool queue_full =
            options_.queue_limit > 0
            && queue_.size() >= options_.queue_limit;
        // Drop stdin from the poll set once it is exhausted. POLLHUP is level
        // triggered and survives the zero-length read, so polling a closed
        // standard input returns immediately forever and spins the loop for the
        // rest of the turn.
        const bool include_input =
            !input_done_
            && !(options_.backpressure_stdin && queue_full);
        const InputEvents ready =
            port_.wait(controller_.notification_fd(), include_input);

        if (ready.failed()) {
            port_.notices() << "Console input wait failed.\n";
            return finish(1);
        }

        if (ready.signal_ready() && port_.take_interrupt()) {
            if (controller_.generation_status().active) {
                apply(controller_.request_stop());
            } else {
                return finish(0);
            }
        }

        if (ready.notification_ready()) {
            const bool was_generating =
                controller_.generation_status().active;
            apply(controller_.receive());
            // A finished turn must offer a fresh prompt even when no further
            // stdin activity re-armed it (for example after Ctrl-C cancel).
            if (was_generating && !controller_.generation_status().active) {
                prompt_needed_ = true;
            }
        }

        if (ready.input_ready() || ready.input_closed()) {
            enqueue(port_.take_lines());
            prompt_needed_ = true;
        }
        if (ready.input_closed() || port_.input_closed()) {
            input_done_ = true;
        }

        pump();
        if (!emit()) {
            return finish(1);
        }
    }
}

void ConsoleSession::apply(SessionUpdate update) {
    if (update.notice && !update.notice->empty()) {
        port_.notices() << *update.notice << '\n';
    }
    end_session_ = end_session_ || update.end_session;
}

void ConsoleSession::enqueue(std::vector<std::string> lines) {
    for (std::string& line : lines) {
        const Command command = parse_command(line);
        if (command.kind == CommandKind::stop && command.argument.empty()) {
            apply(controller_.request_stop());
            continue;
        }
        if (command.kind == CommandKind::exit
            && command.argument.empty()) {
            end_session_ = true;
            queue_.clear();
            return;
        }
        queue_.push_back(std::move(line));
        // Pump between lines from the same read. This makes
        // "prompt\n/stop\n" observe the prompt as active while still queuing
        // ordinary prompt batches behind the single in-flight turn.
        pump();
    }
}

void ConsoleSession::pump() {
    while (!end_session_ && !queue_.empty()
        && !controller_.generation_status().active) {
        std::string line = std::move(queue_.front());
        queue_.pop_front();
        apply(handle_text_input(controller_, std::move(line)));
    }
}

bool ConsoleSession::emit() {
    emitter_.write(controller_.transcript().snapshot());
    if (!port_.flush()) {
        port_.notices() << "Failed to write console transcript.\n";
        return false;
    }
    emitter_.commit();
    return true;
}

int ConsoleSession::finish(int exit_code) {
    if (!port_.finish_transcript() && exit_code == 0) {
        port_.notices() << "Failed to write console transcript.\n";
        exit_code = 1;
    }
    try {
        controller_.shutdown();
    } catch (...) {
        if (exit_code == 0) {
            throw;
        }
    }
    return exit_code;
}

} // namespace cha
