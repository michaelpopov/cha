#include "runtime/live_session.h"

#include "chat/transcript.h"
#include "session/session_controller.h"
#include "util/logging.h"
#include "runtime/text_input.h"

#include <exception>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>

namespace cha {
namespace {

template<typename>
inline constexpr bool unsupported_web_command = false;

std::string session_log(const FullSessionId& key, std::string_view event) {
    return "web session forum_id=" + key.forum_id + " session_id=" + key.session_id
        + " event=" + std::string(event);
}

int shutdown_reason_priority(ShutdownReason reason) {
    switch (reason) {
    case ShutdownReason::session_closed: return 0;
    case ShutdownReason::retired: return 0;
    case ShutdownReason::reloading: return 1;
    case ShutdownReason::session_failed: return 2;
    case ShutdownReason::session_deleted: return 3;
    case ShutdownReason::server_stopping: return 4;
    }
    return 0;
}

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
    const ControllerView& snapshot,
    const std::optional<std::uint64_t>& request_id) {
    if (!request_id) return "unknown";
    bool has_prompt = false;
    for (auto entry = snapshot.transcript.entries.rbegin();
         entry != snapshot.transcript.entries.rend(); ++entry) {
        if (entry->request_id != request_id) continue;
        if (entry->kind == EntryKind::human) {
            has_prompt = true;
        } else if ((entry->kind == EntryKind::character
                       || entry->kind == EntryKind::error)
            && entry->status != EntryStatus::streaming) {
            return to_string(entry->status);
        }
    }
    if (has_prompt) return to_string(EntryStatus::cancelled);
    return "unknown";
}

static_assert(std::variant_size_v<WebCommand> == 10);

} // namespace

RuntimeSettings validate_live_session_settings(
    RuntimeSettings settings) {
    if (settings.command_queue_capacity == 0) {
        throw std::invalid_argument(
            "Session runtime command queue capacity must be positive");
    }
    if (settings.command_batch_size == 0 || settings.event_batch_size == 0) {
        throw std::invalid_argument("Session runtime batch sizes must be positive");
    }
    return settings;
}

LiveSession::LiveSession(
    FullSessionId identity,
    std::uint64_t instance,
    std::weak_ptr<SessionRuntime> runtime,
    std::size_t pending_append_byte_limit)
    : identity_(std::move(identity)),
      instance_(instance),
      runtime_(std::move(runtime)),
      output_(std::make_shared<app::SessionOutput>(
          pending_append_byte_limit)) {}

LiveSession::~LiveSession() = default;

std::variant<std::shared_ptr<CommandReply>, ErrorCode> LiveSession::enqueue(
    WebCommand command) {
    if (state_.load() != LiveSessionState::running || stopping_.load()) {
        return shutdown_reason() == ShutdownReason::server_stopping
            ? ErrorCode::server_stopping
            : ErrorCode::session_not_live;
    }
    const std::uint64_t ticket = std::holds_alternative<SubscribeCommand>(command)
        ? subscribe_ticket_.fetch_add(1) + 1
        : 0;
    const auto runtime = runtime_.lock();
    if (!runtime) return ErrorCode::session_not_live;
    return runtime->enqueue(identity_, instance_, std::move(command), ticket);
}

CommandSubmitResult LiveSession::submit(
    WebCommand command,
    std::chrono::milliseconds deadline) {
    auto outcome = enqueue(std::move(command));
    if (const auto* error = std::get_if<ErrorCode>(&outcome)) return *error;
    auto reply = std::get<std::shared_ptr<CommandReply>>(std::move(outcome));
    if (auto result = reply->wait_for(deadline)) return std::move(*result);
    log_event("command_deadline_expired");
    return ErrorCode::command_timeout;
}

CommandSubmitResult LiveSession::snapshot(std::chrono::milliseconds deadline) {
    return submit(SnapshotCommand{}, deadline);
}

CommandSubmitResult LiveSession::subscribe(
    SubscribeCommand command,
    std::chrono::milliseconds deadline) {
    return submit(std::move(command), deadline);
}

CommandSubmitResult LiveSession::unsubscribe(
    UnsubscribeCommand command,
    std::chrono::milliseconds deadline) {
    return submit(std::move(command), deadline);
}

void LiveSession::raise_shutdown_reason(ShutdownReason reason) noexcept {
    ShutdownState current = shutdown_.load();
    while (!current.finalized
        && shutdown_reason_priority(reason) > shutdown_reason_priority(current.reason)
        && !shutdown_.compare_exchange_weak(current, {reason, false})) {}
}

void LiveSession::request_shutdown(ShutdownReason reason) {
    raise_shutdown_reason(reason);
    stopping_.store(true);
    LiveSessionState expected = LiveSessionState::running;
    (void)state_.compare_exchange_strong(expected, LiveSessionState::stopping);
    if (const auto runtime = runtime_.lock()) runtime->wake();
}

void LiveSession::request_retire_when_idle() {
    retire_when_idle_.store(true);
    if (const auto runtime = runtime_.lock()) runtime->wake();
}

void LiveSession::cancel_retirement() {
    retire_when_idle_.store(false);
}

bool LiveSession::idle_for_retirement() const {
    return state_.load() == LiveSessionState::running
        && !stopping_.load() && !generating_.load();
}

std::shared_ptr<const app::SessionOutputItem> LiveSession::take_output() {
    auto item = output_->take();
    if (output_->snapshot_needed()) {
        if (const auto runtime = runtime_.lock()) runtime->wake();
    }
    return item;
}

void LiveSession::acknowledge_output() noexcept {
    output_->acknowledge();
    if (const auto runtime = runtime_.lock()) runtime->wake();
}

void LiveSession::refresh_presentation() {
    output_->require_snapshot();
    if (const auto runtime = runtime_.lock()) runtime->wake();
}

void LiveSession::install(OpenedSession opened) {
    if (!opened.controller) {
        throw std::runtime_error("Session opener returned no controller");
    }
    label_ = std::move(opened.label);
    controller_ = std::move(opened.controller);
    persist_default_character_ = std::move(opened.persist_default_character);
    mirror_ = std::move(opened.mirror);
    cached_audio_entries_ = std::move(opened.cached_audio_entries);
    if (mirror_) {
        mirrored_revision_ = controller_->view().transcript.revision;
        mirrored_label_ = label_;
    }
    (void)apply_notice(opened.notice);
}

void LiveSession::set_running() {
    state_.store(LiveSessionState::running);
    log_event("lease_acquired_runtime_started");
    publish_current_snapshot();
}

void LiveSession::execute(OwnerCommand command) {
    if (std::holds_alternative<SnapshotCommand>(command.command)) {
        (void)command.reply->complete(make_snapshot());
        return;
    }
    if (auto* subscribe = std::get_if<SubscribeCommand>(&command.command)) {
        if (command.subscribe_ticket != subscribe_ticket_.load()) {
            (void)command.reply->complete(ErrorCode::operation_cancelled);
            return;
        }
        output_->attach();
        publish_current_snapshot();
        active_subscription_ = *subscribe;
        (void)command.reply->complete(SubscribeResult{
            subscribe->connection_id,
            subscribe->context_epoch,
            subscribe->subscription_id,
            shared_from_this()});
        return;
    }
    if (auto* unsubscribe = std::get_if<UnsubscribeCommand>(&command.command)) {
        const bool matches = active_subscription_
            && active_subscription_->connection_id == unsubscribe->connection_id
            && active_subscription_->context_epoch == unsubscribe->context_epoch
            && active_subscription_->subscription_id == unsubscribe->subscription_id;
        if (matches) {
            active_subscription_.reset();
            output_->detach();
        }
        (void)command.reply->complete(CommandResult{});
        return;
    }
    if (auto* rename = std::get_if<RenameSessionCommand>(&command.command)) {
        controller_->rename(rename->label);
        label_ = std::move(rename->label);
        publish_current_snapshot();
        mirror_if_changed();
        (void)command.reply->complete(SessionLabelResult{
            identity_.session_id, label_});
        return;
    }

    SessionController& controller = *controller_;
    CommandResult outcome = std::visit([&controller](auto&& value) -> CommandResult {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, RawCommand>) {
            return handle_text_input(
                controller, controller.view().default_persona_id, std::move(value.text));
        } else if constexpr (std::is_same_v<T, StopCommand>) {
            return {.session = controller.request_stop()};
        } else if constexpr (std::is_same_v<T, CoverCommand>) {
            return {.session = controller.cover_conversation(value.through_entry_id)};
        } else if constexpr (std::is_same_v<T, UncoverCommand>) {
            return {.session = controller.uncover_conversation()};
        } else if constexpr (std::is_same_v<T, DeleteTurnCommand>) {
            return {.session = controller.delete_turn(value.response_entry_id)};
        } else if constexpr (std::is_same_v<T, SetDefaultCharacterCommand>) {
            CommandResult result{
                .session = controller.set_default_character_by_id(value.character_id)};
            if (requires_snapshot(result.session)
                && controller.view().default_character_id != null_agent_handle) {
                result.persist_default_character_id =
                    std::string(controller.view().default_character_id);
            }
            return result;
        } else if constexpr (std::is_same_v<T, RenameSessionCommand>
            || std::is_same_v<T, SnapshotCommand>
            || std::is_same_v<T, SubscribeCommand>
            || std::is_same_v<T, UnsubscribeCommand>) {
            throw std::logic_error("Command handled before dispatch");
        } else {
            static_assert(unsupported_web_command<T>);
        }
    }, command.command);

    if (outcome.persist_default_character_id && persist_default_character_) {
        try {
            persist_default_character_(*outcome.persist_default_character_id);
        } catch (const std::bad_alloc&) {
            throw;
        } catch (const std::exception& error) {
            log_warn(session_log(
                identity_, "default_character_not_saved " + std::string(error.what())));
            outcome.session.notice =
                outcome.session.notice.value_or(std::string()) + " (not saved)";
            outcome.persist_default_character_id.reset();
        }
    }
    const bool presentation_changed = apply_notice(outcome.session.notice);
    publish_update(std::move(outcome.session.state), presentation_changed);
    mirror_if_changed();
    const bool session_ended = outcome.session.session_ended;
    (void)command.reply->complete(std::move(outcome));
    if (session_ended) request_shutdown(ShutdownReason::session_closed);
}

bool LiveSession::receive_events(std::size_t batch_size) {
    ControllerEventBatch events = controller_->receive_events(batch_size);
    generating_.store(controller_->is_generating());
    const bool presentation_changed = apply_notice(events.update.notice);
    publish_update(std::move(events.update.state), presentation_changed);
    if (output_->snapshot_needed()) output_->publish_snapshot(make_snapshot());
    mirror_if_changed();
    if (events.update.session_ended) request_shutdown(ShutdownReason::session_closed);
    return events.full;
}

bool LiveSession::retirement_requested() const noexcept {
    return retire_when_idle_.load() && !generating_.load();
}

bool LiveSession::shutdown_requested() const noexcept {
    return stopping_.load();
}

ShutdownReason LiveSession::shutdown_reason() const noexcept {
    return shutdown_.load().reason;
}

void LiveSession::fail_current(std::shared_ptr<CommandReply> reply) {
    if (reply) (void)reply->complete(ErrorCode::internal_error);
    raise_shutdown_reason(ShutdownReason::session_failed);
    stopping_.store(true);
    state_.store(LiveSessionState::stopping);
    log_fatal_once();
}

SessionSnapshot LiveSession::make_snapshot() {
    SessionSnapshot snapshot = to_snapshot(
        *controller_->workspace(),
        identity_,
        label_,
        controller_->view(),
        presentation(SessionLifecycle::running));
    if (cached_audio_entries_) {
        try {
            snapshot.cached_audio_entries = cached_audio_entries_();
        } catch (const std::exception& error) {
            log_warn("Could not read audio cache status: " + std::string(error.what()));
        }
    }
    return snapshot;
}

WebPresentationState LiveSession::presentation(
    SessionLifecycle lifecycle,
    std::optional<ShutdownReason> shutdown_reason) const {
    return {
        .notice = notice_,
        .lifecycle = lifecycle,
        .shutdown_reason = shutdown_reason,
    };
}

bool LiveSession::apply_notice(const std::optional<std::string>& notice) {
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

void LiveSession::publish_update(
    ControllerStateUpdate state,
    bool presentation_changed) {
    if (presentation_changed) {
        publish_current_snapshot();
        return;
    }
    if (!has_state_update(state)) return;
    if (TextAppend* append = text_append(state)) {
        if (output_->publish_append(std::move(*append))
            == AppendPublishResult::Accepted) {
            return;
        }
    }
    publish_current_snapshot();
}

void LiveSession::publish_current_snapshot() {
    log_generation_transitions(controller_->view());
    output_->require_snapshot();
    if (output_->snapshot_needed()) output_->publish_snapshot(make_snapshot());
}

void LiveSession::mirror_if_changed() {
    if (!mirror_) return;
    const TranscriptView transcript = controller_->view().transcript;
    const bool label_changed = label_ != mirrored_label_;
    if (!label_changed && controller_->is_generating()) return;
    if (!label_changed && transcript.revision == mirrored_revision_) return;
    mirror_(label_, transcript.entries);
    mirrored_revision_ = transcript.revision;
    mirrored_label_ = label_;
}

void LiveSession::log_generation_transitions(const ControllerView& current) {
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
    generating_.store(is_active);
}

void LiveSession::log_fatal_once() noexcept {
    if (fatal_logged_) return;
    fatal_logged_ = true;
    (void)run_guarded([this] {
        log_error(session_log(identity_, "fatal_contained"));
    });
}

void LiveSession::log_event(std::string_view event) const noexcept {
    (void)run_guarded([this, event] { log_info(session_log(identity_, event)); });
}

void LiveSession::finalize(ShutdownReason reason) noexcept {
    raise_shutdown_reason(reason);
    stopping_.store(true);
    state_.store(LiveSessionState::stopping);
    std::optional<SessionSnapshot> terminal;
    if (controller_ && output_->attached()) {
        if (!run_guarded([&] { terminal = make_snapshot(); })) {
            log_event("terminal_snapshot_failed");
        }
    }
    // Snapshot construction can block. Accept stronger reasons until the
    // owning payload is ready, then freeze its reason before publication.
    ShutdownState shutdown = shutdown_.load();
    while (!shutdown_.compare_exchange_weak(shutdown, {shutdown.reason, true})) {}
    reason = shutdown.reason;
    log_event("runtime_stopping reason=" + std::string(to_string(reason)));
    if (terminal) {
        terminal->lifecycle = SessionLifecycle::stopping;
        terminal->shutdown_reason = reason;
        (void)run_guarded([&] { output_->publish_snapshot(std::move(*terminal)); });
    }
    output_->close();
    if (controller_) {
        if (!run_guarded([&] { controller_->shutdown(); })) {
            raise_shutdown_reason(ShutdownReason::session_failed);
            log_fatal_once();
        }
        controller_.reset();
        persist_default_character_ = {};
        mirror_ = {};
        log_event("controller_released_runtime_finished");
    }
    state_.store(LiveSessionState::finished);
}

} // namespace cha
