#include "web/live_session.h"

#include "session/not_found_error.h"
#include "session/session_controller.h"
#include "chat/transcript.h"
#include "web/text_input.h"
#include "util/logging.h"

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

std::string session_log(const FullSessionId& key, std::string_view event) {
    return "web session forum_id=" + key.forum_id + " session_id=" + key.session_id
        + " event=" + std::string(event);
}

int shutdown_reason_priority(ShutdownReason reason) {
    switch (reason) {
    case ShutdownReason::browser_disconnected: return 0;
    case ShutdownReason::retired: return 0;
    case ShutdownReason::reloading: return 1;
    case ShutdownReason::session_failed: return 2;
    case ShutdownReason::session_deleted: return 3;
    case ShutdownReason::server_stopping: return 4;
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
    // A cancelled response may end before producing answer text, in which
    // case the completed prompt is the request's only transcript entry.
    if (has_prompt) return to_string(EntryStatus::cancelled);
    // A production terminal transition has a transcript entry for its request.
    // Keep an explicit diagnostic value for malformed controller snapshots
    // instead of reporting a made-up successful outcome.
    return "unknown";
}

static_assert(std::variant_size_v<WebCommand> == 10);

} // namespace

WebSettings validate_live_session_settings(WebSettings settings) {
    if (settings.command_queue_capacity == 0) {
        throw std::invalid_argument(
            "Live session command queue capacity must be positive");
    }
    if (settings.command_batch_size == 0 || settings.event_batch_size == 0) {
        throw std::invalid_argument("Live session batch sizes must be positive");
    }
    return settings;
}

LiveSession::LiveSession(
    WebSettings settings,
    FullSessionId identity,
    SessionOpener opener,
    LiveSessionClock clock)
    : identity_(std::move(identity)),
      settings_(validate_live_session_settings(std::move(settings))),
      opener_(std::move(opener)),
      clock_(clock ? std::move(clock) : [] {
          return std::chrono::steady_clock::now();
      }),
      notifier_(std::make_shared<OwnerWakeSignal>()),
      output_(std::make_shared<cha::app::SessionOutput>(
          settings_.pending_append_byte_limit)),
      commands_(settings_.command_queue_capacity) {
    if (!opener_) throw std::invalid_argument("Live session needs a session opener");
}

LiveSession::~LiveSession() {
    // The manager joins the owner before releasing its last reference, and
    // grace expiry takes the no-destructor exit path instead of reaching here,
    // so destruction never has to start a new blocking join.
}

std::variant<std::shared_ptr<CommandReply>, ErrorCode> LiveSession::enqueue(
    WebCommand command) {
    auto reply = std::make_shared<CommandReply>();
    bool wake_owner = false;
    std::optional<ErrorCode> rejection;
    {
        std::lock_guard lock(lifecycle_mutex_);
        std::uint64_t subscribe_ticket = 0;
        if (std::holds_alternative<SubscribeCommand>(command)) {
            subscribe_ticket = ++subscribe_ticket_;
        }
        if (stopping_) {
            rejection = shutdown_reason_ == ShutdownReason::server_stopping
                ? ErrorCode::server_stopping
                : ErrorCode::session_not_live;
        } else {
            const CommandEnqueueResult enqueued =
                commands_.try_push({std::move(command), reply, subscribe_ticket});
            if (!enqueued.accepted) {
                rejection = ErrorCode::command_queue_full;
            } else {
                wake_owner = enqueued.wake_owner;
            }
        }
    }
    if (rejection) return *rejection;
    if (wake_owner) notifier_->wake();
    return reply;
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

void LiveSession::request_retire_when_idle() {
    {
        std::lock_guard lock(lifecycle_mutex_);
        if (state_ != LiveSessionState::running || stopping_) return;
        retire_when_idle_ = true;
    }
    notifier_->wake();
}

void LiveSession::cancel_retirement() {
    std::lock_guard lock(lifecycle_mutex_);
    retire_when_idle_ = false;
}

bool LiveSession::idle_for_retirement() {
    std::lock_guard lock(lifecycle_mutex_);
    return state_ == LiveSessionState::running && !stopping_ && !generating_;
}

std::shared_ptr<const cha::app::SessionOutputItem> LiveSession::take_output() {
    auto item = output_->take();
    if (output_->snapshot_needed()) notifier_->wake();
    return item;
}

void LiveSession::acknowledge_output() noexcept {
    output_->acknowledge();
    notifier_->wake();
}

void LiveSession::request_shutdown(ShutdownReason reason) {
    {
        std::lock_guard lock(lifecycle_mutex_);
        stopping_ = true;
        shutdown_reason_ = keep_higher_priority_reason(shutdown_reason_, reason);
    }
    notifier_->wake();
}

LiveSessionState LiveSession::lifecycle() {
    std::lock_guard lock(lifecycle_mutex_);
    return state_;
}

void LiveSession::start_owner() {
    // The thread captures raw `this`. Its lifetime guarantee is the manager's
    // map entry, which is inserted before this call and erased only after the
    // owner has published Finished.
    owner_ = std::thread([this] { owner_main(); });
}

void LiveSession::resolve_unstarted(LiveSessionStartResult result) {
    {
        std::lock_guard lock(lifecycle_mutex_);
        if (!start_result_) start_result_ = result;
        stopping_ = true;
        teardown_started_ = true;
        state_ = LiveSessionState::finished;
    }
    lifecycle_changed_.notify_all();
}

std::optional<LiveSessionStartResult> LiveSession::wait_for_start(
    std::chrono::milliseconds deadline) {
    std::unique_lock lock(lifecycle_mutex_);
    lifecycle_changed_.wait_for(lock, deadline, [this] {
        return start_result_.has_value() || start_waiters_woken_;
    });
    return start_result_;
}

void LiveSession::wake_start_waiters() {
    {
        std::lock_guard lock(lifecycle_mutex_);
        start_waiters_woken_ = true;
    }
    lifecycle_changed_.notify_all();
}

bool LiveSession::wait_until_finished(
    std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock(lifecycle_mutex_);
    return lifecycle_changed_.wait_until(lock, deadline, [this] {
        return state_ == LiveSessionState::finished;
    });
}

bool LiveSession::owner_pending() const {
    std::lock_guard lock(lifecycle_mutex_);
    return state_ != LiveSessionState::finished;
}

void LiveSession::join_finished() noexcept {
    // Joining is bounded by invariant: Finished is published after every
    // blocking teardown step, leaving only non-blocking stack unwinding. A
    // wedged owner never publishes it and is never joined here.
    {
        std::lock_guard lock(lifecycle_mutex_);
        if (state_ != LiveSessionState::finished) return;
        if (joined_ || !owner_.joinable()) return;
        joined_ = true;
    }
    owner_.join();
}

void LiveSession::join_owner() noexcept {
    {
        std::lock_guard lock(lifecycle_mutex_);
        if (joined_ || !owner_.joinable()) return;
        joined_ = true;
    }
    owner_.join();
}

void LiveSession::owner_main() {
    if (!open_controller()) return;
    if (commit_running()) log_info(session_log(identity_, "registry_running"));
    owner_loop();
}

bool LiveSession::open_controller() {
    LiveSessionStartResult failure = LiveSessionStartResult::failed;
    try {
        OpenedSession opened = opener_(identity_, notifier_);
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
        return true;
    } catch (const std::bad_alloc&) {
        std::terminate();
    } catch (const SessionNotFoundError&) {
        log_warn(session_log(identity_, "storage_not_found"));
        failure = LiveSessionStartResult::not_found;
    } catch (const ForumNotFoundError&) {
        log_warn(session_log(identity_, "storage_not_found"));
        failure = LiveSessionStartResult::not_found;
    } catch (...) {
        log_error(session_log(identity_, "startup_failed"));
        failure = LiveSessionStartResult::failed;
    }
    // Opening either installed a controller or released everything it had
    // acquired, so a failed start has nothing left to tear down.
    resolve_unstarted(failure);
    return false;
}

bool LiveSession::commit_running() {
    bool running = false;
    {
        std::lock_guard lock(lifecycle_mutex_);
        // A shutdown request that arrived while the opener was running wins
        // the commit race: Running is never published, and teardown proceeds.
        if (stopping_) {
            state_ = LiveSessionState::stopping;
            if (!start_result_) {
                start_result_ = LiveSessionStartResult::shutting_down;
            }
        } else {
            state_ = LiveSessionState::running;
            if (!start_result_) start_result_ = LiveSessionStartResult::ready;
            running = true;
        }
    }
    lifecycle_changed_.notify_all();
    return running;
}

void LiveSession::publish_finished() noexcept {
    {
        std::lock_guard lock(lifecycle_mutex_);
        // Startup is always resolved before teardown; completing it here only
        // guarantees that no waiter can be stranded by a future change.
        if (!start_result_) {
            start_result_ = LiveSessionStartResult::shutting_down;
        }
        state_ = LiveSessionState::finished;
    }
    lifecycle_changed_.notify_all();
}

void LiveSession::owner_loop() {
    ShutdownReason reason = ShutdownReason::browser_disconnected;
    bool fatal = false;
    try {
        log_event("lease_acquired_owner_started");
        publish_current_snapshot();
        while (true) {
            std::size_t processed = 0;
            while (processed < settings_.command_batch_size) {
                {
                    std::lock_guard lock(lifecycle_mutex_);
                    if (stopping_) { reason = shutdown_reason_; break; }
                }
                auto work = commands_.try_pop();
                if (!work) break;
                execute(std::move(*work));
                ++processed;
            }
            {
                std::lock_guard lock(lifecycle_mutex_);
                if (stopping_) { reason = shutdown_reason_; break; }
            }
            ControllerEventBatch events =
                controller_->receive_events(settings_.event_batch_size);
            {
                std::lock_guard lock(lifecycle_mutex_);
                generating_ = controller_->is_generating();
            }
            const bool presentation_changed = apply_notice(events.update.notice);
            publish_update(std::move(events.update.state), presentation_changed);
            if (output_->snapshot_needed()) {
                output_->publish_snapshot(make_snapshot());
            }
            mirror_if_changed();
            if (events.update.session_ended) {
                (void)mark_stopping(ShutdownReason::browser_disconnected);
            }
            {
                std::lock_guard lock(lifecycle_mutex_);
                if (stopping_) { reason = shutdown_reason_; break; }
            }
            bool retire = false;
            {
                std::lock_guard lock(lifecycle_mutex_);
                retire = retire_when_idle_ && !generating_;
            }
            if (retire && !controller_->is_generating()) {
                reason = mark_stopping(ShutdownReason::retired);
                break;
            }
            if (processed == settings_.command_batch_size || events.full) continue;
            (void)notifier_->wait_until(
                std::chrono::steady_clock::time_point::max());
        }
    } catch (const std::bad_alloc&) {
        std::terminate();
    } catch (...) {
        fatal = true;
        reason = mark_stopping(ShutdownReason::session_failed);
        log_fatal_once();
    }
    teardown(reason, fatal || reason == ShutdownReason::server_stopping);
}

void LiveSession::execute(OwnerCommand command) {
    if (std::holds_alternative<SnapshotCommand>(command.command)) {
        (void)command.reply->complete(make_snapshot());
        return;
    }
    if (auto* subscribe = std::get_if<SubscribeCommand>(&command.command)) {
        bool stale = false;
        {
            std::lock_guard lock(lifecycle_mutex_);
            stale = command.subscribe_ticket != subscribe_ticket_;
        }
        if (stale) {
            (void)command.reply->complete(ErrorCode::operation_cancelled);
            return;
        }
        output_->attach();
        publish_current_snapshot();
        {
            std::lock_guard lock(lifecycle_mutex_);
            active_subscription_ = *subscribe;
        }
        (void)command.reply->complete(SubscribeResult{
            subscribe->connection_id,
            subscribe->context_epoch,
            subscribe->subscription_id});
        return;
    }
    if (auto* unsubscribe = std::get_if<UnsubscribeCommand>(&command.command)) {
        bool matches = false;
        {
            std::lock_guard lock(lifecycle_mutex_);
            matches = active_subscription_
                && active_subscription_->connection_id == unsubscribe->connection_id
                && active_subscription_->context_epoch == unsubscribe->context_epoch
                && active_subscription_->subscription_id
                    == unsubscribe->subscription_id;
            if (matches) active_subscription_.reset();
        }
        if (matches) output_->detach();
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
        } else if constexpr (std::is_same_v<T, RenameSessionCommand>) {
            throw std::logic_error("Rename command handled before dispatch");
        } else if constexpr (std::is_same_v<T, SnapshotCommand>) {
            throw std::logic_error("Snapshot command handled before dispatch");
        } else if constexpr (std::is_same_v<T, SubscribeCommand>) {
            throw std::logic_error("Subscribe handled before dispatch");
        } else if constexpr (std::is_same_v<T, UnsubscribeCommand>) {
            throw std::logic_error("Unsubscribe handled before dispatch");
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
            // The session keeps the new default; only the saved copy is missing.
            // The reason can name workspace paths, so it goes to the log alone.
            log_warn(session_log(
                identity_, "default_character_not_saved " + std::string(error.what())));
            outcome.session.notice =
                outcome.session.notice.value_or(std::string()) + " (not saved)";
            outcome.persist_default_character_id.reset();
        }
    }
    const bool presentation_changed = apply_notice(outcome.session.notice);
    // The state effect is in-process only; the serialized result carries just
    // clear_input and the notice.
    publish_update(std::move(outcome.session.state), presentation_changed);
    mirror_if_changed();
    const bool session_ended = outcome.session.session_ended;
    (void)command.reply->complete(std::move(outcome));
    if (session_ended) {
        (void)mark_stopping(ShutdownReason::browser_disconnected);
    }
}

SessionSnapshot LiveSession::make_snapshot() {
    // The borrowed view lives only for this expression; to_snapshot() copies
    // everything it needs into the returned owning value.
    SessionSnapshot snapshot = to_snapshot(
        *controller_->workspace(),
        identity_, label_, controller_->view(),
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

ShutdownReason LiveSession::mark_stopping(ShutdownReason reason) {
    std::lock_guard lock(lifecycle_mutex_);
    stopping_ = true;
    shutdown_reason_ = keep_higher_priority_reason(shutdown_reason_, reason);
    return shutdown_reason_;
}

void LiveSession::publish_update(
    ControllerStateUpdate state,
    bool presentation_changed) {
    // Notice lives only in a full snapshot under the current protocol, so a
    // presentation change dominates append delivery.
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
    if (!label_changed && transcript.revision == mirrored_revision_) {
        return;
    }
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
    {
        std::lock_guard lock(lifecycle_mutex_);
        generating_ = is_active;
    }
}

void LiveSession::publish_final(ShutdownReason reason) {
    SessionSnapshot snapshot = make_snapshot();
    snapshot.lifecycle = SessionLifecycle::stopping;
    snapshot.shutdown_reason = reason;
    output_->publish_snapshot(std::move(snapshot));
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

void LiveSession::teardown(ShutdownReason reason, bool skip_final_drain) noexcept {
    {
        std::lock_guard lock(lifecycle_mutex_);
        if (teardown_started_) return;
        teardown_started_ = true;
        stopping_ = true;
        shutdown_reason_ = keep_higher_priority_reason(shutdown_reason_, reason);
        reason = shutdown_reason_;
        state_ = LiveSessionState::stopping;
    }
    lifecycle_changed_.notify_all();
    (void)run_guarded([this] {
        log_info(session_log(identity_, "registry_stopping"));
    });
    (void)skip_final_drain;
    log_event("runtime_stopping reason=" + std::string(to_string(reason)));
    if (controller_) {
        (void)run_guarded([&] { publish_final(reason); });
        ShutdownReason latest_reason;
        {
            std::lock_guard lock(lifecycle_mutex_);
            latest_reason = shutdown_reason_;
        }
        if (latest_reason != reason) {
            reason = latest_reason;
            (void)run_guarded([&] { publish_final(reason); });
        }
    }
    output_->close();
    // A queue/reply mutex failure may strand later waiters, but it must
    // not strand the controller, journal, or workers.
    (void)run_guarded([&] {
        while (auto work = commands_.try_pop()) {
            (void)work->reply->complete(
                reason == ShutdownReason::server_stopping
                    ? ErrorCode::server_stopping
                    : ErrorCode::session_not_live);
        }
    });
    if (controller_) {
        if (!run_guarded([&] { controller_->shutdown(); })) {
            (void)mark_stopping(ShutdownReason::session_failed);
            log_fatal_once();
        }
        // Destroy the controller before publishing Finished so a replacement
        // actor can start immediately afterwards.
        controller_.reset();
        persist_default_character_ = {};
        mirror_ = {};
        log_event("controller_released_owner_finished");
    }
    publish_finished();
}

} // namespace cha::web
