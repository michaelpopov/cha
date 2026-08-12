#include "session/session_controller.h"

#include "session/session_label.h"
#include "util/logging.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <variant>

namespace cha {
namespace {

void log_persistence_failure(
    const std::string& action,
    std::string_view details) noexcept {
    try {
        log_error(
            "Session persistence failed: " + action + "; reason="
            + std::string(details));
    } catch (...) {
        log_error("Session persistence failed");
    }
}

template<typename Operation>
void persist(std::string action, Operation&& operation) {
    try {
        operation();
    } catch (const std::exception& error) {
        log_persistence_failure(action, error.what());
        throw std::runtime_error(
            "Failed to " + std::move(action) + ": " + error.what());
    }
}

std::string request_action(
    std::string_view action,
    RequestId request_id,
    std::string_view character_display_name) {
    return std::string(action) + " request " + std::to_string(request_id)
        + " for @" + std::string(character_display_name);
}

ForumCharacters make_forum_characters(
    const std::vector<ModelBackendInfo>& runtime_info) {
    std::vector<CharacterMetadata> characters;
    characters.reserve(runtime_info.size());
    for (const ModelBackendInfo& backend : runtime_info) {
        characters.push_back(backend.character);
    }
    // Definitions reached the controller through either the validated
    // workspace boundary or a trusted application built-in factory.
    return ForumCharacters(std::move(characters), true);
}

void require_character_count(std::size_t count) {
    if (count == 0) {
        throw std::invalid_argument(
            "Generation executor requires at least one character");
    }
}

} // namespace

std::unique_ptr<SessionController> SessionController::from_shared_definitions(
    std::vector<CharacterDefinition> definitions,
    SharedPersonaRoster personas,
    ParticipantId initial_default_character_id,
    std::filesystem::path database_path,
    SessionLease lease,
    WakeNotifier& notifier,
    SessionRestore restored) {
    require_character_count(definitions.size());
    if (!personas) throw std::invalid_argument("Session controller requires a persona roster");
    if (!lease.active()) throw std::invalid_argument("Production session controllers require an active session lease");
    return std::unique_ptr<SessionController>(new SessionController(
        std::move(definitions), std::move(personas), std::move(initial_default_character_id),
        std::move(database_path), std::move(lease), notifier, std::move(restored)));
}

std::unique_ptr<SessionController> SessionController::from_definitions_for_testing(
    std::vector<CharacterDefinition> definitions,
    PersonaRoster personas,
    ParticipantId initial_default_character_id,
    std::filesystem::path database_path,
    WakeNotifier& notifier,
    SessionRestore restored) {
    require_character_count(definitions.size());
    return std::unique_ptr<SessionController>(new SessionController(
        std::move(definitions),
        std::make_shared<const PersonaRoster>(std::move(personas)),
        std::move(initial_default_character_id),
        std::move(database_path),
        SessionLease::inactive_for_testing(),
        notifier,
        std::move(restored)));
}

std::unique_ptr<SessionController> SessionController::from_backends_for_testing(
    std::vector<std::unique_ptr<ModelBackend>> backends,
    PersonaRoster personas,
    ParticipantId initial_default_character_id,
    std::filesystem::path database_path,
    WakeNotifier& notifier,
    SessionRestore restored,
    ActivationHook before_activation) {
    require_character_count(backends.size());
    return std::unique_ptr<SessionController>(new SessionController(
        std::move(backends),
        std::move(personas),
        std::move(initial_default_character_id),
        std::move(database_path),
        notifier,
        std::move(restored),
        std::move(before_activation)));
}

SessionController::SessionController(
    std::vector<CharacterDefinition> definitions,
    SharedPersonaRoster personas,
    ParticipantId initial_default_character_id,
    std::filesystem::path path,
    SessionLease lease,
    WakeNotifier& notifier,
    SessionRestore restored)
    : lease_(std::move(lease)),
      journal_(std::move(path)),
      worker_pool_(definitions.size()),
      generation_executor_(std::move(definitions), notifier, worker_pool_),
      characters_(make_forum_characters(generation_executor_.runtime_info())),
      personas_(std::move(personas)),
      default_character_id_(std::move(initial_default_character_id)) {
    initialize(std::move(restored));
}

SessionController::SessionController(
    std::vector<std::unique_ptr<ModelBackend>> backends,
    PersonaRoster personas,
    ParticipantId initial_default_character_id,
    std::filesystem::path path,
    WakeNotifier& notifier,
    SessionRestore restored,
    ActivationHook before_activation)
    : lease_(SessionLease::inactive_for_testing()),
      journal_(std::move(path)),
      worker_pool_(backends.size()),
      generation_executor_(std::move(backends), notifier, worker_pool_),
      characters_(make_forum_characters(generation_executor_.runtime_info())),
      personas_(std::make_shared<const PersonaRoster>(std::move(personas))),
      default_character_id_(std::move(initial_default_character_id)),
      before_activation_(std::move(before_activation)) {
    initialize(std::move(restored));
}

SessionController::~SessionController() {
    try {
        shutdown();
    } catch (...) {
    }
}

void SessionController::initialize(SessionRestore restored) {
    if (!characters_.find(default_character_id_)) {
        throw std::invalid_argument(
            "Initial default character ID is not in the forum roster");
    }
    transcript_.replace_entries(std::move(restored.entries));
    next_request_id_ = restored.next_request_id;
    next_entry_id_ = restored.next_entry_id;
    for (const InterruptedTurn& turn : restored.interrupted_turns) {
        persist(
            request_action(
                "persist interrupted-turn repair for",
                turn.request_id,
                turn.error_entry.participant_id),
            [this, &turn] {
                journal_.fail_turn(turn.request_id, turn.error_entry);
            });
        transcript_.add_entry(turn.error_entry);
    }
}

ControllerGenerationView SessionController::generation_view() const noexcept {
    if (batch_ && batch_->cancellation_requested()) {
        const RunSpec& run = batch_->foreground_run();
        return {
            .active = true,
            .request_id = run.request_id,
            .character_id = run.target.id,
            .character_display_name = run.target.display_name,
            .phase = ResponsePhase::stopping,
        };
    }
    return {
        .active = busy(),
        .request_id = active_ ? std::optional<RequestId>(active_->request_id)
                              : std::nullopt,
        .character_id = active_ ? std::string_view(active_->character_id)
                            : std::string_view{},
        .character_display_name = active_ ? std::string_view(active_->character_display_name)
                              : std::string_view{},
        .phase = active_ ? active_->phase : ResponsePhase::waiting,
        .reasoning_text = active_ ? std::string_view(active_->reasoning_text)
                                  : std::string_view{},
    };
}

ControllerView SessionController::view() const noexcept {
    // Every field borrows live controller storage. The caller consumes it
    // before the next mutation on this thread; nothing here allocates.
    return {
        .characters = characters_.all(),
        .default_character_id = default_character_id_,
        .transcript = transcript_.view(),
        .generation = generation_view(),
    };
}

bool SessionController::is_generating() const noexcept {
    return busy();
}

bool SessionController::busy() const noexcept {
    return active_ || batch_;
}

ControllerUpdate SessionController::busy_notice() const {
    return {.notice = std::string(generation_in_progress_notice)};
}

std::optional<EntryIdentity> SessionController::resolve_author(
    std::string_view author_id,
    ControllerUpdate& update) const {
    const auto author = std::find_if(
        personas_->begin(), personas_->end(),
        [author_id](const Persona& persona) { return persona.id == author_id; });
    if (author == personas_->end()) {
        update.notice = "Unknown persona ID '" + std::string(author_id) + "'";
        return std::nullopt;
    }
    return EntryIdentity{author->id, author->display_name};
}

ControllerUpdate SessionController::submit_prompt(
    std::string_view author_id,
    std::string text,
    std::string handle) {
    if (busy()) {
        return busy_notice();
    }
    if (text.empty() && handle.empty()) {
        return {};
    }

    ControllerUpdate update;
    const CharacterMetadata* target = nullptr;
    if (handle.empty()) {
        target = characters_.find(default_character_id_);
    } else {
        const HandleResolution resolution = characters_.resolve_handle(handle);
        if (resolution.match != HandleMatch::resolved) {
            update.notice = format_handle_resolution_notice(
                handle, resolution, characters_);
            return update;
        }
        target = resolution.character;
    }
    if (!target) {
        throw std::logic_error("Default character is not among the forum characters");
    }
    if (text.empty()) {
        update.notice = "Prompt for @" + target->display_name + " is empty";
        return update;
    }

    std::optional<EntryIdentity> author = resolve_author(author_id, update);
    if (!author) return update;

    SharedModelHistory history =
        std::make_shared<const ModelHistory>(
            transcript_.model_history());
    update.input_consumed = true;
    start_batch(
        std::move(*author),
        std::move(text),
        std::vector<CharacterMetadata>{*target},
        std::move(history),
        update);
    return update;
}

void SessionController::start_batch(
    EntryIdentity author,
    std::string text,
    std::vector<CharacterMetadata> targets,
    SharedModelHistory history,
    ControllerUpdate& update) {
    if (!history || targets.empty()) {
        throw std::invalid_argument(
            "Generation batch requires history and at least one target");
    }
    // Each input owns its own run, so there is no second run vector to keep in
    // step with the batch's execution slots.
    std::vector<GenerationRequest> inputs;
    inputs.reserve(targets.size());
    for (CharacterMetadata& target : targets) {
        inputs.push_back({
            .history = history,
            .run = {
                .request_id = next_request_id_++,
                .target = std::move(target),
                .author = author,
                .prompt_text = text,
            },
        });
    }

    try {
        batch_.emplace(generation_executor_.stage_batch(std::move(inputs)));
    } catch (const std::runtime_error&) {
        update.input_consumed = false;
        update.notice = "Request could not be dispatched";
        return;
    }

    try {
        activate_current_run(update);
        // Only now can a backend publish output, and only into session state
        // that is already durable and able to receive it.
        batch_->open();
    } catch (...) {
        release_batch();
        throw;
    }
}

void SessionController::activate_current_run(ControllerUpdate& update) {
    if (!batch_) {
        throw std::logic_error("Foreground activation requires a staged batch");
    }
    const RunSpec& run = batch_->foreground_run();
    TranscriptEntry prompt = make_human_entry({
        .id = next_entry_id_++,
        .author = run.author,
        .addressed_to = {run.target.id, run.target.display_name},
        .text = run.prompt_text,
        .request_id = run.request_id,
    });
    ActiveResponse response{
        .request_id = run.request_id,
        .response_entry_id = next_entry_id_++,
        .character_id = run.target.id,
        .character_display_name = run.target.display_name,
        .phase = ResponsePhase::waiting,
    };

    if (before_activation_) {
        before_activation_(batch_->foreground_index());
    }
    persist(
        request_action(
            "persist start of",
            run.request_id,
            run.target.display_name),
        [this, &run, &prompt] {
            journal_.start_turn(run.request_id, prompt);
        });
    try {
        transcript_.add_entry(prompt);
    } catch (...) {
        TranscriptEntry error = make_error_entry(
            next_entry_id_++,
            "Failed to add the submitted prompt to the transcript",
            run.request_id,
            run.target.id);
        persist(
            request_action(
                "persist failure of",
                run.request_id,
                run.target.display_name),
            [this, &run, &error] {
                journal_.fail_turn(run.request_id, error);
            });
        throw;
    }
    active_ = std::move(response);
    require_snapshot(update);
    update.notice = "";
}

void SessionController::start_next_batch_run(ControllerUpdate& update) {
    if (!batch_) {
        return;
    }
    if (batch_->cancellation_requested()) {
        poll_abort_cleanup(update);
        return;
    }

    try {
        activate_current_run(update);
    } catch (...) {
        release_batch();
        throw;
    }
}

void SessionController::finish_batch_run(ControllerUpdate& update) {
    if (!batch_) {
        return;
    }
    if (shutdown_) {
        finish_batch(update);
        return;
    }

    if (batch_->cancellation_requested()) {
        append_batch_notice(update);
        poll_abort_cleanup(update);
        return;
    }

    if (!batch_->has_next_foreground()) {
        finish_batch(update);
        return;
    }
    append_batch_notice(update);
    batch_->advance_foreground();
    start_next_batch_run(update);
}

void SessionController::finish_batch(ControllerUpdate& update) {
    if (!batch_) {
        return;
    }
    append_batch_notice(update);
    const std::string terminal_notices = terminal_notices_;
    release_batch();
    // Ending the batch ends the visible generation. Every caller reaches here
    // from a terminal event that already requested a snapshot; classify it
    // locally anyway so this helper cannot be moved into a purer path.
    require_snapshot(update);
    if (!terminal_notices.empty()) {
        update.notice = terminal_notices;
    }
}

void SessionController::finish_aborted_batch(ControllerUpdate& update) {
    if (!batch_) {
        return;
    }
    std::string notices = terminal_notices_;
    if (!stop_notice_recorded_ && !notices.empty()) {
        notices += "\nGeneration stopped";
    } else if (!stop_notice_recorded_) {
        notices = "Generation stopped";
    }
    release_batch();
    require_snapshot(update);
    update.notice = std::move(notices);
}

void SessionController::poll_abort_cleanup(ControllerUpdate& update) {
    if (!batch_ || !batch_->cancellation_requested()) {
        return;
    }
    if (!active_ && batch_->executions_finished()) {
        finish_aborted_batch(update);
    }
}

void SessionController::append_batch_notice(const ControllerUpdate& update) {
    if (!batch_ || !update.notice || update.notice->empty()) {
        return;
    }
    if (!terminal_notices_.empty()) {
        terminal_notices_ += '\n';
    }
    terminal_notices_ += *update.notice;
}

void SessionController::release_batch() noexcept {
    if (batch_) {
        // Wait here rather than in the destructor so every blocking point is
        // visible on the path that reaches it. Events buffered for children
        // that never became foreground are discarded with their queues.
        batch_->cancel();
        batch_->wait_until_finished();
        batch_.reset();
    }
    terminal_notices_.clear();
    stop_notice_recorded_ = false;
}

ControllerUpdate SessionController::clear_transcript() {
    if (busy()) {
        return busy_notice();
    }
    try {
        journal_.clear();
    } catch (const std::exception& error) {
        throw std::runtime_error(
            std::string("Failed to persist /clear: ") + error.what());
    }
    transcript_.clear();
    return {
        .state = SnapshotRequired{},
        .input_consumed = true,
        .notice = "Transcript cleared",
    };
}

ControllerUpdate SessionController::open_offrecord() {
    if (busy()) {
        return busy_notice();
    }
    if (!transcript_.open_offrecord(next_entry_id_)) {
        return {
            .input_consumed = true,
            .notice = "Already off the record; use /hide-off first",
        };
    }
    ++next_entry_id_;
    return {
        .state = SnapshotRequired{},
        .input_consumed = true,
    };
}

ControllerUpdate SessionController::extend_offrecord() {
    if (busy()) {
        return busy_notice();
    }
    if (!transcript_.extend_offrecord(next_entry_id_)) {
        return {
            .input_consumed = true,
            .notice = "No off-record span to extend",
        };
    }
    ++next_entry_id_;
    return {
        .state = SnapshotRequired{},
        .input_consumed = true,
    };
}

ControllerUpdate SessionController::restore_offrecord() {
    if (busy()) {
        return busy_notice();
    }
    if (!transcript_.restore_offrecord(next_entry_id_)) {
        return {
            .input_consumed = true,
            .notice = "Nothing to restore",
        };
    }
    ++next_entry_id_;
    return {
        .state = SnapshotRequired{},
        .input_consumed = true,
    };
}

ControllerUpdate SessionController::start_multicast(
    std::string_view author_id,
    std::string text,
    std::vector<std::string> handles) {
    if (busy()) {
        return busy_notice();
    }

    std::vector<CharacterMetadata> targets;
    if (handles.empty()) {
        targets = characters_.all();
    } else {
        std::unordered_set<ParticipantId> distinct;
        targets.reserve(handles.size());
        for (const std::string& handle : handles) {
            const HandleResolution resolution =
                characters_.resolve_handle(handle);
            if (resolution.match != HandleMatch::resolved) {
                return {
                    .notice = format_handle_resolution_notice(
                        handle, resolution, characters_),
                };
            }
            if (!distinct.insert(resolution.character->id).second) {
                return {
                    .notice = format_duplicate_character_notice(
                        resolution.character->display_name),
                };
            }
            targets.push_back(*resolution.character);
        }
    }
    return start_resolved_multicast(author_id, std::move(text), std::move(targets));
}

ControllerUpdate SessionController::start_resolved_multicast(
    std::string_view author_id,
    std::string text,
    std::vector<CharacterMetadata> targets) {
    if (text.empty()) {
        return {.notice = "Multicast prompt is empty"};
    }
    if (targets.empty()) {
        return {.notice = "Multicast has no targets"};
    }

    ControllerUpdate update;
    std::optional<EntryIdentity> author = resolve_author(author_id, update);
    if (!author) return update;

    // Capture once so the off-record precondition and every child use the
    // same model history.
    SharedModelHistory history =
        std::make_shared<const ModelHistory>(
            transcript_.model_history());
    if (history->offrecord_span.begin) {
        return {
            .notice =
                "Cannot start multicast while an off-record span is active",
        };
    }

    update.input_consumed = true;
    start_batch(
        std::move(*author),
        std::move(text),
        std::move(targets),
        std::move(history),
        update);
    return update;
}

ControllerUpdate SessionController::session_information() {
    if (busy()) {
        return busy_notice();
    }
    return {
        .input_consumed = true,
        .notice = format_session_information(
            transcript_.view().size(),
            characters_,
            generation_executor_.runtime_info(),
            default_character_id_),
    };
}

ControllerUpdate SessionController::character_information() {
    if (busy()) {
        return busy_notice();
    }
    return {
        .input_consumed = true,
        .notice = format_characters_notice(
            characters_, generation_executor_.runtime_info(), default_character_id_),
    };
}

ControllerUpdate SessionController::set_default_character(std::string_view handle) {
    if (busy()) {
        return busy_notice();
    }
    ControllerUpdate update{.input_consumed = true};
    if (handle.empty()) {
        update.notice = "Usage: /@CharacterName";
        return update;
    }
    const HandleResolution result = characters_.resolve_handle(handle);
    if (result.match != HandleMatch::resolved) {
        update.notice = format_handle_resolution_notice(handle, result, characters_);
        return update;
    }
    default_character_id_ = result.character->id;
    require_snapshot(update);
    update.notice = "Default character is now " + result.character->display_name;
    return update;
}

ControllerUpdate SessionController::set_default_character_by_id(std::string_view id) {
    if (busy()) {
        return busy_notice();
    }
    // This typed action submits no editor text, so it never clears a draft.
    ControllerUpdate update;
    const CharacterMetadata* character = characters_.find(id);
    if (!character) {
        update.notice = "Unknown character";
        return update;
    }
    default_character_id_ = character->id;
    require_snapshot(update);
    update.notice = "Default character is now " + character->display_name;
    return update;
}

ControllerUpdate SessionController::request_stop() {
    ControllerUpdate update;
    if (!batch_) {
        update.notice = "No generation is active";
        return update;
    }

    if (!batch_->cancellation_requested()) {
        log_info("Session generation cancellation requested");
        // Non-blocking: cancellation only signals the executions. The event
        // loop performs the cleanup.
        batch_->cancel();
        require_snapshot(update);
    }
    if (!active_) {
        poll_abort_cleanup(update);
        if (!update.notice) {
            update.notice = "Stopping generation...";
        }
        return update;
    }
    update.notice = "Stopping generation...";
    return update;
}

void SessionController::rename(std::string_view label) {
    validate_session_label(label);
    persist("rename session", [this, label] { journal_.rename(label); });
}

ControllerUpdate SessionController::handle_generation_event(GenerationEvent event) {
    ControllerUpdate update;
    std::visit(
        [this, &update](const auto& value) { apply(value, update); },
        event);
    return update;
}

void SessionController::apply(const GenerationEventDelta& event, ControllerUpdate& update) {
    if (!matches(event.request_id) || event.text.empty()) {
        return;
    }
    if (event.kind == GenerationDeltaKind::answer) {
        // Opening the response entry also changes the phase, so only growth of
        // an already-answering entry is a pure append.
        const bool structural = active_->phase != ResponsePhase::answering;
        if (structural) {
            transcript_.begin_entry(response_entry(EntryStatus::streaming));
        }
        // Capture the target before the text moves into transcript storage.
        const EntryId entry_id = active_->response_entry_id;
        transcript_.append_answer(entry_id, event.text);
        active_->phase = ResponsePhase::answering;
        if (structural) {
            require_snapshot(update);
            return;
        }
        merge(update, {.state = TextAppend{EntryTextTarget{entry_id}, event.text}});
        return;
    }
    // The first reasoning chunk establishes visible request state. Later
    // reasoning is a pure append even after answering began; the frontend's
    // transport decides whether that target switch needs a snapshot.
    const bool structural = active_->phase == ResponsePhase::waiting;
    const RequestId request_id = active_->request_id;
    active_->reasoning_text.append(event.text);
    if (structural) {
        active_->phase = ResponsePhase::reasoning;
        require_snapshot(update);
        return;
    }
    merge(update, {.state = TextAppend{ReasoningTextTarget{request_id}, event.text}});
}

void SessionController::apply(const GenerationCompleted& event, ControllerUpdate& update) {
    if (!matches(event.request_id)) {
        return;
    }
    if (active_->phase != ResponsePhase::answering) {
        fail_active_response(
            "Generation finished without answer content", active_->character_id, update);
        finish_batch_run(update);
        return;
    }
    const TranscriptEntry response =
        response_entry(EntryStatus::complete);
    persist(
        request_action(
            "persist generation result for",
            event.request_id,
            active_->character_display_name),
        [this, &event, &response] {
            journal_.complete_turn(event.request_id, response);
        });
    finish_response_entry(EntryStatus::complete);
    active_.reset();
    require_snapshot(update);
    update.notice = "";
    finish_batch_run(update);
}

void SessionController::apply(const GenerationCancelled& event, ControllerUpdate& update) {
    if (!matches(event.request_id)) {
        return;
    }
    if (active_->phase == ResponsePhase::answering) {
        const TranscriptEntry response =
            response_entry(EntryStatus::cancelled);
        persist(
            request_action(
                "persist cancellation of",
                event.request_id,
                active_->character_display_name),
            [this, &event, &response] {
                journal_.cancel_turn(event.request_id, response);
            });
        finish_response_entry(EntryStatus::cancelled);
    } else {
        persist(
            request_action(
                "persist cancellation of",
                event.request_id,
                active_->character_display_name),
            [this, &event] {
                journal_.cancel_turn(event.request_id, std::nullopt);
            });
    }
    active_.reset();
    require_snapshot(update);
    update.notice = "Generation stopped";
    stop_notice_recorded_ = true;
    finish_batch_run(update);
}

void SessionController::apply(const GenerationFailed& event, ControllerUpdate& update) {
    if (matches(event.request_id)) {
        fail_active_response(event.message, active_->character_id, update);
        finish_batch_run(update);
    }
}

void SessionController::fail_active_response(
    std::string message,
    ParticipantId participant_id,
    ControllerUpdate& update) {
    TranscriptEntry error = make_error_entry(
        next_entry_id_++,
        std::move(message),
        active_->request_id,
        std::move(participant_id));
    log_error("Session generation failed");
    persist(
        request_action(
            "persist failure of",
            active_->request_id,
            active_->character_display_name),
        [this, &error] {
            journal_.fail_turn(active_->request_id, error);
        });
    if (active_->phase == ResponsePhase::answering) {
        transcript_.discard_entry(active_->response_entry_id);
    }
    transcript_.add_entry(std::move(error));
    active_.reset();
    require_snapshot(update);
    update.notice = "Generation failed";
}

void SessionController::finish_response_entry(EntryStatus status) {
    transcript_.finish_entry(active_->response_entry_id, status);
}

TranscriptEntry SessionController::response_entry(EntryStatus status) const {
    std::string text;
    if (active_->phase == ResponsePhase::answering) {
        text = transcript_.open_entry_text(active_->response_entry_id);
    }
    return make_character_entry(
        active_->response_entry_id,
        active_->character_id,
        active_->character_display_name,
        std::move(text),
        status,
        active_->request_id);
}

bool SessionController::matches(RequestId request_id) const {
    return active_ && active_->request_id == request_id;
}

ControllerEventBatch SessionController::receive_events(std::size_t max_events) {
    if (max_events == 0) {
        throw std::invalid_argument("Generation event batch size must be positive");
    }
    ControllerUpdate update;
    if (shutdown_ && !batch_) {
        update.session_ended = true;
        return {.update = std::move(update)};
    }
    GenerationEvent event = GenerationCompleted{};
    std::size_t processed = 0;
    while (batch_ && active_ && processed < max_events) {
        const ChannelReadStatus status = batch_->try_receive_foreground(event);
        if (status != ChannelReadStatus::value) {
            break;
        }
        merge(update, handle_generation_event(std::move(event)));
        ++processed;
    }
    poll_abort_cleanup(update);
    return {
        .update = std::move(update),
        .full = processed == max_events,
    };
}

void SessionController::shutdown() {
    if (shutdown_) {
        return;
    }
    log_info("Session controller shutting down");
    shutdown_ = true;
    try {
        if (batch_) {
            batch_->cancel();
            // No execution can reach a backend after this returns, while its
            // queues stay drainable below.
            batch_->wait_until_finished();
        }
        (void)receive_events(std::numeric_limits<std::size_t>::max());
        active_.reset();
        release_batch();
    } catch (...) {
        // `execution_finished` allows a task to issue its final wake after
        // wait_until_finished() returns. Join the pool while the executor and
        // its borrowed notifier are still alive even when terminal persistence
        // fails.
        worker_pool_.stop();
        throw;
    }
    worker_pool_.stop();
}

} // namespace cha
