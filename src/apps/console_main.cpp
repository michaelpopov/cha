#include "session/session_controller.h"
#include "session/workspace.h"
#include "ui/console/console_session.h"
#include "ui/console/console_startup.h"
#include "ui/console/system_console.h"
#include "ui/console/transcript_emitter.h"
#include "ui/render/transcript_writer.h"
#include "util/environment.h"
#include "util/event_fd_notifier.h"

#include <cerrno>
#include <csignal>
#include <exception>
#include <iostream>
#include <memory>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <sys/signalfd.h>
#include <unistd.h>
#include <utility>
#include <variant>

namespace {

class OwnedDescriptor {
public:
    explicit OwnedDescriptor(int value) : value_(value) {
    }
    ~OwnedDescriptor() {
        if (value_ != -1) {
            ::close(value_);
        }
    }
    int get() const {
        return value_;
    }
    int release() {
        return std::exchange(value_, -1);
    }
private:
    int value_{-1};
};

sigset_t block_interrupt() {
    sigset_t signals{};
    if (::sigemptyset(&signals) != 0
        || ::sigaddset(&signals, SIGINT) != 0) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "Failed to prepare console signal mask");
    }
    const int result = ::pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    if (result != 0) {
        throw std::system_error(
            result,
            std::generic_category(),
            "Failed to block console interrupt");
    }
    return signals;
}

bool use_color(cha::ColorMode mode, bool stream_is_tty) {
    if (mode == cha::ColorMode::always) {
        return true;
    }
    if (mode == cha::ColorMode::never) {
        return false;
    }
    return stream_is_tty;
}

int main_internal(int argc, const char* const* argv) {
    std::signal(SIGPIPE, SIG_IGN);
    const sigset_t interrupt_mask = block_interrupt();

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

    OwnedDescriptor signal_descriptor(
        ::signalfd(
            -1,
            &interrupt_mask,
            SFD_CLOEXEC | SFD_NONBLOCK));
    if (signal_descriptor.get() == -1) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "Failed to create console signal descriptor");
    }

    cha::EventFdNotifier notifier;
    cha::ConsoleSelection selection =
        cha::open_console_session(workspace, options, notifier);
    cha::SessionController& controller = *selection.controller;
    const bool input_is_tty = ::isatty(STDIN_FILENO) != 0;
    const bool output_is_tty = ::isatty(STDOUT_FILENO) != 0;
    const bool error_is_tty = ::isatty(STDERR_FILENO) != 0;
    if (input_is_tty) {
        // The resolved ID, not the requested one: a session created by --new or
        // by default has no ID on the command line, so reporting it here avoids
        // making the user run a separate listing before reopening it.
        std::cerr << options.room << " / " << selection.session_id
                  << " ready\n";
    }

    cha::SystemConsole console(
        signal_descriptor.get(),
        use_color(options.color, output_is_tty),
        use_color(options.color, error_is_tty));
    (void)signal_descriptor.release();
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
        notifier,
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
