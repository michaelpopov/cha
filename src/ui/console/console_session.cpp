#include "ui/console/console_session.h"

#include "ui/console/console_writer.h"
#include "ui/text/application_command.h"
#include "ui/text/application_dispatcher.h"
#include "ui/text/command.h"

#include <ostream>
#include <stdexcept>
#include <utility>

namespace cha {

ConsoleSession::ConsoleSession(
    ConsolePort& port,
    ChatApplication& application,
    ConsoleSessionOptions options)
    : port_(port),
      application_(application),
      emitter_(std::make_unique<TranscriptEmitter>(
          port.transcript(),
          show_addressing(
              application.controller().characters(),
              application.controller().transcript().view()),
          !options.show_prompt)),
      options_(options) {
}

int ConsoleSession::run() {
    if (!emit()) {
        return finish(1);
    }

    while (true) {
        if (end_session_
            || (input_done_ && queue_.empty()
                && !application_.controller().is_generating())) {
            return finish(output_failed_ ? 1 : 0);
        }

        // Only arm the prompt while the session can accept a new submission.
        // Printing it as soon as stdin wakes leaves a stray ">" in front of the
        // next agent line, and never restores one after the turn goes idle.
        if (options_.show_prompt && prompt_needed_
            && !application_.controller().is_generating()
            && queue_.empty()
            && !input_done_
            && !end_session_) {
            const CharacterInfo* default_agent =
                application_.controller().characters().find(
                    application_.controller().default_agent_id());
            if (!default_agent) {
                throw std::logic_error(
                    "Default agent is not a forum character");
            }
            write_console_prompt(port_.prompt(), default_agent->name);
            port_.notices() << std::flush;
            prompt_needed_ = false;
        }

        const bool queue_full =
            options_.queue_limit > 0
            && queue_.size() >= options_.queue_limit;
        // Stop the frontend's stdin reader once it is exhausted. A sticky EOF
        // source would otherwise wake the loop forever and spin the rest of
        // the turn.
        const bool include_input =
            !input_done_
            && !(options_.backpressure_stdin && queue_full);
        const InputEvents ready = port_.wait(include_input);

        if (ready.failed()) {
            port_.notices() << "Console input wait failed.\n";
            return finish(1);
        }

        if (ready.signal_ready() && port_.take_interrupt()) {
            if (application_.controller().is_generating()) {
                apply({.session = application_.controller().request_stop()});
            } else {
                return finish(0);
            }
        }

        if (ready.notification_ready()) {
            const bool was_generating =
                application_.controller().is_generating();
            apply({.session = application_.controller().receive()});
            // A finished turn must offer a fresh prompt even when no further
            // stdin activity re-armed it (for example after Ctrl-C cancel).
            if (was_generating && !application_.controller().is_generating()) {
                prompt_needed_ = true;
            }
        }

        // Ports should suppress input events while include_input is false.
        // Keep the boundary defensive as an already-issued asynchronous file
        // read can complete after backpressure is applied.
        if (include_input
            && (ready.input_ready() || ready.input_closed())) {
            enqueue(port_.take_lines());
            prompt_needed_ = true;
        }
        if (include_input
            && (ready.input_closed() || port_.input_closed())) {
            input_done_ = true;
        }

        pump();
        if (!emit()) {
            return finish(1);
        }
    }
}

void ConsoleSession::apply(ApplicationResult result) {
    if (result.notice && !result.notice->empty()) {
        port_.notices() << sanitize_console_text(*result.notice) << '\n';
    }
    if (result.list) {
        port_.notices() << sanitize_console_text(result.list->title) << ":\n";
        for (const std::string& row : result.list->rows) {
            port_.notices() << sanitize_console_text(row) << '\n';
        }
    }
    if (result.session_changed) {
        emitter_ = std::make_unique<TranscriptEmitter>(
            port_.transcript(),
            show_addressing(
                application_.controller().characters(),
                application_.controller().transcript().view()),
            !options_.show_prompt);
        // A fresh emitter must observe restored entries before subsequent
        // input can add to the target transcript.  Otherwise those entries
        // are mistaken for restored history and can be echoed on a TTY.
        if (!emit()) {
            output_failed_ = true;
            end_session_ = true;
            return;
        }
        prompt_needed_ = true;
    }
    end_session_ = end_session_ || result.exit_requested || result.session.controller_ended;
}

void ConsoleSession::enqueue(std::vector<std::string> lines) {
    for (std::string& line : lines) {
        // A completion notification and more input may arrive in the same
        // wait.  Drain already-read ordinary work first so a navigation
        // command cannot overtake it into a different session.
        pump();
        if (end_session_) {
            return;
        }
        // Application commands have deliberately different busy semantics:
        // /help is available and navigation is rejected immediately, never
        // silently retained behind a model response.
        const auto application_command = parse_application_command(line);
        if (application_command) {
            if (const auto* command = std::get_if<ApplicationCommand>(
                    &*application_command);
                command != nullptr
                && !application_.controller().is_generating()
                && (command->kind == ApplicationCommandKind::open
                    || command->kind == ApplicationCommandKind::create)
                && !emit()) {
                output_failed_ = true;
                end_session_ = true;
                return;
            }
            ApplicationDispatcher dispatcher(application_);
            apply(dispatcher.handle(std::move(line)));
            continue;
        }
        const Command command = parse_command(line);
        if (command.kind == CommandKind::stop && command.argument.empty()) {
            ApplicationDispatcher dispatcher(application_);
            apply(dispatcher.handle(std::move(line)));
            continue;
        }
        if (command.kind == CommandKind::exit && command.argument.empty()) {
            end_session_ = true;
            queue_.clear();
            return;
        }
        queue_.push_back(std::move(line));
        // Pump between lines from the same read. This makes
        // "prompt\n/stop\n" observe the prompt as active while still queuing
        // ordinary prompt batches behind the in-flight response batch.
        pump();
    }
}

void ConsoleSession::pump() {
    while (!end_session_ && !queue_.empty()
        && !application_.controller().is_generating()) {
        std::string line = std::move(queue_.front());
        queue_.pop_front();
        const auto application_command = parse_application_command(line);
        if (application_command
            && std::holds_alternative<ApplicationCommand>(*application_command)) {
            const ApplicationCommandKind kind =
                std::get<ApplicationCommand>(*application_command).kind;
            if ((kind == ApplicationCommandKind::open || kind == ApplicationCommandKind::create)
                && !emit()) {
                output_failed_ = true;
                end_session_ = true;
                return;
            }
        }
        ApplicationDispatcher dispatcher(application_);
        apply(dispatcher.handle(std::move(line)));
    }
}

bool ConsoleSession::emit() {
    emitter_->write(application_.controller().transcript().view());
    if (!port_.flush()) {
        port_.notices() << "Failed to write console transcript.\n";
        return false;
    }
    emitter_->commit();
    return true;
}

int ConsoleSession::finish(int exit_code) {
    if (!port_.finish_transcript() && exit_code == 0) {
        port_.notices() << "Failed to write console transcript.\n";
        exit_code = 1;
    }
    try {
        application_.shutdown();
    } catch (...) {
        if (exit_code == 0) {
            throw;
        }
    }
    return exit_code;
}

} // namespace cha
