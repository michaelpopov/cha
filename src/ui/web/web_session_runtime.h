#pragma once

#include "session/session_update.h"
#include "ui/web/command_queue.h"
#include "ui/web/wake_notifier.h"
#include "ui/web/web_settings.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace cha {

class SessionController;

} // namespace cha

namespace cha::web {

// Minimal controller boundary: it keeps fakes out of session/ and ensures all
// controller work remains on the runtime's permanent owner thread.
class WebSessionController {
public:
    virtual ~WebSessionController() = default;
    virtual SessionUpdate handle_raw_input(std::string input) = 0;
    virtual SessionUpdate request_stop() = 0;
    virtual SessionUpdate set_default_agent_id(std::string_view persona_id) = 0;
    virtual SessionEventBatch receive(std::size_t max_events) = 0;
    virtual void shutdown() = 0;
};

using WebControllerFactory =
    std::function<std::unique_ptr<WebSessionController>(WakeNotifier&)>;

class WebSessionRuntime {
public:
    WebSessionRuntime(
        WebControllerFactory factory,
        WebSettings settings = {});
    // No submit() call may overlap destruction. The production session-handle
    // layer must enforce that lifetime and confirm owner exit within the
    // process shutdown grace before allowing this final join.
    ~WebSessionRuntime();
    WebSessionRuntime(const WebSessionRuntime&) = delete;
    WebSessionRuntime& operator=(const WebSessionRuntime&) = delete;

    [[nodiscard]] CommandSubmitResult submit(
        WebCommand command,
        std::chrono::milliseconds deadline);
    void request_shutdown();

private:
    void owner_loop(WebControllerFactory factory);
    void execute(WebSessionController& controller, OwnerCommand command);
    void apply_notice(const SessionUpdate& update);
    void mark_stopping();

    WakeNotifier notifier_;
    WebSettings settings_;
    CommandQueue commands_;
    // Shared by submitters and the owner thread.
    std::mutex state_mutex_;
    bool stopping_{};

    // Owner thread only. Snapshot construction must stay on that thread.
    std::optional<std::string> notice_;
    // This must be last: starting the owner thread before the state it uses is
    // constructed would let it observe partially constructed runtime state.
    std::thread owner_;
};

// Production adapter; controller construction remains in later runtime/registry work.
[[nodiscard]] std::unique_ptr<WebSessionController> adapt_session_controller(
    std::unique_ptr<SessionController> controller);

} // namespace cha::web
