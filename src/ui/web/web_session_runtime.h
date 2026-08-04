#pragma once

#include "session/opened_session.h"
#include "session/session_state.h"
#include "ui/web/web_session_update.h"
#include "ui/web/browser_connection_state.h"
#include "ui/web/command_queue.h"
#include "ui/web/protocol.h"
#include "ui/web/session_projection.h"
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
    virtual WebSessionUpdate handle_raw_input(
        std::string_view author_id,
        std::string input) = 0;
    virtual WebSessionUpdate request_stop() = 0;
    virtual WebSessionUpdate set_default_agent_id(std::string_view character_id) = 0;
    virtual WebSessionEventBatch receive(std::size_t max_events) = 0;
    [[nodiscard]] virtual bool is_generating() const = 0;
    // Called only by the owner thread. The returned value owns every field and
    // contains no controller or transcript borrows.
    virtual SessionState state() { return {}; }
    // Controllers may prove a text-only delta without rebuilding a snapshot.
    // Returning nullopt makes the runtime publish a full snapshot; the runtime
    // does not independently infer an append from snapshot differences.
    virtual std::optional<SessionAppendProjection> text_append_since(
        const SessionStateCursor&) {
        return std::nullopt;
    }
    virtual void shutdown() = 0;
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
        std::shared_ptr<WakeNotifier> notifier = {});
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
    [[nodiscard]] WakeNotifier& notifier_for_owner() noexcept { return *notifier_; }
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
        std::shared_ptr<WakeNotifier> notifier = {});

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
    void apply_notice(const WebSessionUpdate& update);
    [[nodiscard]] ShutdownReason mark_stopping(ShutdownReason reason);
    void publish_change(
        WebSessionController& controller,
        bool force_snapshot = false);
    [[nodiscard]] bool publish_append(
        WebAppendCandidate candidate,
        SessionStateCursor cursor);
    void publish_snapshot(
        SessionSnapshot snapshot,
        std::optional<SessionStateCursor> cursor = std::nullopt);
    void publish_final(WebSessionController& controller, ShutdownReason reason);
    void log_fatal_once() noexcept;
    void log_event(std::string_view event) noexcept;
    void teardown(
        std::unique_ptr<WebSessionController>& controller,
        ShutdownReason reason,
        bool skip_final_drain,
        bool mark_finished) noexcept;
    void mark_finished() noexcept;

    std::shared_ptr<WakeNotifier> notifier_;
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
    std::optional<SessionStateCursor> last_cursor_;
    bool fatal_logged_{};
    BrowserConnectionState browser_connection_;
    bool has_connected_sse_{};
    SessionDescriptor descriptor_;
    std::shared_ptr<WebSnapshotSink> sink_;
    std::shared_ptr<SseMailbox> sse_mailbox_;
    WebRuntimeHooks hooks_;
    WebRuntimeClock clock_;
};

// Production adapter borrows the controller held by OpenedSession for the
// complete owner loop.
[[nodiscard]] std::unique_ptr<WebSessionController> adapt_session_controller(
    SessionController& controller);
[[nodiscard]] std::unique_ptr<WebSessionController> adapt_session_controller(
    std::unique_ptr<SessionController> controller);

} // namespace cha::web
