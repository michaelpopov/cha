#include "session/session_controller.h"
#include "session/workspace.h"
#include "ui/console/console_session.h"
#include "ui/console/console_startup.h"
#include "ui/console/system_console.h"
#include "ui/console/transcript_emitter.h"
#include "ui/render/transcript_writer.h"
#include "util/environment.h"

#include <csignal>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>

namespace {

bool use_color(cha::ColorMode mode, bool stream_color_enabled) {
    if (mode == cha::ColorMode::always) {
        return true;
    }
    if (mode == cha::ColorMode::never) {
        return false;
    }
    return stream_color_enabled;
}

int main_internal(int argc, const char* const* argv) {
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif

    cha::load_dotenv();
    cha::Workspace workspace;
    const auto parsed = cha::parse_console_arguments(argc, argv);
    if (const auto* error = std::get_if<cha::ArgumentError>(&parsed)) {
        std::cerr << error->message << '\n';
        return error->exit_code;
    }
    const cha::ConsoleOptions& options = std::get<cha::ConsoleOptions>(parsed);
    if (options.list_rooms) {
        cha::write_room_listing(workspace, std::cout);
        return 0;
    }
    if (options.list_sessions) {
        cha::write_session_listing(workspace, options.room, std::cout);
        return 0;
    }

    const bool input_is_tty =
        cha::standard_stream_is_terminal(
            cha::StandardStream::input);
    const bool output_color_enabled =
        options.color != cha::ColorMode::never
        && cha::enable_standard_stream_color(
            cha::StandardStream::output);
    const bool error_color_enabled =
        options.color != cha::ColorMode::never
        && cha::enable_standard_stream_color(
            cha::StandardStream::error);
    cha::SystemConsole console(
        use_color(options.color, output_color_enabled),
        use_color(options.color, error_color_enabled));
    cha::ConsoleSelection selection =
        cha::open_console_session(workspace, options, console);
    cha::SessionController& controller = *selection.controller;
    if (input_is_tty) {
        // The resolved ID, not the requested one: a session created by --new or
        // by default has no ID on the command line, so reporting it here avoids
        // making the user run a separate listing before reopening it.
        std::cerr << options.room << " / " << selection.session_id
                  << " ready\n";
    }

    cha::TranscriptEmitter emitter(
        console.transcript(),
        cha::show_addressing(
            controller.personas(),
            controller.transcript()),
        // TTY input is already visible when typed; only pipes need a second
        // copy of the human prompt in the transcript stream.
        !input_is_tty);
    cha::ConsoleSession session(
        console,
        controller,
        emitter,
        {
            .show_prompt = input_is_tty,
            .backpressure_stdin = !input_is_tty,
        });
    return session.run();
}

} // namespace

int main(int argc, const char* const* argv) {
    try {
        return main_internal(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "Failed: " << error.what() << '\n';
        return 1;
    }
}
