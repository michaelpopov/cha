#pragma once

#include "session/controller_update.h"
#include "session/controller_view.h"
#include "session/opened_session.h"
#include "ui/web/browser_connection_state.h"
#include "ui/web/command_queue.h"
#include "ui/web/protocol.h"
#include "ui/web/session_projection.h"
#include "ui/web/owner_wake_signal.h"
#include "ui/web/web_settings.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace cha {

class SessionController;

} // namespace cha

namespace cha::web {

class SseMailbox;

// Minimal controller boundary: it keeps fakes out of session/ and ensures all
// controller work remains on the runtime's permanent owner thread.
class WebSessionController {
public:
    virtual ~WebSessionController() = default;
    virtual CommandResult handle_raw_input(
        std::string_view author_id,
        std::string input) = 0;
    virtual ControllerUpdate request_stop() = 0;
    virtual ControllerUpdate set_default_agent_id(std::string_view character_id) = 0;
    virtual ControllerEventBatch receive(std::size_t max_events) = 0;
    [[nodiscard]] virtual bool is_generating() const = 0;
    // Called only by the owner thread and consumed synchronously: the returned
    // view borrows controller storage that the next mutation invalidates.
    [[nodiscard]] virtual ControllerView view() const = 0;
    virtual void shutdown() = 0;
};

// Whether a sink represented one controller-proven append exactly, or needs the
// owner to publish a current full snapshot instead.
enum class AppendPublishResult {
    Accepted,
    SnapshotRequired,
};

// This is deliberately a transport-neutral final-drain seam. The SSE mailbox
// and socket writer are later blocks; the owner only publishes immutable data
// and can wait for one already-published final payload to be acknowledged.
class WebSnapshotSink {
public:
    virtual ~WebSnapshotSink() = default;
    virtual void publish(SnapshotEvent snapshot) = 0;
    // A pending snapshot cannot safely coexist with a later append. Rejecting
    // the append leaves the sink's payload untouched and obliges the owner to
    // project and publish a current full snapshot.
    [[nodiscard]] virtual AppendPublishResult publish_append(TextAppend append) = 0;
    virtual bool wait_for_written(std::chrono::milliseconds deadline) = 0;
    virtual void close() noexcept = 0;
};

struct WebRuntimeHooks {
    std::function<void()> mark_registry_stopping;
    std::function<void()> mark_finished;
    // The runtime supplies its own descriptor, so session identity is part of
    // the logging contract and a hook cannot report a session other than the
    // one it was installed on. Log IDs only: the label is user-supplied text.
    std::function<void(const SessionDescriptor&)> log_fatal;
    std::function<void(const SessionDescriptor&, std::string_view)> log_event;
};

// Owner-thread monotonic time seam; an empty function selects steady_clock.
using WebRuntimeClock =
    std::function<std::chrono::steady_clock::time_point()>;

class WebSessionRuntime {
public:
    // Registry-owned runtimes are constructed and opened on the registry's
    // owner thread, then published before this loop starts.  This keeps the
    // controller and its permanent owner thread the same thread.
    WebSessionRuntime(
        WebSettings settings,
        SessionDescriptor descriptor,
        std::shared_ptr<SseMailbox> mailbox,
        WebRuntimeHooks hooks = {},
        WebRuntimeClock clock = {},
        std::shared_ptr<OwnerWakeSignal> notifier = {});
    ~WebSessionRuntime() = default;
    WebSessionRuntime(const WebSessionRuntime&) = delete;
    WebSessionRuntime& operator=(const WebSessionRuntime&) = delete;

    [[nodiscard]] CommandSubmitResult submit(
        WebCommand command,
        std::chrono::milliseconds deadline);
    [[nodiscard]] CommandSubmitResult snapshot(
        std::chrono::milliseconds deadline);
    [[nodiscard]] CommandSubmitResult connect_sse(
        std::chrono::milliseconds deadline);
    void disconnect_sse(
        std::uint64_t connection_id,
        std::size_t collapsed_payloads) noexcept;
    void request_shutdown(
        ShutdownReason reason = ShutdownReason::browser_disconnected);
    [[nodiscard]] cha::WakeNotifier& notifier_for_owner() noexcept {
        return *notifier_;
    }
    void run_with_controller(std::unique_ptr<WebSessionController> controller);
    void run(OpenedSession opened);

protected:
    // Threadless transport-neutral seam for tests that replace the production
    // mailbox sink. The derived harness owns and joins its owner thread.
    WebSessionRuntime(
        WebSettings settings,
        SessionDescriptor descriptor,
        std::shared_ptr<WebSnapshotSink> sink,
        std::shared_ptr<SseMailbox> mailbox,
        WebRuntimeHooks hooks = {},
        WebRuntimeClock clock = {},
        std::shared_ptr<OwnerWakeSignal> notifier = {});

private:
    void owner_loop(
        std::unique_ptr<WebSessionController> controller,
        bool mark_finished = true);
    void execute(WebSessionController& controller, OwnerCommand command);
    void apply_notification(OwnerNotification notification);
    [[nodiscard]] SessionSnapshot make_snapshot(WebSessionController& controller);
    [[nodiscard]] WebPresentationState presentation(
        SessionLifecycle lifecycle,
        std::optional<ShutdownReason> shutdown_reason = std::nullopt) const;
    bool apply_notice(const std::optional<std::string>& notice);
    [[nodiscard]] ShutdownReason mark_stopping(ShutdownReason reason);
    void publish_update(
        WebSessionController& controller,
        ControllerStateUpdate state,
        bool presentation_changed);
    void publish_current_snapshot(WebSessionController& controller);
    void log_generation_transitions(const SessionSnapshot& current);
    void publish_final(WebSessionController& controller, ShutdownReason reason);
    void log_fatal_once() noexcept;
    void log_event(std::string_view event) noexcept;
    void teardown(
        std::unique_ptr<WebSessionController>& controller,
        ShutdownReason reason,
        bool skip_final_drain,
        bool mark_finished) noexcept;
    void mark_finished() noexcept;

    std::shared_ptr<OwnerWakeSignal> notifier_;
    WebSettings settings_;
    CommandQueue commands_;
    // Shared by submitters and the owner thread.
    std::mutex state_mutex_;
    bool stopping_{};
    ShutdownReason shutdown_reason_{ShutdownReason::browser_disconnected};
    bool teardown_started_{};

    // Owner thread only. Snapshot construction must stay on that thread.
    std::optional<std::string> notice_;
    // The only generation facts the runtime retains: enough to recognize start
    // and terminal transitions without caching a protocol snapshot.
    bool logged_generation_active_{};
    std::optional<RequestId> logged_active_request_;
    bool fatal_logged_{};
    BrowserConnectionState browser_connection_;
    bool has_connected_sse_{};
    SessionDescriptor descriptor_;
    std::shared_ptr<WebSnapshotSink> sink_;
    std::shared_ptr<SseMailbox> sse_mailbox_;
    WebRuntimeHooks hooks_;
    WebRuntimeClock clock_;
};

// Production adapter: it borrows the controller held by OpenedSession for the
// complete owner loop, so releasing the session also releases its lease.
[[nodiscard]] std::unique_ptr<WebSessionController> adapt_session_controller(
    SessionController& controller);
// Owning counterpart for tests that drive a real controller through
// run_with_controller() without assembling an OpenedSession. Production
// startup always uses the borrowing overload above.
[[nodiscard]] std::unique_ptr<WebSessionController> adapt_session_controller(
    std::unique_ptr<SessionController> controller);

} // namespace cha::web
