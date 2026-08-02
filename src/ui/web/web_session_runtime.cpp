#include "ui/web/web_session_runtime.h"

#include "ui/web/sse_mailbox.h"

#include "session/session_controller.h"
#include "transcript/transcript.h"
#include "ui/text/text_input.h"

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

TranscriptKind web_kind(EntryKind kind) {
    switch (kind) {
    case EntryKind::human: return TranscriptKind::human;
    case EntryKind::agent: return TranscriptKind::agent;
    case EntryKind::notice: return TranscriptKind::notice;
    case EntryKind::error: return TranscriptKind::error;
    }
    throw std::logic_error("Unsupported transcript entry kind");
}

TranscriptStatus web_status(EntryStatus status) {
    switch (status) {
    case EntryStatus::complete: return TranscriptStatus::complete;
    case EntryStatus::streaming: return TranscriptStatus::streaming;
    case EntryStatus::cancelled: return TranscriptStatus::cancelled;
    case EntryStatus::failed: return TranscriptStatus::failed;
    }
    throw std::logic_error("Unsupported transcript entry status");
}

GenerationPhase web_phase(ResponsePhase phase) {
    switch (phase) {
    case ResponsePhase::waiting: return GenerationPhase::waiting;
    case ResponsePhase::reasoning: return GenerationPhase::reasoning;
    case ResponsePhase::answering: return GenerationPhase::answering;
    case ResponsePhase::stopping: return GenerationPhase::stopping;
    }
    throw std::logic_error("Unsupported generation phase");
}

std::string_view generation_terminal_status(
    const SessionSnapshot& snapshot,
    const std::optional<std::uint64_t>& request_id) {
    if (!request_id) return "unknown";
    bool has_prompt = false;
    for (auto entry = snapshot.transcript.rbegin();
         entry != snapshot.transcript.rend(); ++entry) {
        if (entry->request_id != request_id) continue;
        if (entry->kind == TranscriptKind::human) {
            has_prompt = true;
        } else if ((entry->kind == TranscriptKind::agent
                       || entry->kind == TranscriptKind::error)
            && entry->status != TranscriptStatus::streaming) {
            return to_string(entry->status);
        }
    }
    // A cancelled response may end before producing answer text, in which
    // case the completed prompt is the request's only transcript entry.
    if (has_prompt) return to_string(TranscriptStatus::cancelled);
    // A production terminal transition has a transcript entry for its request.
    // Keep an explicit diagnostic value for malformed controller snapshots
    // instead of reporting a made-up successful outcome.
    return "unknown";
}

bool same_generation_structure(
    const GenerationStatusView& current,
    const GenerationState& before) {
    return current.active == before.active
        && current.request_id == before.request_id
        && current.agent_id == before.agent_id
        && current.agent_name == before.agent_name
        && web_phase(current.phase) == before.phase;
}

bool same_domain_entry_shape(
    const cha::TranscriptEntry& current,
    const TranscriptEntry& before) {
    return current.id == before.id
        && web_kind(current.kind) == before.kind
        && current.participant_id == before.participant_id
        && current.display_name == before.display_name
        && current.addressed_to == before.addressed_to
        && current.addressed_to_name == before.addressed_to_name
        && web_status(current.status) == before.status
        && current.request_id == before.request_id;
}

class SessionControllerAdapter final : public WebSessionController {
public:
    explicit SessionControllerAdapter(std::unique_ptr<SessionController> controller)
        : controller_(std::move(controller)) {}

    SessionUpdate handle_raw_input(
        std::string_view author_id,
        std::string input) override {
        return cha::handle_text_input(*controller_, author_id, std::move(input));
    }
    SessionUpdate request_stop() override { return controller_->request_stop(); }
    SessionUpdate set_default_agent_id(std::string_view id) override {
        return controller_->set_default_agent_by_id(id);
    }
    SessionEventBatch receive(std::size_t max_events) override {
        return controller_->receive_events(max_events);
    }
    [[nodiscard]] bool is_generating() const override {
        return controller_->is_generating();
    }
    SessionSnapshot snapshot() override {
        SessionSnapshot result;
        result.personas.reserve(controller_->personas().all().size());
        for (const PersonaInfo& persona : controller_->personas().all()) {
            result.personas.push_back({persona.id, persona.name});
        }
        result.default_persona_id = controller_->default_agent_id();
        const TranscriptView view = controller_->transcript().view();
        snapshot_revision_ = view.revision;
        result.transcript.reserve(view.entries.size());
        for (const cha::TranscriptEntry& entry : view.entries) {
            result.transcript.push_back({
                .id = entry.id,
                .kind = web_kind(entry.kind),
                .participant_id = entry.participant_id,
                .display_name = entry.display_name,
                .addressed_to = entry.addressed_to,
                .addressed_to_name = entry.addressed_to_name,
                .text = entry.text,
                .status = web_status(entry.status),
                .request_id = entry.request_id,
            });
        }
        const GenerationStatusView status =
            controller_->generation_status_view();
        result.generation = {
            .active = status.active,
            .request_id = status.request_id,
            .agent_id = std::string(status.agent_id),
            .agent_name = std::string(status.agent_name),
            .phase = web_phase(status.phase),
            .reasoning_text = std::string(status.reasoning_text),
        };
        return result;
    }
    std::optional<WebAppendCandidate> append_candidate(
        const SessionSnapshot& before) override {
        if (!snapshot_revision_
            || controller_->default_agent_id()
                != before.default_persona_id) {
            return std::nullopt;
        }

        const TranscriptView transcript = controller_->transcript().view();
        const GenerationStatusView generation =
            controller_->generation_status_view();
        if (transcript.entries.size() != before.transcript.size()
            || !same_generation_structure(generation, before.generation)) {
            return std::nullopt;
        }

        if (transcript.revision == *snapshot_revision_) {
            if (generation.phase != ResponsePhase::reasoning
                || !generation.request_id
                || generation.reasoning_text.size()
                    <= before.generation.reasoning_text.size()) {
                return std::nullopt;
            }
            // A live request's reasoning buffer is append-only. Equal request
            // and phase fields plus an unchanged transcript prove continuity.
            return WebAppendCandidate{
                .target = AppendTargetReasoning{*generation.request_id},
                .text = std::string(generation.reasoning_text.substr(
                    before.generation.reasoning_text.size())),
            };
        }

        if (transcript.revision < *snapshot_revision_
            || generation.phase != ResponsePhase::answering
            || generation.reasoning_text.size()
                != before.generation.reasoning_text.size()
            || !transcript.open_entry_id
            || transcript.entries.empty()
            || before.transcript.empty()) {
            return std::nullopt;
        }
        const cha::TranscriptEntry& current = transcript.entries.back();
        const TranscriptEntry& previous = before.transcript.back();
        if (*transcript.open_entry_id != current.id
            || current.status != EntryStatus::streaming
            || !same_domain_entry_shape(current, previous)
            || current.text.size() <= previous.text.size()) {
            return std::nullopt;
        }
        // Transcript permits only append_answer() to mutate an open entry's
        // text. Stable size/open ID/metadata therefore prove the prefix without
        // rescanning the accumulated answer.
        return WebAppendCandidate{
            .target = AppendTargetEntry{current.id},
            .text = current.text.substr(previous.text.size()),
        };
    }
    void shutdown() override { controller_->shutdown(); }

private:
    std::unique_ptr<SessionController> controller_;
    std::optional<std::size_t> snapshot_revision_;
};

static_assert(std::variant_size_v<WebCommand> == 5);

} // namespace

WebSessionRuntime::WebSessionRuntime(
    WebSettings settings,
    WebSessionMetadata metadata,
    std::shared_ptr<WebSnapshotSink> sink,
    std::shared_ptr<SseMailbox> mailbox,
    WebRuntimeHooks hooks,
    WebRuntimeClock clock)
    : settings_(validate_runtime_settings(std::move(settings))),
      commands_(settings_.command_queue_capacity),
      metadata_(std::move(metadata)),
      sink_(std::move(sink)),
      sse_mailbox_(std::move(mailbox)),
      hooks_(std::move(hooks)),
      clock_(clock ? std::move(clock) : [] {
          return std::chrono::steady_clock::now();
      }) {}

WebSessionRuntime::WebSessionRuntime(
    WebSettings settings,
    WebSessionMetadata metadata,
    std::shared_ptr<SseMailbox> mailbox,
    WebRuntimeHooks hooks,
    WebRuntimeClock clock)
    : WebSessionRuntime(
          std::move(settings),
          std::move(metadata),
          mailbox,
          mailbox,
          std::move(hooks),
          std::move(clock)) {
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
    if (wake_owner) notifier_.wake();
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
        notifier_.wake();
    }
}

void WebSessionRuntime::request_shutdown(ShutdownReason reason) {
    {
        std::lock_guard lock(state_mutex_);
        stopping_ = true;
        shutdown_reason_ = keep_higher_priority_reason(shutdown_reason_, reason);
    }
    notifier_.wake();
}

void WebSessionRuntime::run_with_controller(
    std::unique_ptr<WebSessionController> controller) {
    owner_loop(std::move(controller));
}

void WebSessionRuntime::owner_loop(
    std::unique_ptr<WebSessionController> controller) {
    ShutdownReason reason = ShutdownReason::browser_disconnected;
    bool fatal = false;
    try {
        if (!controller) throw std::runtime_error("Web controller factory returned null");
        log_event("lease_acquired_owner_started");
        browser_connection_.published(clock_());
        publish_change(*controller);
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
            SessionEventBatch events = controller->receive(settings_.event_batch_size);
            const bool notice_changed = events.update.notice.has_value();
            apply_notice(events.update);
            if (events.update.render_needed || notice_changed) {
                publish_change(*controller, notice_changed);
            }
            if (events.update.end_session) {
                // The production controller currently emits end_session only
                // from its shutdown drain, outside this loop. Keep this
                // defensive fallback until a distinct controller-ended reason
                // exists; it must not be read as a real browser signal.
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
            (void)notifier_.wait_until(
                deadline.value_or(std::chrono::steady_clock::time_point::max()));
        }
    } catch (const std::bad_alloc&) {
        std::terminate();
    } catch (...) {
        fatal = true;
        reason = mark_stopping(ShutdownReason::session_failed);
        log_fatal_once();
    }
    teardown(controller, reason, fatal || reason == ShutdownReason::server_stopping);
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
            // Establish the exact snapshot sent on connect as the runtime's
            // append base. This keeps later candidates relative to what the
            // browser actually received even if a controller changed without
            // first emitting a render hint.
            publish_snapshot(make_snapshot(controller));
            const SseMailbox::Stream stream =
                sse_mailbox_->begin_stream({*last_snapshot_});
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
    SessionUpdate update = std::visit([&controller](auto&& value) -> SessionUpdate {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, RawCommand>) return controller.handle_raw_input(value.user, std::move(value.text));
        else if constexpr (std::is_same_v<T, StopCommand>) return controller.request_stop();
        else if constexpr (std::is_same_v<T, SetDefaultAgentCommand>) {
            return controller.set_default_agent_id(value.persona_id);
        } else if constexpr (std::is_same_v<T, SnapshotCommand>) {
            throw std::logic_error("Snapshot command handled before dispatch");
        } else if constexpr (std::is_same_v<T, SseConnectCommand>) {
            throw std::logic_error("SSE connect handled before dispatch");
        } else {
            static_assert(unsupported_web_command<T>);
        }
    }, command.command);
    const bool notice_changed = update.notice.has_value();
    apply_notice(update);
    if (update.render_needed || notice_changed) {
        publish_change(controller, notice_changed);
    }
    (void)command.completion->complete(CommandResult{
        .clear_input = update.clear_input,
        .notice = update.notice,
    });
    if (update.end_session) {
        // See the receive-path note above. This is a defensive fallback, not
        // evidence that the browser initiated the controller's terminal event.
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
    SessionSnapshot current = controller.snapshot();
    current.forum = metadata_.forum;
    current.session_id = metadata_.session_id;
    current.session_label = metadata_.session_label;
    current.notice = notice_;
    current.lifecycle = SessionLifecycle::running;
    current.shutdown_reason.reset();
    return current;
}

void WebSessionRuntime::apply_notice(const SessionUpdate& update) {
    if (!update.notice) return;
    if (update.notice->empty()) notice_.reset();
    else notice_ = *update.notice;
}

ShutdownReason WebSessionRuntime::mark_stopping(ShutdownReason reason) {
    std::lock_guard lock(state_mutex_);
    stopping_ = true;
    shutdown_reason_ = keep_higher_priority_reason(shutdown_reason_, reason);
    return shutdown_reason_;
}

void WebSessionRuntime::publish_change(
    WebSessionController& controller,
    bool force_snapshot) {
    if (!sink_) return;
    if (!force_snapshot && last_snapshot_ && append_target_) {
        if (auto candidate = controller.append_candidate(*last_snapshot_);
            candidate && publish_append(std::move(*candidate))) {
            return;
        }
    }

    SessionSnapshot current = make_snapshot(controller);
    const bool was_active = last_snapshot_
        && last_snapshot_->generation.active;
    const bool is_active = current.generation.active;
    const bool active_request_changed = was_active && is_active
        && last_snapshot_->generation.request_id
            != current.generation.request_id;
    if (was_active && (!is_active || active_request_changed)) {
        log_event("generation_terminal request_id="
            + (last_snapshot_->generation.request_id
                ? std::to_string(*last_snapshot_->generation.request_id)
                : std::string("none"))
            + " status="
            + std::string(generation_terminal_status(
                current, last_snapshot_->generation.request_id)));
    }
    if (is_active && (!was_active || active_request_changed)) {
        log_event("generation_started request_id="
            + (current.generation.request_id
                ? std::to_string(*current.generation.request_id)
                : std::string("none")));
    }
    if (last_snapshot_ && current == *last_snapshot_) return;
    publish_snapshot(std::move(current));
}

bool WebSessionRuntime::publish_append(WebAppendCandidate candidate) {
    if (!sink_ || !last_snapshot_ || !append_target_
        || candidate.text.empty()
        || *append_target_ != candidate.target) {
        return false;
    }
    if (const auto* entry = std::get_if<AppendTargetEntry>(&candidate.target)) {
        if (!append_entry_index_
            || *append_entry_index_ >= last_snapshot_->transcript.size()
            || last_snapshot_->transcript[*append_entry_index_].id
                != entry->entry_id) {
            return false;
        }
        last_snapshot_->transcript[*append_entry_index_].text.append(
            candidate.text);
    } else {
        const auto& reasoning =
            std::get<AppendTargetReasoning>(candidate.target);
        if (last_snapshot_->generation.request_id != reasoning.request_id) {
            return false;
        }
        last_snapshot_->generation.reasoning_text.append(candidate.text);
    }
    sink_->publish_append(std::move(candidate), *last_snapshot_);
    return true;
}

void WebSessionRuntime::publish_snapshot(SessionSnapshot snapshot) {
    sink_->publish(SnapshotEvent{snapshot});
    last_snapshot_ = std::move(snapshot);
    const auto selection = snapshot_append_selection(*last_snapshot_);
    if (selection) {
        append_target_ = selection->target;
        append_entry_index_ = selection->transcript_index;
    } else {
        append_target_.reset();
        append_entry_index_.reset();
    }
}

void WebSessionRuntime::publish_final(WebSessionController& controller, ShutdownReason reason) {
    if (!sink_) return;
    SessionSnapshot snapshot = controller.snapshot();
    snapshot.forum = metadata_.forum;
    snapshot.session_id = metadata_.session_id;
    snapshot.session_label = metadata_.session_label;
    snapshot.notice = notice_;
    snapshot.lifecycle = SessionLifecycle::stopping;
    snapshot.shutdown_reason = reason;
    sink_->publish(SnapshotEvent{std::move(snapshot)});
}

void WebSessionRuntime::log_fatal_once() noexcept {
    if (fatal_logged_) return;
    fatal_logged_ = true;
    if (hooks_.log_fatal) {
        (void)run_guarded([this] { hooks_.log_fatal(metadata_); });
    }
}

void WebSessionRuntime::log_event(std::string_view event) noexcept {
    if (!hooks_.log_event) return;
    (void)run_guarded([this, event] { hooks_.log_event(metadata_, event); });
}

void WebSessionRuntime::teardown(
    std::unique_ptr<WebSessionController>& controller,
    ShutdownReason reason,
    bool skip_final_drain) noexcept {
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
        log_event("lease_released_owner_finished");
    }
    if (hooks_.mark_finished) {
        (void)run_guarded([this] { hooks_.mark_finished(); });
    }
}

std::unique_ptr<WebSessionController> adapt_session_controller(std::unique_ptr<SessionController> controller) {
    return std::make_unique<SessionControllerAdapter>(std::move(controller));
}

} // namespace cha::web
