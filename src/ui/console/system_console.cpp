#include "ui/console/system_console.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <sys/signalfd.h>
#include <unistd.h>

namespace cha {
namespace {

void append_all(
    std::vector<std::string>& destination,
    std::vector<std::string> source) {
    for (std::string& line : source) {
        destination.push_back(std::move(line));
    }
}

} // namespace

SystemConsole::SystemConsole(int signal_fd, bool color)
    : signal_fd_(signal_fd),
      surface_(std::cout, color) {
    original_input_flags_ = ::fcntl(STDIN_FILENO, F_GETFL);
    if (original_input_flags_ == -1
        || ::fcntl(
            STDIN_FILENO,
            F_SETFL,
            original_input_flags_ | O_NONBLOCK) == -1) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "Failed to make console input non-blocking");
    }
}

SystemConsole::~SystemConsole() {
    if (original_input_flags_ != -1) {
        (void)::fcntl(STDIN_FILENO, F_SETFL, original_input_flags_);
    }
    if (signal_fd_ != -1) {
        ::close(signal_fd_);
    }
}

InputEvents SystemConsole::wait(
    int notification_fd,
    bool include_input) {
    const InputEvents result =
        wait_for_input_events(notification_fd, signal_fd_, include_input);
    may_read_ = include_input
        && (result.input_ready() || result.input_closed());
    return result;
}

std::vector<std::string> SystemConsole::take_lines() {
    std::vector<std::string> submissions;
    if (!may_read_ || input_closed_) {
        return submissions;
    }
    may_read_ = false;

    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t size =
            ::read(STDIN_FILENO, buffer.data(), buffer.size());
        if (size > 0) {
            append_all(
                submissions,
                reader_.append(std::string_view(
                    buffer.data(),
                    static_cast<std::size_t>(size))));
            continue;
        }
        if (size == 0) {
            input_closed_ = true;
            append_all(submissions, reader_.flush());
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        throw std::system_error(
            errno,
            std::generic_category(),
            "Failed to read console input");
    }
    return submissions;
}

bool SystemConsole::input_closed() const {
    return input_closed_;
}

bool SystemConsole::take_interrupt() {
    signalfd_siginfo information{};
    while (true) {
        const ssize_t size =
            ::read(signal_fd_, &information, sizeof(information));
        if (size == static_cast<ssize_t>(sizeof(information))) {
            return information.ssi_signo == SIGINT;
        }
        if (size == -1 && errno == EINTR) {
            continue;
        }
        if (size == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return false;
        }
        throw std::system_error(
            errno,
            std::generic_category(),
            "Failed to read console signal");
    }
}

TranscriptSurface& SystemConsole::transcript() {
    return surface_;
}

std::ostream& SystemConsole::notices() {
    return std::cerr;
}

bool SystemConsole::flush() {
    std::cout.flush();
    return !std::cout.fail();
}

bool SystemConsole::finish_transcript() {
    surface_.finish();
    return flush();
}

} // namespace cha
