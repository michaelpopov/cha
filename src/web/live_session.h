#pragma once

#include "session/controller_update.h"
#include "session/opened_session.h"
#include "session/session_identity.h"
#include "web/browser_connection_state.h"
#include "web/command_queue.h"
#include "web/owner_wake_signal.h"
#include "web/protocol.h"
#include "web/session_projection.h"
#include "web/sse_mailbox.h"
#include "web/web_settings.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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

// The one construction operation the web host still needs. Session
// construction combines application-owned model and repository data, and it
// must happen on the actor's own owner thread, so the actor calls back out for
// it exactly once. It always returns the production-shaped result containing a
// concrete SessionController; there is no test-only alternative.
using SessionOpener = std::function<OpenedSession(
    const SessionIdentity&,
    cha::WakeNotifier&)>;

// Owner-thread monotonic time seam; an empty function selects steady_clock.
using LiveSessionClock =
    std::function<std::chrono::steady_clock::time_point()>;

// Validates the per-actor bounds a LiveSession needs. The manager calls it at
// construction so a misconfigured server fails at startup rather than at its
// first open.
[[nodiscard]] WebSettings validate_live_session_settings(WebSettings settings);

enum class LiveSessionState {
    starting,
    running,
    stopping,
    finished,
};

// The precise outcome one open waiter observes. It is separate from the broad
// lifecycle because an open timeout is a waiter outcome, not an actor
// transition: timing out never cancels startup.
enum class LiveSessionStartResult {
    ready,
    busy,
    not_found,
    failed,
    shutting_down,
};

// One complete live-session actor: its permanent owner thread, the concrete
// SessionController that thread exclusively owns, the bounded command queue
// HTTP threads submit through, the coalescing owner wake signal, browser and
// presentation state, the SSE mailbox, and one lifecycle record.
//
// Routes retain a shared pointer to this object and use only the command API.
// They never reach the controller, the wake signal, or the thread.
class LiveSession {
public:
    // Destruction never joins: the manager is the join authority and makes the
    // owner thread non-joinable before the last map reference is released.
    ~LiveSession();
    LiveSession(const LiveSession&) = delete;
    LiveSession& operator=(const LiveSession&) = delete;

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

    [[nodiscard]] const SessionIdentity& identity() const noexcept {
        return identity_;
    }
    [[nodiscard]] LiveSessionState lifecycle();

private:
    // Construction, owner-thread startup, startup waiting, finished waiting,
    // and joining are collection-level operations with no route-facing
    // meaning. Keeping construction private prevents an actor from existing in
    // Starting forever without the manager-owned thread that can resolve it.
    friend class LiveSessionManager;

    LiveSession(
        WebSettings settings,
        SessionIdentity identity,
        SessionOpener opener,
        LiveSessionClock clock = {});
    // Starts the permanent owner thread. The caller must already have made
    // this actor reachable so the thread's raw `this` cannot dangle.
    void start_owner();
    // Resolves startup as failed without ever running an owner thread.
    void resolve_unstarted(LiveSessionStartResult result);
    // Empty until the owner resolves startup; an interrupted wait reports the
    // manager-stopping flag instead of manufacturing a result.
    [[nodiscard]] std::optional<LiveSessionStartResult> wait_for_start(
        std::chrono::milliseconds deadline);
    void wake_start_waiters();
    [[nodiscard]] bool wait_until_finished(
        std::chrono::steady_clock::time_point deadline);
    [[nodiscard]] bool owner_pending() const;
    void join_finished() noexcept;
    // Final safety join for normal manager destruction and controlled tests.
    // Production reaches it only after bounded shutdown proved every owner
    // finished; grace expiry takes the no-destructor exit path instead.
    void join_owner() noexcept;

    void owner_main();
    [[nodiscard]] bool open_controller();
    [[nodiscard]] bool commit_running();
    void publish_finished() noexcept;
    void owner_loop();
    void execute(OwnerCommand command);
    void apply_notification(OwnerNotification notification);
    [[nodiscard]] SessionSnapshot make_snapshot();
    [[nodiscard]] WebPresentationState presentation(
        SessionLifecycle lifecycle,
        std::optional<ShutdownReason> shutdown_reason = std::nullopt) const;
    bool apply_notice(const std::optional<std::string>& notice);
    [[nodiscard]] ShutdownReason mark_stopping(ShutdownReason reason);
    void publish_update(
        ControllerStateUpdate state,
        bool presentation_changed);
    void publish_current_snapshot();
    void log_generation_transitions(const SessionSnapshot& current);
    void publish_final(ShutdownReason reason);
    void log_fatal_once() noexcept;
    void log_event(std::string_view event) const noexcept;
    void teardown(ShutdownReason reason, bool skip_final_drain) noexcept;

    const SessionIdentity identity_;
    WebSettings settings_;
    SessionOpener opener_;
    LiveSessionClock clock_;
    std::shared_ptr<OwnerWakeSignal> notifier_;
    std::shared_ptr<SseMailbox> mailbox_;
    CommandQueue commands_;

    // The one lifecycle record. It is the only actor state shared between the
    // owner thread, HTTP threads, and the manager.
    mutable std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_changed_;
    LiveSessionState state_{LiveSessionState::starting};
    std::optional<LiveSessionStartResult> start_result_;
    bool start_waiters_woken_{};
    bool stopping_{};
    ShutdownReason shutdown_reason_{ShutdownReason::browser_disconnected};
    bool teardown_started_{};
    std::thread owner_;
    bool joined_{};

    // Owner thread only. Snapshot construction and controller access must stay
    // on that thread; none of this needs the lifecycle mutex.
    // Declared before everything that borrows the workspace generation, so that
    // it is destroyed after them even if teardown() never ran.
    std::shared_ptr<const void> workspace_lifetime_;
    std::unique_ptr<SessionController> controller_;
    SessionDescriptor descriptor_;
    std::function<void(std::string_view)> persist_default_character_;
    std::function<void(std::string_view)> persist_default_persona_;
    std::optional<std::string> notice_;
    // The only generation facts the actor retains: enough to recognize start
    // and terminal transitions without caching a protocol snapshot.
    bool logged_generation_active_{};
    std::optional<RequestId> logged_active_request_;
    bool fatal_logged_{};
    BrowserConnectionState browser_connection_;
    bool has_connected_sse_{};
};

} // namespace cha::web
