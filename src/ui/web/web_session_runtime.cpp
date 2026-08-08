#include "ui/web/web_session_runtime.h"

#include "ui/web/sse_mailbox.h"

#include "session/session_controller.h"
#include "transcript/transcript.h"
#include "ui/web/text_input.h"

#include <exception>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace cha::web {
namespace {

template<typename>
inline constexpr bool unsupported_web_command = false;

WebSettings validate_runtime_settings(WebSettings settings) {
    if (settings.command_queue_capacity == 0) {
        throw std::invalid_argument(
            "Web runtime command queue capacity must be positive");
    }
    if (settings.command_batch_size == 0 || settings.event_batch_size == 0) {
        throw std::invalid_argument(
            "Web runtime batch sizes must be positive");
    }
    if (settings.orphan_limit < settings.idle_grace) {
        throw std::invalid_argument("Web orphan limit must be at least idle grace");
    }
    return settings;
}

int shutdown_reason_priority(ShutdownReason reason) {
    switch (reason) {
    case ShutdownReason::browser_disconnected: return 0;
    case ShutdownReason::session_failed: return 1;
    case ShutdownReason::server_stopping: return 2;
    }
    return 0;
}

ShutdownReason keep_higher_priority_reason(
    ShutdownReason current,
    ShutdownReason candidate) {
    return shutdown_reason_priority(candidate) > shutdown_reason_priority(current)
        ? candidate
        : current;
}

// Teardown is best effort for ordinary session-local failures. Exhausted
// allocation capacity is not recoverable at this boundary and remains fatal to
// the process instead of being mistaken for successful cleanup.
template<typename Operation>
bool run_guarded(Operation&& operation) noexcept {
    try {
        std::forward<Operation>(operation)();
        return true;
    } catch (const std::bad_alloc&) {
        std::terminate();
    } catch (...) {
        return false;
    }
}


std::string_view generation_terminal_status(
    const SessionSnapshot& snapshot,
    const std::optional<std::uint64_t>& request_id) {
    if (!request_id) return "unknown";
    bool has_prompt = false;
    for (auto entry = snapshot.transcript.rbegin();
         entry != snapshot.transcript.rend(); ++entry) {
        if (entry->request_id != request_id) continue;
        if (entry->kind == EntryKind::human) {
            has_prompt = true;
        } else if ((entry->kind == EntryKind::agent
                       || entry->kind == EntryKind::error)
            && entry->status != EntryStatus::streaming) {
            return to_string(entry->status);
        }
    }
    // A cancelled response may end before producing answer text, in which
    // case the completed prompt is the request's only transcript entry.
    if (has_prompt) return to_string(EntryStatus::cancelled);
    // A production terminal transition has a transcript entry for its request.
    // Keep an explicit diagnostic value for malformed controller snapshots
    // instead of reporting a made-up successful outcome.
    return "unknown";
}

class SessionControllerAdapter final : public WebSessionController {
public:
    explicit SessionControllerAdapter(SessionController& controller)
        : controller_(controller) {}
    explicit SessionControllerAdapter(std::unique_ptr<SessionController> controller)
        : owned_controller_(std::move(controller)), controller_(*owned_controller_) {}

    CommandResult handle_raw_input(
        std::string_view author_id,
        std::string input) override {
        return handle_text_input(controller_, author_id, std::move(input));
    }
    ControllerUpdate request_stop() override {
        return controller_.request_stop();
    }
    ControllerUpdate set_default_agent_id(std::string_view id) override {
        return controller_.set_default_agent_by_id(id);
    }
    ControllerEventBatch receive(std::size_t max_events) override {
        return controller_.receive_events(max_events);
    }
    [[nodiscard]] bool is_generating() const override {
        return controller_.is_generating();
    }
    [[nodiscard]] ControllerView view() const override {
        return controller_.view();
    }
    void shutdown() override { controller_.shutdown(); }

private:
    std::unique_ptr<SessionController> owned_controller_;
    SessionController& controller_;
};

static_assert(std::variant_size_v<WebCommand> == 5);

} // namespace

WebSessionRuntime::WebSessionRuntime(
    WebSettings settings,
    SessionDescriptor descriptor,
    std::shared_ptr<WebSnapshotSink> sink,
    std::shared_ptr<SseMailbox> mailbox,
    WebRuntimeHooks hooks,
    WebRuntimeClock clock,
    std::shared_ptr<OwnerWakeSignal> notifier)
    : notifier_(notifier
          ? std::move(notifier)
          : std::make_shared<OwnerWakeSignal>()),
      settings_(validate_runtime_settings(std::move(settings))),
      commands_(settings_.command_queue_capacity),
      descriptor_(std::move(descriptor)),
      sink_(std::move(sink)),
      sse_mailbox_(std::move(mailbox)),
      hooks_(std::move(hooks)),
      clock_(clock ? std::move(clock) : [] {
          return std::chrono::steady_clock::now();
      }) {}

WebSessionRuntime::WebSessionRuntime(
    WebSettings settings,
    SessionDescriptor descriptor,
    std::shared_ptr<SseMailbox> mailbox,
    WebRuntimeHooks hooks,
    WebRuntimeClock clock,
    std::shared_ptr<OwnerWakeSignal> notifier)
    : WebSessionRuntime(
          std::move(settings),
          std::move(descriptor),
          mailbox,
          mailbox,
          std::move(hooks),
          std::move(clock),
          std::move(notifier)) {
    if (!sse_mailbox_) {
        throw std::invalid_argument(
            "Registry-owned web runtime needs an SSE mailbox");
    }
}

CommandSubmitResult WebSessionRuntime::submit(
    WebCommand command,
    std::chrono::milliseconds deadline) {
    auto completion = std::make_shared<CommandCompletion>();
    bool wake_owner = false;
    std::optional<ErrorCode> rejection;
    {
        std::lock_guard lock(state_mutex_);
        if (stopping_) {
            rejection = shutdown_reason_ == ShutdownReason::server_stopping
                ? ErrorCode::server_stopping
                : ErrorCode::session_not_live;
        } else {
            const CommandEnqueueResult enqueued = commands_.try_push({std::move(command), completion});
            if (!enqueued.accepted) {
                rejection = ErrorCode::command_queue_full;
            } else {
                wake_owner = enqueued.wake_owner;
            }
        }
    }
    if (rejection) return *rejection;
    if (wake_owner) notifier_->wake();
    if (auto result = completion->wait_for(deadline)) return std::move(*result);
    log_event("command_deadline_expired");
    return ErrorCode::command_timeout;
}

CommandSubmitResult WebSessionRuntime::snapshot(
    std::chrono::milliseconds deadline) {
    return submit(SnapshotCommand{}, deadline);
}

CommandSubmitResult WebSessionRuntime::connect_sse(
    std::chrono::milliseconds deadline) {
    return submit(SseConnectCommand{}, deadline);
}

void WebSessionRuntime::disconnect_sse(
    std::uint64_t connection_id,
    std::size_t collapsed_payloads) noexcept {
    if (commands_.push_notification(
            SseDisconnectNotification{connection_id, collapsed_payloads})) {
        notifier_->wake();
    }
}

void WebSessionRuntime::request_shutdown(ShutdownReason reason) {
    {
        std::lock_guard lock(state_mutex_);
        stopping_ = true;
        shutdown_reason_ = keep_higher_priority_reason(shutdown_reason_, reason);
    }
    notifier_->wake();
}

void WebSessionRuntime::run_with_controller(
    std::unique_ptr<WebSessionController> controller) {
    owner_loop(std::move(controller));
}

void WebSessionRuntime::run(OpenedSession opened) {
    std::unique_ptr<WebSessionController> controller =
        adapt_session_controller(*opened.controller);
    owner_loop(std::move(controller), false);
    // The registry's finished transition permits a same-key owner to start.
    // Release the real controller (and its cross-process lease) first; the
    // adapter above only borrowed it and its destruction cannot do that.
    opened.controller.reset();
    log_event("lease_released_owner_finished");
    mark_finished();
}

void WebSessionRuntime::owner_loop(
    std::unique_ptr<WebSessionController> controller,
    bool mark_finished_on_teardown) {
    ShutdownReason reason = ShutdownReason::browser_disconnected;
    bool fatal = false;
    try {
        if (!controller) throw std::runtime_error("Web controller factory returned null");
        log_event("lease_acquired_owner_started");
        browser_connection_.published(clock_());
        publish_current_snapshot(*controller);
        while (true) {
            std::size_t processed = 0;
            while (processed < settings_.command_batch_size) {
                {
                    std::lock_guard lock(state_mutex_);
                    if (stopping_) { reason = shutdown_reason_; break; }
                }
                auto work = commands_.try_pop();
                if (!work) break;
                if (auto* notification = std::get_if<OwnerNotification>(&*work)) {
                    apply_notification(std::move(*notification));
                    continue;
                }
                execute(*controller, std::move(std::get<OwnerCommand>(*work)));
                ++processed;
            }
            {
                std::lock_guard lock(state_mutex_);
                if (stopping_) { reason = shutdown_reason_; break; }
            }
            ControllerEventBatch events = controller->receive(settings_.event_batch_size);
            const bool presentation_changed = apply_notice(events.update.notice);
            publish_update(
                *controller, std::move(events.update.state), presentation_changed);
            if (events.update.session_ended) {
                (void)mark_stopping(ShutdownReason::browser_disconnected);
            }
            {
                std::lock_guard lock(state_mutex_);
                if (stopping_) { reason = shutdown_reason_; break; }
            }
            const auto deadline = browser_connection_.deadline(
                controller->is_generating(), settings_.idle_grace,
                settings_.orphan_limit);
            if (deadline && clock_() >= *deadline) {
                log_event("disconnect_deadline_expired");
                reason = mark_stopping(ShutdownReason::browser_disconnected);
                break;
            }
            if (processed == settings_.command_batch_size || events.full) continue;
            (void)notifier_->wait_until(
                deadline.value_or(std::chrono::steady_clock::time_point::max()));
        }
    } catch (const std::bad_alloc&) {
        std::terminate();
    } catch (...) {
        fatal = true;
        reason = mark_stopping(ShutdownReason::session_failed);
        log_fatal_once();
    }
    teardown(
        controller,
        reason,
        fatal || reason == ShutdownReason::server_stopping,
        mark_finished_on_teardown);
}

void WebSessionRuntime::execute(WebSessionController& controller, OwnerCommand command) {
    if (std::holds_alternative<SnapshotCommand>(command.command)) {
        (void)command.completion->complete(make_snapshot(controller));
        return;
    }
    if (std::holds_alternative<SseConnectCommand>(command.command)) {
        if (!sse_mailbox_) {
            (void)command.completion->complete(ErrorCode::internal_error);
        } else {
            const auto connection_id = browser_connection_.accept();
            if (!connection_id) {
                log_event("sse_conflict");
                (void)command.completion->complete(
                    ErrorCode::browser_stream_in_use);
                return;
            }
            // The snapshot sent on connect establishes the mailbox's append
            // base and resets its sequence accounting, so a later append is
            // always relative to what this browser actually received.
            const SseMailbox::Stream stream =
                sse_mailbox_->begin_stream({make_snapshot(controller)});
            if (!command.completion->complete(SseConnectResult{
                    sse_mailbox_, stream, *connection_id})) {
                // Mutations retain their unknown outcome after a timeout, but
                // an unclaimed connect must not retain its exclusive slot.
                sse_mailbox_->end_stream(stream);
                (void)browser_connection_.close(*connection_id, clock_());
            } else {
                log_event(has_connected_sse_
                    ? "sse_reconnected"
                    : "sse_connected");
                has_connected_sse_ = true;
            }
        }
        return;
    }
    CommandResult outcome = std::visit([&controller](auto&& value) -> CommandResult {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, RawCommand>) {
            return controller.handle_raw_input(
                value.persona, std::move(value.text));
        } else if constexpr (std::is_same_v<T, StopCommand>) {
            return {.session = controller.request_stop()};
        }
        else if constexpr (std::is_same_v<T, SetDefaultAgentCommand>) {
            return {.session = controller.set_default_agent_id(value.character_id)};
        } else if constexpr (std::is_same_v<T, SnapshotCommand>) {
            throw std::logic_error("Snapshot command handled before dispatch");
        } else if constexpr (std::is_same_v<T, SseConnectCommand>) {
            throw std::logic_error("SSE connect handled before dispatch");
        } else {
            static_assert(unsupported_web_command<T>);
        }
    }, command.command);
    const bool presentation_changed = apply_notice(outcome.session.notice);
    // The state effect is in-process only; the serialized result carries just
    // clear_input and the notice.
    publish_update(
        controller, std::move(outcome.session.state), presentation_changed);
    const bool close_session =
        outcome.session.session_ended || outcome.close_session;
    (void)command.completion->complete(std::move(outcome));
    if (close_session) {
        (void)mark_stopping(ShutdownReason::browser_disconnected);
    }
}

void WebSessionRuntime::apply_notification(OwnerNotification notification) {
    if (browser_connection_.close(notification.connection_id, clock_())) {
        log_event(
            "sse_disconnected collapsed_payloads="
            + std::to_string(notification.collapsed_payloads));
    }
}

SessionSnapshot WebSessionRuntime::make_snapshot(WebSessionController& controller) {
    // The borrowed view lives only for this expression; to_snapshot() copies
    // everything it needs into the returned owning value.
    return to_snapshot(
        descriptor_, controller.view(), presentation(SessionLifecycle::running));
}

WebPresentationState WebSessionRuntime::presentation(
    SessionLifecycle lifecycle,
    std::optional<ShutdownReason> shutdown_reason) const {
    return {
        .notice = notice_,
        .lifecycle = lifecycle,
        .shutdown_reason = shutdown_reason,
    };
}

bool WebSessionRuntime::apply_notice(const std::optional<std::string>& notice) {
    if (!notice) return false;
    if (notice->empty()) {
        if (!notice_) return false;
        notice_.reset();
        return true;
    }
    if (notice_ && *notice_ == *notice) return false;
    notice_ = *notice;
    return true;
}

ShutdownReason WebSessionRuntime::mark_stopping(ShutdownReason reason) {
    std::lock_guard lock(state_mutex_);
    stopping_ = true;
    shutdown_reason_ = keep_higher_priority_reason(shutdown_reason_, reason);
    return shutdown_reason_;
}

void WebSessionRuntime::publish_update(
    WebSessionController& controller,
    ControllerStateUpdate state,
    bool presentation_changed) {
    if (!sink_) return;
    // Notice lives only in a full snapshot under the current protocol, so a
    // presentation change dominates append delivery.
    if (presentation_changed) {
        publish_current_snapshot(controller);
        return;
    }
    if (!has_state_update(state)) return;
    if (TextAppend* append = text_append(state)) {
        if (sink_->publish_append(std::move(*append))
            == AppendPublishResult::Accepted) {
            return;
        }
    }
    publish_current_snapshot(controller);
}

void WebSessionRuntime::publish_current_snapshot(
    WebSessionController& controller) {
    if (!sink_) return;
    SessionSnapshot current = make_snapshot(controller);
    log_generation_transitions(current);
    sink_->publish(SnapshotEvent{std::move(current)});
}

void WebSessionRuntime::log_generation_transitions(
    const SessionSnapshot& current) {
    const bool was_active = logged_generation_active_;
    const bool is_active = current.generation.active;
    const bool request_changed = was_active && is_active
        && logged_active_request_ != current.generation.request_id;
    if (was_active && (!is_active || request_changed)) {
        log_event("generation_terminal request_id="
            + (logged_active_request_
                ? std::to_string(*logged_active_request_)
                : std::string("none"))
            + " status="
            + std::string(generation_terminal_status(
                current, logged_active_request_)));
    }
    if (is_active && (!was_active || request_changed)) {
        log_event("generation_started request_id="
            + (current.generation.request_id
                ? std::to_string(*current.generation.request_id)
                : std::string("none")));
    }
    logged_generation_active_ = is_active;
    logged_active_request_ = current.generation.request_id;
}

void WebSessionRuntime::publish_final(WebSessionController& controller, ShutdownReason reason) {
    if (!sink_) return;
    SessionSnapshot snapshot = to_snapshot(
        descriptor_, controller.view(),
        presentation(SessionLifecycle::stopping, reason));
    sink_->publish(SnapshotEvent{std::move(snapshot)});
}

void WebSessionRuntime::log_fatal_once() noexcept {
    if (fatal_logged_) return;
    fatal_logged_ = true;
    if (hooks_.log_fatal) {
        (void)run_guarded([this] { hooks_.log_fatal(descriptor_); });
    }
}

void WebSessionRuntime::log_event(std::string_view event) noexcept {
    if (!hooks_.log_event) return;
    (void)run_guarded([this, event] { hooks_.log_event(descriptor_, event); });
}

void WebSessionRuntime::teardown(
    std::unique_ptr<WebSessionController>& controller,
    ShutdownReason reason,
    bool skip_final_drain,
    bool mark_finished_on_teardown) noexcept {
    {
        std::lock_guard lock(state_mutex_);
        if (teardown_started_) return;
        teardown_started_ = true;
        stopping_ = true;
        shutdown_reason_ = keep_higher_priority_reason(shutdown_reason_, reason);
        reason = shutdown_reason_;
    }
    skip_final_drain = skip_final_drain || reason == ShutdownReason::server_stopping;
    if (hooks_.mark_registry_stopping) {
        (void)run_guarded([this] { hooks_.mark_registry_stopping(); });
    }
    log_event("runtime_stopping reason=" + std::string(to_string(reason)));
    if (controller) {
        (void)run_guarded([&] {
            publish_final(*controller, reason);
            if (!skip_final_drain && sink_) {
                (void)sink_->wait_for_written(settings_.sse_drain_deadline);
            }
        });
    }
    // End presentation output immediately after its bounded final drain. Later
    // teardown work must not keep an SSE request alive.
    if (sink_) sink_->close();
    // A queue/completion mutex failure may strand later waiters, but it must
    // not strand the controller, journal, workers, or session lease.
    (void)run_guarded([&] {
        while (auto work = commands_.try_pop()) {
            auto* command = std::get_if<OwnerCommand>(&*work);
            if (!command) continue;
            (void)command->completion->complete(
                reason == ShutdownReason::server_stopping
                    ? ErrorCode::server_stopping
                    : ErrorCode::session_not_live);
        }
    });
    if (controller) {
        if (!run_guarded([&] { controller->shutdown(); })) {
            (void)mark_stopping(ShutdownReason::session_failed);
            log_fatal_once();
        }
        controller.reset();
    }
    if (mark_finished_on_teardown) mark_finished();
}

void WebSessionRuntime::mark_finished() noexcept {
    if (hooks_.mark_finished) {
        (void)run_guarded([this] { hooks_.mark_finished(); });
    }
}

std::unique_ptr<WebSessionController> adapt_session_controller(SessionController& controller) {
    return std::make_unique<SessionControllerAdapter>(controller);
}

std::unique_ptr<WebSessionController> adapt_session_controller(
    std::unique_ptr<SessionController> controller) {
    return std::make_unique<SessionControllerAdapter>(std::move(controller));
}

} // namespace cha::web
