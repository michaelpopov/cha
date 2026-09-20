#pragma once

#include "app/runtime_settings.h"
#include "app/session_output.h"
#include "chat/session_identity.h"
#include "session/controller_update.h"
#include "session/opened_session.h"
#include "util/wake_notifier.h"
#include "web/command_queue.h"
#include "web/protocol.h"
#include "web/session_projection.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace cha {

class SessionController;

using SessionOpener = std::function<OpenedSession(
    const FullSessionId&,
    std::shared_ptr<WakeNotifier>)>;

using LiveSessionClock =
    std::function<std::chrono::steady_clock::time_point()>;

[[nodiscard]] app::RuntimeSettings validate_live_session_settings(
    app::RuntimeSettings settings);

enum class LiveSessionState {
    starting,
    running,
    stopping,
    finished,
};

class LiveSession;

// The small interface retained by a session endpoint. The concrete runtime
// owns the sole session thread, command queue, controller map, and notifier.
class SessionRuntime {
public:
    ~SessionRuntime();
    SessionRuntime(const SessionRuntime&) = delete;
    SessionRuntime& operator=(const SessionRuntime&) = delete;

    [[nodiscard]] std::variant<std::shared_ptr<CommandReply>, ErrorCode> enqueue(
        const FullSessionId& identity,
        std::uint64_t instance,
        WebCommand command,
        std::uint64_t subscribe_ticket);
    void wake() noexcept;

private:
    friend class LiveSession;
    friend class LiveSessionManager;
    SessionRuntime(
        app::RuntimeSettings settings,
        SessionOpener opener,
        LiveSessionClock clock);
    [[nodiscard]] std::shared_ptr<LiveSession> make_session(
        FullSessionId identity,
        std::uint64_t instance);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A stable endpoint for one live controller instance. It can outlive
// retirement so in-flight delivery and stale commands fail safely. The
// controller-bearing portion is private and is constructed, used, cleared,
// and destroyed exclusively by SessionRuntime's single thread.
class LiveSession : public std::enable_shared_from_this<LiveSession> {
public:
    ~LiveSession();
    LiveSession(const LiveSession&) = delete;
    LiveSession& operator=(const LiveSession&) = delete;

    [[nodiscard]] CommandSubmitResult submit(
        WebCommand command,
        std::chrono::milliseconds deadline);
    [[nodiscard]] std::variant<std::shared_ptr<CommandReply>, ErrorCode>
    enqueue(WebCommand command);
    [[nodiscard]] CommandSubmitResult snapshot(
        std::chrono::milliseconds deadline);
    [[nodiscard]] CommandSubmitResult subscribe(
        SubscribeCommand command,
        std::chrono::milliseconds deadline);
    [[nodiscard]] CommandSubmitResult unsubscribe(
        UnsubscribeCommand command,
        std::chrono::milliseconds deadline);
    void request_shutdown(
        ShutdownReason reason = ShutdownReason::session_closed);
    void request_retire_when_idle();
    void cancel_retirement();
    [[nodiscard]] bool idle_for_retirement() const;
    [[nodiscard]] std::shared_ptr<const app::SessionOutputItem>
    take_output();
    void acknowledge_output() noexcept;
    void refresh_presentation();
    [[nodiscard]] std::shared_ptr<app::SessionOutput> output() const {
        return output_;
    }

    [[nodiscard]] const FullSessionId& identity() const noexcept {
        return identity_;
    }
    [[nodiscard]] LiveSessionState lifecycle() const noexcept {
        return state_.load();
    }

private:
    friend class SessionRuntime;
    friend class LiveSessionManager;

    LiveSession(
        FullSessionId identity,
        std::uint64_t instance,
        std::weak_ptr<SessionRuntime> runtime,
        std::size_t pending_append_byte_limit);

    void install(OpenedSession opened);
    void set_running();
    void execute(OwnerCommand command);
    [[nodiscard]] bool receive_events(std::size_t batch_size);
    [[nodiscard]] bool retirement_requested() const noexcept;
    [[nodiscard]] bool shutdown_requested() const noexcept;
    [[nodiscard]] ShutdownReason shutdown_reason() const noexcept;
    void fail_current(std::shared_ptr<CommandReply> reply = {});
    void finalize(ShutdownReason reason) noexcept;

    [[nodiscard]] SessionSnapshot make_snapshot();
    [[nodiscard]] WebPresentationState presentation(
        SessionLifecycle lifecycle,
        std::optional<ShutdownReason> shutdown_reason = std::nullopt) const;
    bool apply_notice(const std::optional<std::string>& notice);
    void publish_update(
        ControllerStateUpdate state,
        bool presentation_changed);
    void publish_current_snapshot();
    void mirror_if_changed();
    void log_generation_transitions(const ControllerView& current);
    void log_fatal_once() noexcept;
    void log_event(std::string_view event) const noexcept;
    void raise_shutdown_reason(ShutdownReason reason) noexcept;

    const FullSessionId identity_;
    const std::uint64_t instance_;
    std::weak_ptr<SessionRuntime> runtime_;
    std::shared_ptr<app::SessionOutput> output_;

    std::atomic<LiveSessionState> state_{LiveSessionState::starting};
    std::atomic<bool> generating_{};
    std::atomic<bool> retire_when_idle_{};
    std::atomic<bool> stopping_{};
    struct ShutdownState {
        ShutdownReason reason{ShutdownReason::session_closed};
        bool finalized{};
    };
    std::atomic<ShutdownState> shutdown_{};
    std::atomic<std::uint64_t> subscribe_ticket_{};

    // Runtime-thread only.
    std::unique_ptr<SessionController> controller_;
    std::function<std::set<EntryId>()> cached_audio_entries_;
    std::string label_;
    std::function<void(std::string_view)> persist_default_character_;
    std::function<void(
        std::string_view,
        std::span<const TranscriptEntry>)> mirror_;
    std::size_t mirrored_revision_{};
    std::string mirrored_label_;
    std::optional<std::string> notice_;
    bool logged_generation_active_{};
    std::optional<RequestId> logged_active_request_;
    bool fatal_logged_{};
    std::optional<SubscribeCommand> active_subscription_;
};

} // namespace cha
