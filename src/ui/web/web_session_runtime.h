#pragma once

#include "session/session_update.h"
#include "ui/web/browser_connection_state.h"
#include "ui/web/command_queue.h"
#include "ui/web/protocol.h"
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
#include <variant>

namespace cha {

class SessionController;

} // namespace cha

namespace cha::web {

class SseMailbox;

// An owner-observed delta has no sequence number. The sink/mailbox assigns one
// only when it stores a new append payload; merging a pending candidate must
// consume no additional value.
struct WebAppendCandidate {
    AppendTarget target;
    std::string text;
};

// Minimal controller boundary: it keeps fakes out of session/ and ensures all
// controller work remains on the runtime's permanent owner thread.
class WebSessionController {
public:
    virtual ~WebSessionController() = default;
    virtual SessionUpdate handle_raw_input(std::string input) = 0;
    virtual SessionUpdate request_stop() = 0;
    virtual SessionUpdate set_default_agent_id(std::string_view persona_id) = 0;
    virtual SessionEventBatch receive(std::size_t max_events) = 0;
    [[nodiscard]] virtual bool is_generating() const = 0;
    // Called only by the owner thread. The returned value owns every field and
    // contains no controller or transcript borrows.
    virtual SessionSnapshot snapshot() { return {}; }
    // Controllers may prove a text-only delta without rebuilding a snapshot.
    // Returning nullopt makes the runtime publish a full snapshot; the runtime
    // does not independently infer an append from snapshot differences.
    virtual std::optional<WebAppendCandidate> append_candidate(
        const SessionSnapshot&) {
        return std::nullopt;
    }
    virtual void shutdown() = 0;
};

using WebControllerFactory =
    std::function<std::unique_ptr<WebSessionController>(WakeNotifier&)>;

struct WebSessionMetadata {
    ForumSummary forum;
    std::string session_id;
    std::string session_label;
};

// This is deliberately a transport-neutral final-drain seam. The SSE mailbox
// and socket writer are later blocks; the owner only publishes immutable data
// and can wait for one already-published final payload to be acknowledged.
class WebSnapshotSink {
public:
    virtual ~WebSnapshotSink() = default;
    virtual void publish(SnapshotEvent snapshot) = 0;
    // A pending snapshot cannot safely coexist with a later append. Concrete
    // sinks borrow the latest state and copy it only if they actually need to
    // collapse the append to a replacement snapshot.
    virtual void publish_append(
        WebAppendCandidate candidate,
        const SessionSnapshot& fallback_snapshot) = 0;
    virtual bool wait_for_written(std::chrono::milliseconds deadline) = 0;
    virtual void close() noexcept = 0;
};

struct WebRuntimeHooks {
    std::function<void()> mark_registry_stopping;
    std::function<void()> mark_finished;
    // Passing metadata makes session identity part of the logging contract;
    // registry wiring does not have to capture and duplicate it correctly.
    std::function<void(const WebSessionMetadata&)> log_fatal;
    std::function<void(const WebSessionMetadata&, std::string_view)> log_event;
};

// Owner-thread monotonic time seam; an empty function selects steady_clock.
using WebRuntimeClock =
    std::function<std::chrono::steady_clock::time_point()>;

class WebSessionRuntime {
public:
    WebSessionRuntime(
        WebControllerFactory factory,
        WebSettings settings = {},
        WebSessionMetadata metadata = {},
        std::shared_ptr<WebSnapshotSink> sink = {},
        WebRuntimeHooks hooks = {},
        WebRuntimeClock clock = {});
    WebSessionRuntime(
        WebControllerFactory factory,
        WebSettings settings,
        WebSessionMetadata metadata,
        std::shared_ptr<SseMailbox> mailbox,
        WebRuntimeHooks hooks = {},
        WebRuntimeClock clock = {});
    // Registry-owned runtimes are constructed and opened on the registry's
    // owner thread, then published before this loop starts.  This keeps the
    // controller and its permanent owner thread the same thread.
    WebSessionRuntime(
        WebSettings settings,
        WebSessionMetadata metadata,
        std::shared_ptr<SseMailbox> mailbox,
        WebRuntimeHooks hooks = {},
        WebRuntimeClock clock = {});
    // No submit() call may overlap destruction. The production session-handle
    // layer must enforce that lifetime and confirm owner exit within the
    // process shutdown grace before allowing this final join.
    ~WebSessionRuntime();
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
    [[nodiscard]] WakeNotifier& notifier_for_owner() noexcept { return notifier_; }
    void run_with_controller(std::unique_ptr<WebSessionController> controller);

private:
    WebSessionRuntime(
        WebControllerFactory factory,
        WebSettings settings,
        WebSessionMetadata metadata,
        std::shared_ptr<WebSnapshotSink> sink,
        std::shared_ptr<SseMailbox> mailbox,
        WebRuntimeHooks hooks,
        WebRuntimeClock clock);
    void owner_loop(std::unique_ptr<WebSessionController> controller);
    void execute(WebSessionController& controller, OwnerCommand command);
    void apply_notification(OwnerNotification notification);
    [[nodiscard]] SessionSnapshot make_snapshot(WebSessionController& controller);
    void apply_notice(const SessionUpdate& update);
    [[nodiscard]] ShutdownReason mark_stopping(ShutdownReason reason);
    void publish_change(
        WebSessionController& controller,
        bool force_snapshot = false);
    [[nodiscard]] bool publish_append(WebAppendCandidate candidate);
    void publish_snapshot(SessionSnapshot snapshot);
    void publish_final(WebSessionController& controller, ShutdownReason reason);
    void log_fatal_once() noexcept;
    void log_event(std::string_view event) noexcept;
    void teardown(
        std::unique_ptr<WebSessionController>& controller,
        ShutdownReason reason,
        bool skip_final_drain) noexcept;

    WakeNotifier notifier_;
    WebSettings settings_;
    CommandQueue commands_;
    // Shared by submitters and the owner thread.
    std::mutex state_mutex_;
    bool stopping_{};
    ShutdownReason shutdown_reason_{ShutdownReason::browser_disconnected};
    bool teardown_started_{};

    // Owner thread only. Snapshot construction must stay on that thread.
    std::optional<std::string> notice_;
    std::optional<SessionSnapshot> last_snapshot_;
    std::optional<AppendTarget> append_target_;
    std::optional<std::size_t> append_entry_index_;
    bool fatal_logged_{};
    BrowserConnectionState browser_connection_;
    bool has_connected_sse_{};
    WebSessionMetadata metadata_;
    std::shared_ptr<WebSnapshotSink> sink_;
    std::shared_ptr<SseMailbox> sse_mailbox_;
    WebRuntimeHooks hooks_;
    WebRuntimeClock clock_;
    // This must be last: starting the owner thread before the state it uses is
    // constructed would let it observe partially constructed runtime state.
    std::thread owner_;
};

// Production adapter; controller construction remains in later runtime/registry work.
[[nodiscard]] std::unique_ptr<WebSessionController> adapt_session_controller(
    std::unique_ptr<SessionController> controller);

} // namespace cha::web
