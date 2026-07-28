#pragma once

#include "util/wake_notifier.h"

#include <uv.h>

namespace cha {

// Owns one libuv loop and its cross-thread async wake handle. Frontends add
// their input and signal handles to native_loop(), then drive the loop from
// their main thread. uv_async_send() coalesces wakes, which is sufficient
// because every frontend drains the complete foreground event channel after a
// wake.
class UvEventLoop final : public WakeNotifier {
public:
    UvEventLoop();
    ~UvEventLoop() override;

    UvEventLoop(const UvEventLoop&) = delete;
    UvEventLoop& operator=(const UvEventLoop&) = delete;

    void wake() noexcept override;
    uv_loop_t& native_loop() noexcept;
    void run_once() noexcept;
    bool take_notification() noexcept;

    // Stops and closes a frontend-owned handle synchronously. The caller must
    // first stop any operation associated with the handle.
    void close(uv_handle_t& handle) noexcept;

private:
    static void receive_notification(uv_async_t* handle);

    uv_loop_t loop_{};
    uv_async_t async_{};
    bool notified_{};
};

} // namespace cha
