#pragma once

#include "ui/console/console_port.h"
#include "ui/console/console_writer.h"
#include "ui/console/line_reader.h"
#include "util/uv_event_loop.h"

#include <uv.h>

#include <array>
#include <cstdio>
#include <vector>

namespace cha {

enum class StandardStream {
    input,
    output,
    error,
};

bool standard_stream_is_terminal(StandardStream stream) noexcept;
// Configures the attached Windows console to interpret narrow output as UTF-8.
// The console output code page is process-wide; redirected streams retain their
// byte-oriented UTF-8 contract.
void enable_console_output_utf8() noexcept;
// Returns whether the stream can use ANSI color after performing any
// platform setup required to enable it.
bool enable_standard_stream_color(StandardStream stream) noexcept;

// Adapts process streams and Ctrl-C to ConsolePort through one libuv loop. The
// same loop owns the uv_async_t used by agent runners, so ConsoleSession sees
// only semantic input, notification, and interrupt events.
class SystemConsole final : public ConsolePort {
public:
    SystemConsole(bool transcript_color, bool prompt_color);
    ~SystemConsole() override;

    SystemConsole(const SystemConsole&) = delete;
    SystemConsole& operator=(const SystemConsole&) = delete;

    void wake() noexcept override;
    InputEvents wait(bool include_input = true) override;
    std::vector<std::string> take_lines() override;
    bool input_closed() const override;
    bool take_interrupt() override;
    TranscriptSurface& transcript() override;
    TranscriptSurface& prompt() override;
    std::ostream& notices() override;
    bool flush() override;
    bool finish_transcript() override;

private:
    static void allocate_input(
        uv_handle_t* handle,
        std::size_t suggested_size,
        uv_buf_t* buffer);
    static void read_stream(
        uv_stream_t* stream,
        ssize_t size,
        const uv_buf_t* buffer);
    static void read_file(uv_fs_t* request);
    static void receive_interrupt(
        uv_signal_t* signal,
        int number);

    void initialize_signal();
    void initialize_input();
    void set_input_enabled(bool enabled);
    void schedule_file_read();
    void accept_bytes(const char* bytes, std::size_t size);
    void close_input();
    void fail_input();
    void cleanup_initialized_handles() noexcept;

    UvEventLoop event_loop_;
    uv_signal_t interrupt_signal_{};
    uv_tty_t tty_input_{};
    uv_pipe_t pipe_input_{};
    uv_stream_t* input_stream_{};
    uv_fs_t file_request_{};
    uv_file input_file_{-1};
    uv_handle_type input_type_{UV_UNKNOWN_HANDLE};
    bool interrupt_initialized_{};
    bool interrupt_started_{};
    bool input_initialized_{};
    bool stream_reading_{};
    bool file_read_pending_{};
    bool shutting_down_{};
    // input_exhausted_ tracks physical EOF. input_closed_ becomes visible
    // through ConsolePort only after wait() is allowed to deliver input, so a
    // completed file read cannot bypass backpressure with its final lines.
    bool input_exhausted_{};
    bool input_closed_{};
    bool interrupt_pending_{};
    bool wait_failed_{};
    std::array<char, 4096> file_buffer_{};
    uv_buf_t file_uv_buffer_{};
    std::vector<std::string> pending_lines_;
    LineReader reader_;
    ConsoleSurface transcript_surface_;
    ConsoleSurface prompt_surface_;
};

} // namespace cha
