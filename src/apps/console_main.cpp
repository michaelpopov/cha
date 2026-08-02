#include "session/session_controller.h"
#include "session/workspace.h"
#include "ui/console/console_session.h"
#include "ui/console/console_startup.h"
#include "ui/console/system_console.h"
#include "ui/console/transcript_emitter.h"
#include "ui/render/transcript_writer.h"
#include "util/environment.h"
#include "util/logging.h"
#include "util/utf8_path.h"

#include <csignal>
#include <exception>
#include <iostream>
#include <memory>
#include <algorithm>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

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

    // Console output and redirected output both use UTF-8 bytes. On Windows,
    // opt the attached console into UTF-8 interpretation while leaving
    // redirected streams untouched.
    cha::enable_console_output_utf8();

    cha::load_dotenv();
    const cha::ApplicationConfig app_config =
        cha::load_application_config();
    cha::initialize_diagnostic_logging(
        app_config.log_file,
        app_config.log_level);
    cha::log_info("Console application started");
    cha::Workspace workspace(".", app_config);
    const auto parsed = cha::parse_console_arguments(argc, argv);
    if (const auto* error = std::get_if<cha::ArgumentError>(&parsed)) {
        std::cerr << error->message << '\n';
        return error->exit_code;
    }
    const cha::ConsoleOptions& options = std::get<cha::ConsoleOptions>(parsed);
    if (options.list_forums) {
        cha::write_forum_listing(workspace, std::cout);
        return 0;
    }
    if (options.list_sessions) {
        cha::write_session_listing(workspace, options.forum, std::cout);
        return 0;
    }
    if (options.check_forum) {
        cha::write_forum_check(workspace, options.forum, std::cout);
        return 0;
    }
    const cha::PersonaRoster personas = workspace.load_personas();
    if (std::none_of(
            personas.begin(), personas.end(),
            [&options](const cha::Persona& persona) {
                return persona.id == options.persona;
            })) {
        std::cerr << "Unknown persona ID '" << options.persona << "'\n";
        return 2;
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
        // making the persona run a separate listing before reopening it.
        std::cerr << options.forum << " / " << selection.session_id
                  << " ready\n";
    }

    cha::TranscriptEmitter emitter(
        console.transcript(),
        cha::show_addressing(
            controller.characters(),
            controller.transcript().view()),
        // TTY input is already visible when typed; only pipes need a second
        // copy of the human prompt in the transcript stream.
        !input_is_tty);
    cha::ConsoleSession session(
        console,
        controller,
        emitter,
        {
            .author_id = options.persona,
            .show_prompt = input_is_tty,
            .backpressure_stdin = !input_is_tty,
        });
    const int exit_code = session.run();
    cha::log_info("Console application stopped");
    return exit_code;
}

} // namespace

int run_main(int argc, const char* const* argv) {
    try {
        return main_internal(argc, argv);
    } catch (const std::exception& error) {
        cha::log_critical("Console application terminated by an unhandled exception");
        std::cerr << "Failed: " << error.what() << '\n';
        return 1;
    }
}

#ifdef _WIN32
int wmain(int argc, wchar_t* argv[]) {
    try {
        std::vector<std::string> arguments;
        arguments.reserve(static_cast<std::size_t>(argc));
        for (int index = 0; index < argc; ++index) {
            arguments.push_back(cha::utf8_from_wide(argv[index]));
        }
        std::vector<const char*> pointers;
        pointers.reserve(arguments.size());
        for (const std::string& argument : arguments) {
            pointers.push_back(argument.c_str());
        }
        return run_main(argc, pointers.data());
    } catch (const std::exception& error) {
        std::cerr << "Failed: " << error.what() << '\n';
        return 1;
    }
}
#else
int main(int argc, const char* const* argv) {
    return run_main(argc, argv);
}
#endif
