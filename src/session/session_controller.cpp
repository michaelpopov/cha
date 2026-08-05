#include "session/session_controller.h"

#include "util/logging.h"

#include <algorithm>
#include <exception>
#include <limits>
#include <sstream>
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
    std::string_view agent_name) {
    return std::string(action) + " request " + std::to_string(request_id)
        + " for @" + std::string(agent_name);
}

ForumCharacters make_forum_characters(
    const std::vector<AgentRuntimeInfo>& runtime_info) {
    std::vector<CharacterInfo> characters;
    characters.reserve(runtime_info.size());
    for (const AgentRuntimeInfo& agent : runtime_info) {
        characters.push_back(agent.character);
    }
    // Definitions reached the controller through either the validated
    // workspace boundary or a trusted application built-in factory.
    return ForumCharacters(std::move(characters), true);
}

void merge_change(SessionChange& all, SessionChange one) {
    all.state_changed = all.state_changed || one.state_changed;
    all.controller_ended = all.controller_ended || one.controller_ended;
    if (one.notice) {
        all.notice = std::move(one.notice);
    }
}

std::string format_characters_notice(
    const ForumCharacters& characters,
    const std::vector<AgentRuntimeInfo>& runtime_info,
    const ParticipantId& default_agent_id) {
    std::ostringstream result;
    result << "Characters in this forum (" << characters.all().size()
           << "), * marks the default.";
    result << " Any unambiguous prefix works.";
    for (const AgentRuntimeInfo& agent : runtime_info) {
        result << " | " << (agent.character.id == default_agent_id ? "* " : "")
               << "@" << agent.character.name << "  " << agent.model << "  "
               << agent.api << "  "
               << (agent.streaming ? "streaming" : "non-streaming");
    }
    return result.str();
}

std::string format_session_information(
    const Transcript& transcript,
    const ForumCharacters& characters,
    const std::vector<AgentRuntimeInfo>& runtime_info,
    const ParticipantId& default_agent_id) {
    std::ostringstream text;
    text << "Transcript entries: " << transcript.size()
         << " | " << format_characters_notice(
             characters, runtime_info, default_agent_id);
    return text.str();
}

void require_agent_count(std::size_t count) {
    if (count == 0) {
        throw std::invalid_argument(
            "Agent registry requires at least one agent");
    }
}

} // namespace

std::unique_ptr<SessionController> SessionController::from_definitions(
    std::vector<AgentDefinition> definitions,
    PersonaRoster personas,
    ParticipantId initial_default_agent_id,
    std::filesystem::path database_path,
    SessionLease lease,
    WakeNotifier& notifier,
    SessionRestore restored) {
    require_agent_count(definitions.size());
    if (!lease.active()) {
        throw std::invalid_argument(
            "Production session controllers require an active session lease");
    }
    return std::unique_ptr<SessionController>(new SessionController(
        std::move(definitions),
        std::move(personas),
        std::move(initial_default_agent_id),
        std::move(database_path),
        std::move(lease),
        notifier,
        std::move(restored)));
}

std::unique_ptr<SessionController> SessionController::from_shared_definitions(
    std::vector<AgentDefinition> definitions,
    SharedPersonaRoster personas,
    ParticipantId initial_default_agent_id,
    std::filesystem::path database_path,
    SessionLease lease,
    WakeNotifier& notifier,
    SessionRestore restored) {
    require_agent_count(definitions.size());
    if (!personas) throw std::invalid_argument("Session controller requires a persona roster");
    if (!lease.active()) throw std::invalid_argument("Production session controllers require an active session lease");
    return std::unique_ptr<SessionController>(new SessionController(
        std::move(definitions), std::move(personas), std::move(initial_default_agent_id),
        std::move(database_path), std::move(lease), notifier, std::move(restored)));
}

std::unique_ptr<SessionController> SessionController::from_definitions_for_testing(
    std::vector<AgentDefinition> definitions,
    PersonaRoster personas,
    ParticipantId initial_default_agent_id,
    std::filesystem::path database_path,
    WakeNotifier& notifier,
    SessionRestore restored) {
    require_agent_count(definitions.size());
    return std::unique_ptr<SessionController>(new SessionController(
        std::move(definitions),
        std::move(personas),
        std::move(initial_default_agent_id),
        std::move(database_path),
        SessionLease::inactive_for_testing(),
        notifier,
        std::move(restored)));
}

std::unique_ptr<SessionController> SessionController::from_backends_for_testing(
    std::vector<std::unique_ptr<CompletionBackend>> backends,
    PersonaRoster personas,
    ParticipantId initial_default_agent_id,
    std::filesystem::path database_path,
    WakeNotifier& notifier,
    SessionRestore restored,
    ActivationHook before_activation) {
    require_agent_count(backends.size());
    return std::unique_ptr<SessionController>(new SessionController(
        std::move(backends),
        std::move(personas),
        std::move(initial_default_agent_id),
        std::move(database_path),
        notifier,
        std::move(restored),
        std::move(before_activation)));
}

SessionController::SessionController(
    std::vector<AgentDefinition> definitions,
    PersonaRoster personas,
    ParticipantId initial_default_agent_id,
    std::filesystem::path path,
    SessionLease lease,
    WakeNotifier& notifier,
    SessionRestore restored)
    : lease_(std::move(lease)),
      journal_(std::move(path)),
      worker_pool_(definitions.size()),
      registry_(std::move(definitions), notifier, worker_pool_),
      characters_(make_forum_characters(registry_.runtime_info())),
      personas_(std::make_shared<const PersonaRoster>(std::move(personas))),
      default_agent_id_(std::move(initial_default_agent_id)) {
    if (!characters_.find(default_agent_id_)) {
        throw std::invalid_argument("Initial default agent ID is not in the forum roster");
    }
    initialize(std::move(restored));
}

SessionController::SessionController(
    std::vector<AgentDefinition> definitions,
    SharedPersonaRoster personas,
    ParticipantId initial_default_agent_id,
    std::filesystem::path path,
    SessionLease lease,
    WakeNotifier& notifier,
    SessionRestore restored)
    : lease_(std::move(lease)),
      journal_(std::move(path)),
      worker_pool_(definitions.size()),
      registry_(std::move(definitions), notifier, worker_pool_),
      characters_(make_forum_characters(registry_.runtime_info())),
      personas_(std::move(personas)),
      default_agent_id_(std::move(initial_default_agent_id)) {
    if (!characters_.find(default_agent_id_)) throw std::invalid_argument("Initial default agent ID is not in the forum roster");
    initialize(std::move(restored));
}

SessionController::SessionController(
    std::vector<std::unique_ptr<CompletionBackend>> backends,
    PersonaRoster personas,
    ParticipantId initial_default_agent_id,
    std::filesystem::path path,
    WakeNotifier& notifier,
    SessionRestore restored,
    ActivationHook before_activation)
    : lease_(SessionLease::inactive_for_testing()),
      journal_(std::move(path)),
      worker_pool_(backends.size()),
      registry_(std::move(backends), notifier, worker_pool_),
      characters_(make_forum_characters(registry_.runtime_info())),
      personas_(std::make_shared<const PersonaRoster>(std::move(personas))),
      default_agent_id_(std::move(initial_default_agent_id)),
      before_activation_(std::move(before_activation)) {
    if (!characters_.find(default_agent_id_)) {
        throw std::invalid_argument("Initial default agent ID is not in the forum roster");
    }
    initialize(std::move(restored));
}

SessionController::~SessionController() {
    try {
        shutdown();
    } catch (...) {
    }
}

void SessionController::initialize(SessionRestore restored) {
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

GenerationStatus SessionController::generation_status() const {
    const GenerationStatusView view = generation_status_view();
    return {
        .active = view.active,
        .request_id = view.request_id,
        .agent_id = std::string(view.agent_id),
        .agent_name = std::string(view.agent_name),
        .phase = view.phase,
        .reasoning_text = std::string(view.reasoning_text),
    };
}

GenerationStatusView SessionController::generation_status_view() const noexcept {
    if (batch_ && batch_->abort_requested) {
        const RunSpec& run = batch_->runs[batch_->foreground_index];
        return {
            .active = true,
            .request_id = run.request_id,
            .agent_id = run.target.id,
            .agent_name = run.target.name,
            .phase = ResponsePhase::stopping,
        };
    }
    return {
        .active = busy(),
        .request_id = active_ ? std::optional<RequestId>(active_->request_id)
                              : std::nullopt,
        .agent_id = active_ ? std::string_view(active_->agent_id)
                            : std::string_view{},
        .agent_name = active_ ? std::string_view(active_->agent_name)
                              : std::string_view{},
        .phase = active_ ? active_->phase : ResponsePhase::waiting,
        .reasoning_text = active_ ? std::string_view(active_->reasoning_text)
                                  : std::string_view{},
    };
}

SessionState SessionController::state() const {
    const TranscriptView view = transcript_.view();
    return {
        .characters = characters_.all(),
        .default_agent_id = default_agent_id_,
        .transcript = {view.entries.begin(), view.entries.end()},
        .revision = view.revision,
        .open_entry_id = view.open_entry_id,
        .history_epoch = view.history_epoch,
        .generation = generation_status(),
    };
}

std::optional<SessionAppendProjection> SessionController::text_append_since(
    const SessionStateCursor& cursor) const {
    const TranscriptView transcript = transcript_.view();
    const GenerationStatusView generation = generation_status_view();
    if (cursor.history_epoch != transcript.history_epoch
        || cursor.default_agent_id != default_agent_id_
        || cursor.entry_count != transcript.entries.size()
        || cursor.open_entry_id != transcript.open_entry_id
        || !cursor.request_id || !generation.active
        || generation.request_id != cursor.request_id
        || generation.phase != cursor.phase) {
        return std::nullopt;
    }
    if (generation.phase == ResponsePhase::reasoning) {
        if (transcript.revision != cursor.revision
            || generation.reasoning_text.size() <= cursor.reasoning_length) {
            return std::nullopt;
        }
        // All of the non-text continuity fields were matched above. Updating
        // the scalar cursor directly keeps this owner-thread hot path from
        // materializing an owning SessionState (and copying its transcript).
        SessionStateCursor next = cursor;
        next.revision = transcript.revision;
        next.reasoning_length = generation.reasoning_text.size();
        return SessionAppendProjection{
            .append = {ReasoningTextTarget{*generation.request_id},
                       std::string(generation.reasoning_text.substr(cursor.reasoning_length))},
            .cursor = std::move(next),
        };
    }
    if (generation.phase != ResponsePhase::answering
        || transcript.revision <= cursor.revision || !transcript.open_entry_id
        || transcript.entries.empty()
        // Reasoning may arrive after answer chunks. It cannot be hidden inside
        // an answer append, so require that the other appendable text is also
        // unchanged before proving this target.
        || generation.reasoning_text.size() != cursor.reasoning_length) {
        return std::nullopt;
    }
    const TranscriptEntry& entry = transcript.entries.back();
    if (entry.id != *transcript.open_entry_id || entry.status != EntryStatus::streaming
        || entry.request_id != cursor.request_id
        || entry.text.size() <= cursor.answer_length) {
        return std::nullopt;
    }
    // The checks above prove that the cursor differs only by answer growth
    // and the transcript revision caused by that growth.
    SessionStateCursor next = cursor;
    next.revision = transcript.revision;
    next.answer_length = entry.text.size();
    return SessionAppendProjection{
        .append = {EntryTextTarget{entry.id}, entry.text.substr(cursor.answer_length)},
        .cursor = std::move(next),
    };
}

bool SessionController::is_generating() const noexcept {
    return busy();
}

bool SessionController::busy() const noexcept {
    return active_ || batch_;
}

SessionChange SessionController::busy_notice() const {
    return {.notice = std::string(generation_in_progress_notice)};
}

std::optional<EntryIdentity> SessionController::resolve_author(
    std::string_view author_id,
    SessionChange& update) const {
    const auto author = std::find_if(
        personas_->begin(), personas_->end(),
        [author_id](const Persona& persona) { return persona.id == author_id; });
    if (author == personas_->end()) {
        update.notice = "Unknown persona ID '" + std::string(author_id) + "'";
        return std::nullopt;
    }
    return EntryIdentity{author->id, author->display_name};
}

SessionChange SessionController::submit_prompt(
    std::string_view author_id,
    std::string text,
    std::string handle) {
    if (busy()) {
        return busy_notice();
    }
    if (text.empty() && handle.empty()) {
        return {};
    }

    SessionChange update;
    const CharacterInfo* target = nullptr;
    if (handle.empty()) {
        target = characters_.find(default_agent_id_);
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
        throw std::logic_error("Default agent is not among the forum characters");
    }
    if (text.empty()) {
        update.notice = "Prompt for @" + target->name + " is empty";
        return update;
    }

    std::optional<EntryIdentity> author = resolve_author(author_id, update);
    if (!author) return update;

    SharedCompletionHistory history =
        std::make_shared<const CompletionHistory>(
            transcript_.completion_history());
    update.input_consumed = true;
    start_batch(
        std::move(*author),
        std::move(text),
        std::vector<CharacterInfo>{*target},
        std::move(history),
        update);
    return update;
}

void SessionController::start_batch(
    EntryIdentity author,
    std::string text,
    std::vector<CharacterInfo> targets,
    SharedCompletionHistory history,
    SessionChange& update) {
    if (!history || targets.empty()) {
        throw std::invalid_argument(
            "Response batch requires history and at least one target");
    }
    ResponseBatch batch{
        .history = std::move(history),
    };
    batch.runs.reserve(targets.size());
    for (CharacterInfo& target : targets) {
        batch.runs.push_back({
            .request_id = next_request_id_++,
            .target = std::move(target),
            .author = author,
            .prompt_text = text,
        });
    }

    std::vector<CompletionInput> inputs;
    inputs.reserve(batch.runs.size());
    for (const RunSpec& run : batch.runs) {
        inputs.push_back({
            .history = batch.history,
            .run = run,
        });
    }

    try {
        (void)registry_.stage_batch(std::move(inputs));
    } catch (const std::runtime_error&) {
        update.input_consumed = false;
        update.notice = "Request could not be dispatched";
        return;
    }

    batch_ = std::move(batch);
    try {
        activate_current_run(update);
        registry_.open_gate();
    } catch (...) {
        abandon_batch();
        registry_.clear_batch();
        throw;
    }
}

void SessionController::activate_current_run(SessionChange& update) {
    if (!batch_ || batch_->foreground_index >= batch_->runs.size()) {
        throw std::logic_error("Response batch run index is out of range");
    }
    const RunSpec& run = batch_->runs[batch_->foreground_index];
    TranscriptEntry prompt = make_human_entry({
        .id = next_entry_id_++,
        .author = run.author,
        .addressed_to = {run.target.id, run.target.name},
        .text = run.prompt_text,
        .request_id = run.request_id,
    });
    ActiveResponse response{
        .request_id = run.request_id,
        .response_entry_id = next_entry_id_++,
        .agent_id = run.target.id,
        .agent_name = run.target.name,
        .phase = ResponsePhase::waiting,
    };

    if (before_activation_) {
        before_activation_(batch_->foreground_index);
    }
    persist(
        request_action(
            "persist start of",
            run.request_id,
            run.target.name),
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
                run.target.name),
            [this, &run, &error] {
                journal_.fail_turn(run.request_id, error);
            });
        throw;
    }
    active_ = std::move(response);
    update.state_changed = true;
    update.notice = "";
}

void SessionController::start_next_batch_run(SessionChange& update) {
    if (!batch_) {
        return;
    }
    if (batch_->abort_requested) {
        poll_abort_cleanup(update);
        return;
    }
    if (batch_->foreground_index >= batch_->runs.size()) {
        throw std::logic_error("Response batch run index is out of range");
    }

    try {
        activate_current_run(update);
    } catch (...) {
        abandon_batch();
        registry_.clear_batch();
        throw;
    }
}

void SessionController::finish_batch_run(SessionChange& update) {
    if (!batch_) {
        return;
    }
    if (shutdown_) {
        finish_batch(update);
        return;
    }

    if (batch_->abort_requested) {
        append_batch_notice(update);
        poll_abort_cleanup(update);
        return;
    }

    if (batch_->foreground_index + 1 == batch_->runs.size()) {
        finish_batch(update);
        return;
    }
    append_batch_notice(update);
    ++batch_->foreground_index;
    start_next_batch_run(update);
}

void SessionController::finish_batch(SessionChange& update) {
    if (!batch_) {
        return;
    }
    append_batch_notice(update);
    const std::string terminal_notices = batch_->terminal_notices;
    registry_.clear_batch();
    abandon_batch();
    if (!terminal_notices.empty()) {
        update.notice = terminal_notices;
    }
}

void SessionController::finish_aborted_batch(SessionChange& update) {
    if (!batch_) {
        return;
    }
    std::string notices = batch_->terminal_notices;
    if (!batch_->stop_notice_recorded && !notices.empty()) {
        notices += "\nGeneration stopped";
    } else if (!batch_->stop_notice_recorded) {
        notices = "Generation stopped";
    }
    abandon_batch();
    update.state_changed = true;
    update.notice = std::move(notices);
}

void SessionController::poll_abort_cleanup(SessionChange& update) {
    if (!batch_ || !batch_->abort_requested) {
        return;
    }
    if (!active_ && registry_.executions_finished()) {
        registry_.clear_batch();
        finish_aborted_batch(update);
    }
}

void SessionController::append_batch_notice(const SessionChange& update) {
    if (!batch_ || !update.notice || update.notice->empty()) {
        return;
    }
    if (!batch_->terminal_notices.empty()) {
        batch_->terminal_notices += '\n';
    }
    batch_->terminal_notices += *update.notice;
}

void SessionController::abandon_batch() {
    batch_.reset();
}

SessionChange SessionController::clear_transcript() {
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
        .state_changed = true,
        .input_consumed = true,
        .notice = "Transcript cleared",
    };
}

SessionChange SessionController::open_offrecord() {
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
        .state_changed = true,
        .input_consumed = true,
    };
}

SessionChange SessionController::extend_offrecord() {
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
        .state_changed = true,
        .input_consumed = true,
    };
}

SessionChange SessionController::restore_offrecord() {
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
        .state_changed = true,
        .input_consumed = true,
    };
}

SessionChange SessionController::start_multicast(
    std::string_view author_id,
    std::string text,
    std::vector<std::string> handles) {
    if (busy()) {
        return busy_notice();
    }

    std::vector<ParticipantId> ids;
    ids.reserve(handles.size());
    for (const std::string& handle : handles) {
        const HandleResolution resolution = characters_.resolve_handle(handle);
        if (resolution.match != HandleMatch::resolved) {
            return {
                .notice = format_handle_resolution_notice(
                    handle, resolution, characters_),
            };
        }
        ids.push_back(resolution.character->id);
    }
    return start_multicast_from_ids(author_id, std::move(text), std::move(ids));
}

SessionChange SessionController::start_multicast_by_ids(
    std::string_view author_id,
    std::string text,
    std::vector<ParticipantId> ids) {
    if (busy()) {
        return busy_notice();
    }

    return start_multicast_from_ids(author_id, std::move(text), std::move(ids));
}

SessionChange SessionController::start_multicast_from_ids(
    std::string_view author_id,
    std::string text,
    std::vector<ParticipantId> ids) {
    std::vector<CharacterInfo> targets;
    if (ids.empty()) {
        targets = characters_.all();
    } else {
        std::unordered_set<ParticipantId> distinct;
        targets.reserve(ids.size());
        for (const ParticipantId& id : ids) {
            const CharacterInfo* target = characters_.find(id);
            if (!target) {
                return {.notice = "Unknown multicast target ID '" + id + "'"};
            }
            if (!distinct.insert(id).second) {
                return {.notice = format_duplicate_character_notice(target->name)};
            }
            targets.push_back(*target);
        }
    }
    return start_resolved_multicast(author_id, std::move(text), std::move(targets));
}

SessionChange SessionController::start_resolved_multicast(
    std::string_view author_id,
    std::string text,
    std::vector<CharacterInfo> targets) {
    if (text.empty()) {
        return {.notice = "Multicast prompt is empty"};
    }
    if (targets.empty()) {
        return {.notice = "Multicast has no targets"};
    }

    SessionChange update;
    std::optional<EntryIdentity> author = resolve_author(author_id, update);
    if (!author) return update;

    // Capture once so the off-record precondition and every child use the
    // same completion history.
    SharedCompletionHistory history =
        std::make_shared<const CompletionHistory>(
            transcript_.completion_history());
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

SessionChange SessionController::session_information() {
    if (busy()) {
        return busy_notice();
    }
    return {
        .input_consumed = true,
        .notice = format_session_information(
            transcript_,
            characters_,
            registry_.runtime_info(),
            default_agent_id_),
    };
}

SessionChange SessionController::agent_information() {
    if (busy()) {
        return busy_notice();
    }
    return {
        .input_consumed = true,
        .notice = format_characters_notice(
            characters_, registry_.runtime_info(), default_agent_id_),
    };
}

SessionChange SessionController::set_default_agent(std::string_view handle) {
    if (busy()) {
        return busy_notice();
    }
    SessionChange update{.input_consumed = true};
    if (handle.empty()) {
        update.notice = "Usage: /@AgentName";
        return update;
    }
    const HandleResolution result = characters_.resolve_handle(handle);
    if (result.match != HandleMatch::resolved) {
        update.notice = format_handle_resolution_notice(handle, result, characters_);
        return update;
    }
    default_agent_id_ = result.character->id;
    update.state_changed = true;
    update.notice = "Default agent is now " + result.character->name;
    return update;
}

SessionChange SessionController::set_default_agent_by_id(std::string_view id) {
    if (busy()) {
        return busy_notice();
    }
    // This typed action submits no editor text, so it never clears a draft.
    SessionChange update;
    const CharacterInfo* character = characters_.find(id);
    if (!character) {
        update.notice = "Unknown agent";
        return update;
    }
    default_agent_id_ = character->id;
    update.state_changed = true;
    update.notice = "Default agent is now " + character->name;
    return update;
}

SessionChange SessionController::request_stop() {
    SessionChange update;
    if (!batch_) {
        update.notice = "No generation is active";
        return update;
    }

    if (!batch_->abort_requested) {
        log_info("Session generation cancellation requested");
        batch_->abort_requested = true;
        registry_.cancel_batch();
        update.state_changed = true;
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

SessionChange SessionController::handle_agent_event(AgentEvent event) {
    SessionChange update;
    std::visit(
        [this, &update](const auto& value) { apply(value, update); },
        event);
    return update;
}

void SessionController::apply(const AgentDelta& event, SessionChange& update) {
    if (!matches(event.request_id) || event.text.empty()) {
        return;
    }
    if (event.kind == CompletionDeltaKind::answer) {
        if (active_->phase != ResponsePhase::answering) {
            transcript_.begin_entry(response_entry(EntryStatus::streaming));
        }
        transcript_.append_answer(active_->response_entry_id, event.text);
        active_->phase = ResponsePhase::answering;
    } else {
        active_->reasoning_text.append(event.text);
        if (active_->phase == ResponsePhase::waiting) {
            active_->phase = ResponsePhase::reasoning;
        }
    }
    update.state_changed = true;
}

void SessionController::apply(const AgentCompleted& event, SessionChange& update) {
    if (!matches(event.request_id)) {
        return;
    }
    if (active_->phase != ResponsePhase::answering) {
        fail_active_response(
            "Agent completed without answer content", active_->agent_id, update);
        finish_batch_run(update);
        return;
    }
    const TranscriptEntry response =
        response_entry(EntryStatus::complete);
    persist(
        request_action(
            "persist completion of",
            event.request_id,
            active_->agent_name),
        [this, &event, &response] {
            journal_.complete_turn(event.request_id, response);
        });
    finish_response_entry(EntryStatus::complete);
    active_.reset();
    update.state_changed = true;
    update.notice = "";
    finish_batch_run(update);
}

void SessionController::apply(const AgentCancelled& event, SessionChange& update) {
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
                active_->agent_name),
            [this, &event, &response] {
                journal_.cancel_turn(event.request_id, response);
            });
        finish_response_entry(EntryStatus::cancelled);
    } else {
        persist(
            request_action(
                "persist cancellation of",
                event.request_id,
                active_->agent_name),
            [this, &event] {
                journal_.cancel_turn(event.request_id, std::nullopt);
            });
    }
    active_.reset();
    update.state_changed = true;
    update.notice = "Generation stopped";
    batch_->stop_notice_recorded = true;
    finish_batch_run(update);
}

void SessionController::apply(const AgentFailed& event, SessionChange& update) {
    if (matches(event.request_id)) {
        fail_active_response(event.message, active_->agent_id, update);
        finish_batch_run(update);
    }
}

void SessionController::fail_active_response(
    std::string message,
    ParticipantId participant_id,
    SessionChange& update) {
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
            active_->agent_name),
        [this, &error] {
            journal_.fail_turn(active_->request_id, error);
        });
    if (active_->phase == ResponsePhase::answering) {
        transcript_.discard_entry(active_->response_entry_id);
    }
    transcript_.add_entry(std::move(error));
    active_.reset();
    update.state_changed = true;
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
    return make_agent_entry(
        active_->response_entry_id,
        active_->agent_id,
        active_->agent_name,
        std::move(text),
        status,
        active_->request_id);
}

bool SessionController::matches(RequestId request_id) const {
    return active_ && active_->request_id == request_id;
}

SessionEventBatch SessionController::receive_events(std::size_t max_events) {
    if (max_events == 0) {
        throw std::invalid_argument("Agent event batch size must be positive");
    }
    SessionChange update;
    if (shutdown_ && !batch_) {
        update.controller_ended = true;
        return {.change = std::move(update)};
    }
    AgentEvent event = AgentCompleted{};
    std::size_t processed = 0;
    while (batch_ && active_ && processed < max_events) {
        const ChannelReadStatus status = registry_.try_receive(
            batch_->foreground_index, event);
        if (status != ChannelReadStatus::value) {
            break;
        }
        merge_change(update, handle_agent_event(std::move(event)));
        ++processed;
    }
    poll_abort_cleanup(update);
    return {
        .change = std::move(update),
        .full = processed == max_events,
    };
}

SessionChange SessionController::receive() {
    return std::move(
        receive_events(std::numeric_limits<std::size_t>::max()).change);
}

void SessionController::shutdown() {
    if (shutdown_) {
        return;
    }
    log_info("Session controller shutting down");
    shutdown_ = true;
    if (batch_) {
        batch_->abort_requested = true;
    }
    try {
        registry_.cancel_batch();
        registry_.stop();
        (void)receive();
        if (batch_) {
            registry_.clear_batch();
        }
        active_.reset();
        abandon_batch();
    } catch (...) {
        // `execution_finished` allows a task to issue its final wake after
        // registry stop() returns. Join the pool while the registry and its
        // borrowed notifier are still alive even when terminal persistence
        // fails.
        worker_pool_.stop();
        throw;
    }
    worker_pool_.stop();
}

} // namespace cha
