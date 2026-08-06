#include "application/chat_application.h"
#include "session/workspace.h"
#include "ui/console/console_session.h"
#include "ui/console/console_startup.h"
#include "ui/console/system_console.h"
#include "util/environment.h"
#include "util/logging.h"
#include "util/utf8_path.h"

#include <csignal>
#include <exception>
#include <iostream>
#include <memory>
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

    const auto parsed = cha::parse_console_arguments(argc, argv);
    if (const auto* error = std::get_if<cha::ArgumentError>(&parsed)) {
        std::cerr << error->message << '\n';
        return error->exit_code;
    }
    const cha::ConsoleOptions& options = std::get<cha::ConsoleOptions>(parsed);

    cha::load_dotenv();
    const cha::WorkspaceConfig workspace_config = cha::load_workspace_config();
    cha::initialize_diagnostic_logging(
        workspace_config.log_file,
        workspace_config.log_level);
    cha::log_info("Console application started");
    cha::Workspace workspace(".", workspace_config);
    if (options.check) {
        for (const std::string& forum : workspace.forums()) {
            (void)workspace.check_forum(forum);
        }
        std::cout << "Workspace is valid.\n";
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
    cha::ChatApplication application(workspace, console);
    if (input_is_tty) {
        std::cerr << "Entrance / Welcome ready\n";
    }
    cha::ConsoleSession session(
        console,
        application,
        {
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
