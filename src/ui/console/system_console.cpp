#include "ui/console/system_console.h"

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace cha {
namespace {

#ifdef _WIN32
constexpr DWORD virtual_terminal_processing = 0x0004;
#endif

std::runtime_error uv_error(const char* action, int status) {
    return std::runtime_error(
        std::string(action) + ": " + uv_strerror(status));
}

void require_uv(int status, const char* action) {
    if (status < 0) {
        throw uv_error(action, status);
    }
}

std::FILE* standard_file(StandardStream stream) noexcept {
    switch (stream) {
    case StandardStream::input:
        return stdin;
    case StandardStream::output:
        return stdout;
    case StandardStream::error:
        return stderr;
    }
    return stdin;
}

uv_file file_number(std::FILE* file) noexcept {
#ifdef _WIN32
    return ::_fileno(file);
#else
    return ::fileno(file);
#endif
}

void append_all(
    std::vector<std::string>& destination,
    std::vector<std::string> source) {
    for (std::string& line : source) {
        destination.push_back(std::move(line));
    }
}

} // namespace

bool standard_stream_is_terminal(StandardStream stream) noexcept {
    const uv_file file = file_number(standard_file(stream));
    return file >= 0 && uv_guess_handle(file) == UV_TTY;
}

bool enable_standard_stream_color(StandardStream stream) noexcept {
    const uv_file file = file_number(standard_file(stream));
    if (file < 0 || uv_guess_handle(file) != UV_TTY) {
        return false;
    }
#ifdef _WIN32
    const intptr_t native = ::_get_osfhandle(file);
    if (native == -1) {
        return false;
    }
    const HANDLE handle = reinterpret_cast<HANDLE>(native);
    DWORD mode{};
    if (!::GetConsoleMode(handle, &mode)) {
        return false;
    }
    return ::SetConsoleMode(
        handle,
        mode | virtual_terminal_processing) != 0;
#else
    return true;
#endif
}

SystemConsole::SystemConsole(
    bool transcript_color,
    bool prompt_color)
    : transcript_surface_(std::cout, transcript_color),
      prompt_surface_(std::cerr, prompt_color) {
    try {
        initialize_signal();
        initialize_input();
    } catch (...) {
        cleanup_initialized_handles();
        throw;
    }
}

SystemConsole::~SystemConsole() {
    shutting_down_ = true;
    if (file_read_pending_) {
        (void)uv_cancel(reinterpret_cast<uv_req_t*>(&file_request_));
        while (file_read_pending_) {
            event_loop_.run_once();
        }
    }
    cleanup_initialized_handles();
}

void SystemConsole::wake() noexcept {
    event_loop_.wake();
}

InputEvents SystemConsole::wait(bool include_input) {
    while (true) {
        set_input_enabled(include_input && !input_exhausted_);

        const bool notification = event_loop_.take_notification();
        if (wait_failed_) {
            return InputEvents::failure();
        }

        const bool input =
            include_input && !pending_lines_.empty();
        const bool closed =
            include_input && input_exhausted_ && !input_closed_;
        input_closed_ = input_closed_ || closed;
        if (input || closed || notification || interrupt_pending_) {
            return InputEvents::ready(
                input,
                closed,
                notification,
                interrupt_pending_);
        }

        event_loop_.run_once();
    }
}

std::vector<std::string> SystemConsole::take_lines() {
    return std::exchange(pending_lines_, {});
}

bool SystemConsole::input_closed() const {
    return input_closed_;
}

bool SystemConsole::take_interrupt() {
    return std::exchange(interrupt_pending_, false);
}

TranscriptSurface& SystemConsole::transcript() {
    return transcript_surface_;
}

TranscriptSurface& SystemConsole::prompt() {
    return prompt_surface_;
}

std::ostream& SystemConsole::notices() {
    return std::cerr;
}

bool SystemConsole::flush() {
    std::cout.flush();
    return !std::cout.fail();
}

bool SystemConsole::finish_transcript() {
    transcript_surface_.finish();
    return flush();
}

void SystemConsole::allocate_input(
    uv_handle_t*,
    std::size_t suggested_size,
    uv_buf_t* buffer) {
    const std::size_t size = suggested_size == 0 ? 4096 : suggested_size;
    buffer->base = static_cast<char*>(std::malloc(size));
    buffer->len = static_cast<decltype(buffer->len)>(
        buffer->base ? size : 0);
}

void SystemConsole::read_stream(
    uv_stream_t* stream,
    ssize_t size,
    const uv_buf_t* buffer) {
    auto& console = *static_cast<SystemConsole*>(stream->data);
    if (size > 0) {
        console.accept_bytes(
            buffer->base,
            static_cast<std::size_t>(size));
    } else if (size == UV_EOF) {
        console.close_input();
    } else if (size < 0) {
        console.fail_input();
    }
    std::free(buffer->base);
}

void SystemConsole::read_file(uv_fs_t* request) {
    auto& console = *static_cast<SystemConsole*>(request->data);
    const ssize_t size = request->result;
    uv_fs_req_cleanup(request);
    console.file_read_pending_ = false;

    if (size > 0) {
        console.accept_bytes(
            console.file_buffer_.data(),
            static_cast<std::size_t>(size));
    } else if (size == 0) {
        console.close_input();
    } else if (!(console.shutting_down_ && size == UV_ECANCELED)) {
        console.fail_input();
    }
}

void SystemConsole::receive_interrupt(
    uv_signal_t* signal,
    int number) {
    if (number == SIGINT) {
        static_cast<SystemConsole*>(signal->data)
            ->interrupt_pending_ = true;
    }
}

void SystemConsole::initialize_signal() {
    require_uv(
        uv_signal_init(&event_loop_.native_loop(), &interrupt_signal_),
        "Failed to initialize console interrupt");
    interrupt_initialized_ = true;
    interrupt_signal_.data = this;
    require_uv(
        uv_signal_start(
            &interrupt_signal_,
            receive_interrupt,
            SIGINT),
        "Failed to listen for console interrupt");
    interrupt_started_ = true;
}

void SystemConsole::initialize_input() {
    input_file_ = file_number(stdin);
    if (input_file_ < 0) {
        throw std::runtime_error("Failed to identify console input");
    }
    input_type_ = uv_guess_handle(input_file_);

    if (input_type_ == UV_TTY) {
        require_uv(
            uv_tty_init(
                &event_loop_.native_loop(),
                &tty_input_,
                input_file_,
                1),
            "Failed to initialize terminal input");
        input_initialized_ = true;
        input_stream_ =
            reinterpret_cast<uv_stream_t*>(&tty_input_);
    } else if (input_type_ == UV_NAMED_PIPE) {
        require_uv(
            uv_pipe_init(
                &event_loop_.native_loop(),
                &pipe_input_,
                0),
            "Failed to initialize pipe input");
        input_initialized_ = true;
        input_stream_ =
            reinterpret_cast<uv_stream_t*>(&pipe_input_);
        require_uv(
            uv_pipe_open(&pipe_input_, input_file_),
            "Failed to open pipe input");
    } else if (input_type_ != UV_FILE) {
        throw std::runtime_error(
            "Console input is not a terminal, pipe, or file");
    }

    if (input_stream_) {
        input_stream_->data = this;
    } else {
        file_uv_buffer_ = uv_buf_init(
            file_buffer_.data(),
            static_cast<unsigned int>(file_buffer_.size()));
        file_request_.data = this;
    }
}

void SystemConsole::set_input_enabled(bool enabled) {
    if (input_stream_) {
        if (enabled && !stream_reading_) {
            const int status = uv_read_start(
                input_stream_,
                allocate_input,
                read_stream);
            if (status < 0) {
                fail_input();
            } else {
                stream_reading_ = true;
            }
        } else if (!enabled && stream_reading_) {
            const int status = uv_read_stop(input_stream_);
            if (status < 0) {
                fail_input();
            }
            stream_reading_ = false;
        }
    } else if (enabled) {
        schedule_file_read();
    }
}

void SystemConsole::schedule_file_read() {
    if (file_read_pending_ || input_exhausted_) {
        return;
    }
    file_request_.data = this;
    const int status = uv_fs_read(
        &event_loop_.native_loop(),
        &file_request_,
        input_file_,
        &file_uv_buffer_,
        1,
        -1,
        read_file);
    if (status < 0) {
        uv_fs_req_cleanup(&file_request_);
        fail_input();
        return;
    }
    file_read_pending_ = true;
}

void SystemConsole::accept_bytes(
    const char* bytes,
    std::size_t size) {
    append_all(
        pending_lines_,
        reader_.append(std::string_view(bytes, size)));
}

void SystemConsole::close_input() {
    if (input_exhausted_) {
        return;
    }
    input_exhausted_ = true;
    if (input_stream_ && stream_reading_) {
        (void)uv_read_stop(input_stream_);
        stream_reading_ = false;
    }
    append_all(pending_lines_, reader_.flush());
}

void SystemConsole::fail_input() {
    wait_failed_ = true;
    if (input_stream_ && stream_reading_) {
        (void)uv_read_stop(input_stream_);
        stream_reading_ = false;
    }
}

void SystemConsole::cleanup_initialized_handles() noexcept {
    if (input_initialized_) {
        if (stream_reading_) {
            (void)uv_read_stop(input_stream_);
            stream_reading_ = false;
        }
        event_loop_.close(
            *reinterpret_cast<uv_handle_t*>(input_stream_));
        input_initialized_ = false;
        input_stream_ = nullptr;
    }
    if (interrupt_initialized_) {
        if (interrupt_started_) {
            (void)uv_signal_stop(&interrupt_signal_);
            interrupt_started_ = false;
        }
        event_loop_.close(
            *reinterpret_cast<uv_handle_t*>(&interrupt_signal_));
        interrupt_initialized_ = false;
    }
}

} // namespace cha
