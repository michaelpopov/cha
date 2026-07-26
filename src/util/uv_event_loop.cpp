#include "util/uv_event_loop.h"

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>

namespace cha {
namespace {

std::runtime_error uv_error(const char* action, int status) {
    return std::runtime_error(
        std::string(action) + ": " + uv_strerror(status));
}

[[noreturn]] void fatal_uv_error(const char* action, int status) noexcept {
    std::fputs("Fatal: ", stderr);
    std::fputs(action, stderr);
    std::fputs(": ", stderr);
    std::fputs(uv_strerror(status), stderr);
    std::fputc('\n', stderr);
    std::abort();
}

} // namespace

UvEventLoop::UvEventLoop() {
    const int loop_status = uv_loop_init(&loop_);
    if (loop_status < 0) {
        throw uv_error("Failed to initialize event loop", loop_status);
    }

    async_.data = this;
    const int async_status =
        uv_async_init(&loop_, &async_, receive_notification);
    if (async_status < 0) {
        (void)uv_loop_close(&loop_);
        throw uv_error(
            "Failed to initialize event-loop notifier",
            async_status);
    }
}

UvEventLoop::~UvEventLoop() {
    close(*reinterpret_cast<uv_handle_t*>(&async_));
    const int status = uv_loop_close(&loop_);
    if (status < 0) {
        fatal_uv_error("failed to close event loop", status);
    }
}

void UvEventLoop::wake() noexcept {
    const int status = uv_async_send(&async_);
    if (status < 0) {
        fatal_uv_error("failed to signal event-loop notifier", status);
    }
}

uv_loop_t& UvEventLoop::native_loop() noexcept {
    return loop_;
}

void UvEventLoop::run_once() noexcept {
    (void)uv_run(&loop_, UV_RUN_ONCE);
}

bool UvEventLoop::take_notification() noexcept {
    return std::exchange(notified_, false);
}

void UvEventLoop::close(uv_handle_t& handle) noexcept {
    bool closed = false;
    handle.data = &closed;
    uv_close(&handle, [](uv_handle_t* value) {
        *static_cast<bool*>(value->data) = true;
    });
    while (!closed) {
        (void)uv_run(&loop_, UV_RUN_NOWAIT);
    }
}

void UvEventLoop::receive_notification(uv_async_t* handle) {
    static_cast<UvEventLoop*>(handle->data)->notified_ = true;
}

} // namespace cha
