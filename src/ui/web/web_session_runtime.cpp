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
    if (settings.command_batch_size == 0 || settings.event_batch_size == 0) {
        throw std::invalid_argument("Web runtime batch sizes must be positive");
    }
    return settings;
}

bool same_target(const AppendTarget& left, const AppendTarget& right) {
    if (left.index() != right.index()) return false;
    if (const auto* entry = std::get_if<AppendTargetEntry>(&left)) {
        return entry->entry_id == std::get<AppendTargetEntry>(right).entry_id;
    }
    return std::get<AppendTargetReasoning>(left).request_id
        == std::get<AppendTargetReasoning>(right).request_id;
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

bool same_entry_except_text(
    const TranscriptEntry& before,
    const TranscriptEntry& after) {
    return before.id == after.id
        && before.kind == after.kind
        && before.participant_id == after.participant_id
        && before.display_name == after.display_name
        && before.addressed_to == after.addressed_to
        && before.addressed_to_name == after.addressed_to_name
        && before.status == after.status
        && before.request_id == after.request_id;
}

bool same_snapshot_except_transcript(
    const SessionSnapshot& before,
    const SessionSnapshot& after) {
    return before.forum == after.forum
        && before.session_id == after.session_id
        && before.session_label == after.session_label
        && before.personas == after.personas
        && before.default_persona_id == after.default_persona_id
        && before.generation == after.generation
        && before.notice == after.notice
        && before.lifecycle == after.lifecycle
        && before.shutdown_reason == after.shutdown_reason;
}

bool differs_only_by_entry_text(
    const SessionSnapshot& before,
    const SessionSnapshot& after,
    std::size_t changed_index) {
    if (!same_snapshot_except_transcript(before, after)
        || before.transcript.size() != after.transcript.size()) {
        return false;
    }
    for (std::size_t index = 0; index != before.transcript.size(); ++index) {
        if (index == changed_index) {
            if (!same_entry_except_text(
                    before.transcript[index], after.transcript[index])) {
                return false;
            }
        } else if (before.transcript[index] != after.transcript[index]) {
            return false;
        }
    }
    return true;
}

bool differs_only_by_reasoning_text(
    const SessionSnapshot& before,
    const SessionSnapshot& after) {
    return before.forum == after.forum
        && before.session_id == after.session_id
        && before.session_label == after.session_label
        && before.personas == after.personas
        && before.default_persona_id == after.default_persona_id
        && before.transcript == after.transcript
        && before.generation.active == after.generation.active
        && before.generation.request_id == after.generation.request_id
        && before.generation.agent_id == after.generation.agent_id
        && before.generation.agent_name == after.generation.agent_name
        && before.generation.phase == after.generation.phase
        && before.notice == after.notice
        && before.lifecycle == after.lifecycle
        && before.shutdown_reason == after.shutdown_reason;
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

    SessionUpdate handle_raw_input(std::string input) override {
        return cha::handle_text_input(*controller_, std::move(input));
    }
    SessionUpdate request_stop() override { return controller_->request_stop(); }
    SessionUpdate set_default_agent_id(std::string_view id) override {
        return controller_->set_default_agent_by_id(id);
    }
    SessionEventBatch receive(std::size_t max_events) override {
        return controller_->receive_events(max_events);
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
    WebControllerFactory factory,
    WebSettings settings,
    WebSessionMetadata metadata,
    std::shared_ptr<WebSnapshotSink> sink,
    WebRuntimeHooks hooks)
    : settings_(validate_runtime_settings(std::move(settings))),
      commands_(settings_.command_queue_capacity),
      metadata_(std::move(metadata)),
      sink_(std::move(sink)),
      sse_mailbox_(std::dynamic_pointer_cast<SseMailbox>(sink_)),
      hooks_(std::move(hooks)),
      owner_([this, factory = std::move(factory)]() mutable {
          try {
              owner_loop(factory(notifier_));
          } catch (const std::bad_alloc&) {
              std::terminate();
          } catch (...) {
              owner_loop(nullptr);
          }
      }) {}

WebSessionRuntime::WebSessionRuntime(
    WebSettings settings,
    WebSessionMetadata metadata,
    std::shared_ptr<WebSnapshotSink> sink,
    WebRuntimeHooks hooks)
    : settings_(validate_runtime_settings(std::move(settings))),
      commands_(settings_.command_queue_capacity),
      metadata_(std::move(metadata)),
      sink_(std::move(sink)),
      sse_mailbox_(std::dynamic_pointer_cast<SseMailbox>(sink_)),
      hooks_(std::move(hooks)) {}

WebSessionRuntime::~WebSessionRuntime() {
    request_shutdown();
    if (owner_.joinable()) {
        owner_.join();
    }
}

CommandSubmitResult WebSessionRuntime::submit(
    WebCommand command,
    std::chrono::milliseconds deadline) {
    auto completion = std::make_shared<CommandCompletion>();
    bool wake_owner = false;
    {
        std::lock_guard lock(state_mutex_);
        if (stopping_) return ErrorCode::session_not_live;
        const CommandEnqueueResult enqueued = commands_.try_push({std::move(command), completion});
        if (!enqueued.accepted) return ErrorCode::command_queue_full;
        wake_owner = enqueued.wake_owner;
    }
    if (wake_owner) notifier_.wake();
    if (auto result = completion->wait_for(deadline)) return std::move(*result);
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
        publish_change(*controller);
        while (true) {
            std::size_t processed = 0;
            while (processed < settings_.command_batch_size) {
                {
                    std::lock_guard lock(state_mutex_);
                    if (stopping_) { reason = shutdown_reason_; break; }
                }
                auto command = commands_.try_pop();
                if (!command) break;
                execute(*controller, std::move(*command));
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
            if (processed == settings_.command_batch_size || events.full) continue;
            (void)notifier_.wait_until(std::chrono::steady_clock::time_point::max());
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
        command.completion->complete(make_snapshot(controller));
        return;
    }
    if (std::holds_alternative<SseConnectCommand>(command.command)) {
        if (!sse_mailbox_) {
            command.completion->complete(ErrorCode::internal_error);
        } else {
            // Establish the exact snapshot sent on connect as the runtime's
            // append base. This keeps later candidates relative to what the
            // browser actually received even if a controller changed without
            // first emitting a render hint.
            publish_snapshot(make_snapshot(controller));
            const SseMailbox::Stream stream =
                sse_mailbox_->begin_stream({*last_snapshot_});
            command.completion->complete(SseConnectResult{sse_mailbox_, stream});
        }
        return;
    }
    SessionUpdate update = std::visit([&controller](auto&& value) -> SessionUpdate {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, RawCommand>) return controller.handle_raw_input(std::move(value.text));
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
    command.completion->complete(CommandResult{.clear_input = update.clear_input, .notice = update.notice});
    if (update.end_session) {
        // See the receive-path note above. This is a defensive fallback, not
        // evidence that the browser initiated the controller's terminal event.
        (void)mark_stopping(ShutdownReason::browser_disconnected);
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
    if (last_snapshot_) {
        if (current == *last_snapshot_) return;
        for (std::size_t i = 0; i < current.transcript.size() && i < last_snapshot_->transcript.size(); ++i) {
            const TranscriptEntry& before = last_snapshot_->transcript[i];
            const TranscriptEntry& after = current.transcript[i];
            if (after.status == TranscriptStatus::streaming
                && after.text != before.text
                && after.text.starts_with(before.text)
                && differs_only_by_entry_text(*last_snapshot_, current, i)) {
                const AppendTarget target = AppendTargetEntry{after.id};
                if (append_target_
                    && same_target(*append_target_, target)) {
                    const std::string text = after.text.substr(before.text.size());
                    last_snapshot_ = std::move(current);
                    sink_->publish_append({target, text}, *last_snapshot_);
                    return;
                }
            }
        }
        if (current.generation.active && current.generation.request_id
            && current.generation.reasoning_text
                != last_snapshot_->generation.reasoning_text
            && current.generation.reasoning_text.starts_with(
                last_snapshot_->generation.reasoning_text)
            && differs_only_by_reasoning_text(*last_snapshot_, current)) {
            const AppendTarget target =
                AppendTargetReasoning{*current.generation.request_id};
            if (append_target_
                && same_target(*append_target_, target)) {
                const std::string text = current.generation.reasoning_text.substr(
                    last_snapshot_->generation.reasoning_text.size());
                last_snapshot_ = std::move(current);
                sink_->publish_append({target, text}, *last_snapshot_);
                return;
            }
        }
    }
    publish_snapshot(std::move(current));
}

bool WebSessionRuntime::publish_append(WebAppendCandidate candidate) {
    if (!sink_ || !last_snapshot_ || !append_target_
        || candidate.text.empty()
        || !same_target(*append_target_, candidate.target)) {
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
    append_target_ = snapshot_append_target(*last_snapshot_);
    append_entry_index_.reset();
    if (append_target_) {
        if (const auto* target =
                std::get_if<AppendTargetEntry>(&*append_target_)) {
            for (std::size_t index = 0;
                 index != last_snapshot_->transcript.size();
                 ++index) {
                if (last_snapshot_->transcript[index].id
                    == target->entry_id) {
                    append_entry_index_ = index;
                    break;
                }
            }
        }
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
        while (auto command = commands_.try_pop()) {
            command->completion->complete(
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
    if (hooks_.mark_finished) {
        (void)run_guarded([this] { hooks_.mark_finished(); });
    }
}

std::unique_ptr<WebSessionController> adapt_session_controller(std::unique_ptr<SessionController> controller) {
    return std::make_unique<SessionControllerAdapter>(std::move(controller));
}

} // namespace cha::web
