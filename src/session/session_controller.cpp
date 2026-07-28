#include "session/session_controller.h"

#include <exception>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <variant>

namespace cha {
namespace {

template<typename Operation>
void persist(std::string action, Operation&& operation) {
    try {
        operation();
    } catch (const std::exception& error) {
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

ForumPersonas make_forum_personas(
    const std::vector<AgentRuntimeInfo>& runtime_info) {
    std::vector<PersonaInfo> personas;
    personas.reserve(runtime_info.size());
    for (const AgentRuntimeInfo& agent : runtime_info) {
        personas.push_back(agent.persona);
    }
    return ForumPersonas(std::move(personas));
}

void merge_update(SessionUpdate& all, SessionUpdate one) {
    all.render_needed = all.render_needed || one.render_needed;
    all.end_session = all.end_session || one.end_session;
    all.clear_input = all.clear_input || one.clear_input;
    if (one.notice) {
        all.notice = std::move(one.notice);
    }
}

std::string format_personas_notice(
    const ForumPersonas& personas,
    const std::vector<AgentRuntimeInfo>& runtime_info,
    const ParticipantId& default_agent_id) {
    std::ostringstream result;
    result << "Personas in this forum (" << personas.all().size()
           << "), * marks the default.";
    result << " Any unambiguous prefix works.";
    for (const AgentRuntimeInfo& agent : runtime_info) {
        result << " | " << (agent.persona.id == default_agent_id ? "* " : "")
               << "@" << agent.persona.name << "  " << agent.model << "  "
               << agent.api << "  "
               << (agent.streaming ? "streaming" : "non-streaming");
    }
    return result.str();
}

std::string format_session_information(
    const Transcript& transcript,
    const ForumPersonas& personas,
    const std::vector<AgentRuntimeInfo>& runtime_info,
    const ParticipantId& default_agent_id) {
    std::ostringstream text;
    text << "Transcript entries: " << transcript.snapshot().entries.size()
         << " | " << format_personas_notice(
             personas, runtime_info, default_agent_id);
    return text.str();
}

} // namespace

std::unique_ptr<SessionController> SessionController::from_definitions(
    std::vector<AgentDefinition> definitions,
    std::filesystem::path database_path,
    WakeNotifier& notifier,
    SessionRestore restored) {
    return std::unique_ptr<SessionController>(new SessionController(
        std::move(definitions),
        std::move(database_path),
        notifier,
        std::move(restored)));
}

std::unique_ptr<SessionController> SessionController::from_backends_for_testing(
    std::vector<std::unique_ptr<CompletionBackend>> backends,
    std::filesystem::path database_path,
    WakeNotifier& notifier,
    SessionRestore restored,
    ActivationHook before_activation) {
    return std::unique_ptr<SessionController>(new SessionController(
        std::move(backends),
        std::move(database_path),
        notifier,
        std::move(restored),
        std::move(before_activation)));
}

SessionController::SessionController(
    std::vector<AgentDefinition> definitions,
    std::filesystem::path path,
    WakeNotifier& notifier,
    SessionRestore restored)
    : journal_(std::move(path)),
      registry_(std::move(definitions), notifier),
      personas_(make_forum_personas(registry_.runtime_info())),
      default_agent_id_(personas_.first().id) {
    initialize(std::move(restored));
}

SessionController::SessionController(
    std::vector<std::unique_ptr<CompletionBackend>> backends,
    std::filesystem::path path,
    WakeNotifier& notifier,
    SessionRestore restored,
    ActivationHook before_activation)
    : journal_(std::move(path)),
      registry_(std::move(backends), notifier),
      personas_(make_forum_personas(registry_.runtime_info())),
      default_agent_id_(personas_.first().id),
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
    if (batch_ && batch_->abort_requested) {
        const RunSpec& run = batch_->runs[batch_->foreground_index];
        return {
            .active = true,
            .agent_name = run.target.name,
            .phase = ResponsePhase::stopping,
        };
    }
    return {
        .active = busy(),
        .agent_name = active_ ? active_->agent_name : "",
        .phase = active_ ? active_->phase : ResponsePhase::waiting,
        .reasoning_text = active_ ? active_->reasoning_text : "",
    };
}

bool SessionController::busy() const {
    return active_ || batch_;
}

SessionUpdate SessionController::busy_notice() const {
    return {.notice = std::string(generation_in_progress_notice)};
}

SessionUpdate SessionController::submit_prompt(
    std::string text,
    std::string handle) {
    if (busy()) {
        return busy_notice();
    }
    if (text.empty() && handle.empty()) {
        return {};
    }

    SessionUpdate update;
    const PersonaInfo* target = nullptr;
    if (handle.empty()) {
        target = personas_.find(default_agent_id_);
    } else {
        const HandleResolution resolution = personas_.resolve_handle(handle);
        if (resolution.match != HandleMatch::resolved) {
            update.notice = format_handle_resolution_notice(
                handle, resolution, personas_);
            return update;
        }
        target = resolution.persona;
    }
    if (!target) {
        throw std::logic_error("Default agent is not among the forum personas");
    }
    if (text.empty()) {
        update.notice = "Prompt for @" + target->name + " is empty";
        return update;
    }

    SharedCompletionHistory history =
        std::make_shared<const CompletionHistory>(
            transcript_.completion_history());
    update.clear_input = true;
    start_batch(
        std::move(text),
        std::vector<PersonaInfo>{*target},
        std::move(history),
        update);
    return update;
}

void SessionController::start_batch(
    std::string text,
    std::vector<PersonaInfo> targets,
    SharedCompletionHistory history,
    SessionUpdate& update) {
    if (!history || targets.empty()) {
        throw std::invalid_argument(
            "Response batch requires history and at least one target");
    }
    ResponseBatch batch{
        .history = std::move(history),
    };
    batch.runs.reserve(targets.size());
    for (PersonaInfo& target : targets) {
        batch.runs.push_back({
            .request_id = next_request_id_++,
            .target = std::move(target),
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
        batch.staged_batch_id =
            registry_.stage_batch(std::move(inputs));
    } catch (const std::runtime_error&) {
        update.clear_input = false;
        update.notice = "Request could not be dispatched";
        return;
    }

    batch_ = std::move(batch);
    try {
        activate_current_run(update);
        registry_.open_batch_gate(batch_->staged_batch_id);
    } catch (...) {
        const BatchId batch_id = batch_->staged_batch_id;
        abandon_batch();
        registry_.discard_batch(batch_id);
        throw;
    }
}

void SessionController::activate_current_run(SessionUpdate& update) {
    if (!batch_ || batch_->foreground_index >= batch_->runs.size()) {
        throw std::logic_error("Response batch run index is out of range");
    }
    const RunSpec& run = batch_->runs[batch_->foreground_index];
    TranscriptEntry prompt = make_human_entry(
        next_entry_id_++,
        run.target.id,
        run.target.name,
        run.prompt_text,
        run.request_id);
    ActiveResponse response{
        .request_id = run.request_id,
        .response_entry_id = next_entry_id_++,
        .agent_id = run.target.id,
        .agent_name = run.target.name,
        .phase = ResponsePhase::waiting,
    };

    // Select the still-gated event channel before durable or active state.
    // If the batch or position is stale, teardown remains side-effect free.
    registry_.set_foreground(
        batch_->staged_batch_id,
        batch_->foreground_index);
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
    update.render_needed = true;
    update.notice = "";
}

void SessionController::start_next_batch_run(SessionUpdate& update) {
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
        const BatchId batch_id = batch_->staged_batch_id;
        abandon_batch();
        registry_.discard_batch(batch_id);
        throw;
    }
}

void SessionController::finish_batch_run(SessionUpdate& update) {
    if (!batch_) {
        return;
    }
    if (shutdown_) {
        registry_.retire(
            batch_->staged_batch_id,
            batch_->foreground_index);
        finish_batch(update);
        return;
    }

    if (batch_->abort_requested) {
        append_batch_notice(update);
        registry_.release_foreground_to_cleanup(
            batch_->staged_batch_id,
            batch_->foreground_index);
        poll_abort_cleanup(update);
        return;
    }

    registry_.retire(
        batch_->staged_batch_id,
        batch_->foreground_index);
    if (batch_->foreground_index + 1 == batch_->runs.size()) {
        finish_batch(update);
        return;
    }
    append_batch_notice(update);
    ++batch_->foreground_index;
    start_next_batch_run(update);
}

void SessionController::finish_batch(SessionUpdate& update) {
    if (!batch_) {
        return;
    }
    append_batch_notice(update);
    const std::string terminal_notices = batch_->terminal_notices;
    registry_.retire_batch(batch_->staged_batch_id);
    abandon_batch();
    if (!terminal_notices.empty()) {
        update.notice = terminal_notices;
    }
}

void SessionController::finish_aborted_batch(SessionUpdate& update) {
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
    update.render_needed = true;
    update.notice = std::move(notices);
}

void SessionController::poll_abort_cleanup(SessionUpdate& update) {
    if (!batch_ || !batch_->abort_requested || active_) {
        return;
    }
    if (registry_.poll_abort_cleanup(batch_->staged_batch_id)
        == CleanupStatus::complete) {
        finish_aborted_batch(update);
    }
}

void SessionController::append_batch_notice(const SessionUpdate& update) {
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

SessionUpdate SessionController::clear_transcript() {
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
        .render_needed = true,
        .clear_input = true,
        .notice = "Transcript cleared",
    };
}

SessionUpdate SessionController::open_offrecord() {
    if (busy()) {
        return busy_notice();
    }
    if (!transcript_.open_offrecord(next_entry_id_)) {
        return {.notice = "Already off the record; use /hide-off first"};
    }
    ++next_entry_id_;
    return {
        .render_needed = true,
        .clear_input = true,
    };
}

SessionUpdate SessionController::extend_offrecord() {
    if (busy()) {
        return busy_notice();
    }
    if (!transcript_.extend_offrecord(next_entry_id_)) {
        return {.notice = "No off-record span to extend"};
    }
    ++next_entry_id_;
    return {
        .render_needed = true,
        .clear_input = true,
    };
}

SessionUpdate SessionController::restore_offrecord() {
    if (busy()) {
        return busy_notice();
    }
    if (!transcript_.restore_offrecord(next_entry_id_)) {
        return {.notice = "Nothing to restore"};
    }
    ++next_entry_id_;
    return {
        .render_needed = true,
        .clear_input = true,
    };
}

SessionUpdate SessionController::start_multicast(
    std::string text,
    std::vector<PersonaInfo> targets) {
    if (busy()) {
        return busy_notice();
    }
    if (text.empty()) {
        return {.notice = "Multicast prompt is empty"};
    }
    if (targets.empty()) {
        return {.notice = "Multicast has no targets"};
    }

    std::unordered_set<ParticipantId> ids;
    for (const PersonaInfo& target : targets) {
        const PersonaInfo* known = personas_.find(target.id);
        if (!known || known->name != target.name) {
            return {.notice = "Unknown multicast target @" + target.name};
        }
        if (!ids.insert(target.id).second) {
            return {.notice = format_duplicate_persona_notice(target.name)};
        }
    }
    // Check the precondition on the same atomic snapshot every child receives,
    // avoiding a separate lock and a potentially different observed state.
    SharedCompletionHistory history =
        std::make_shared<const CompletionHistory>(
            transcript_.completion_history());
    if (history->offrecord_span.begin) {
        return {
            .notice =
                "Cannot start multicast while an off-record span is active",
        };
    }

    SessionUpdate update{.clear_input = true};
    start_batch(
        std::move(text),
        std::move(targets),
        std::move(history),
        update);
    return update;
}

SessionUpdate SessionController::session_information() {
    if (busy()) {
        return busy_notice();
    }
    return {
        .render_needed = true,
        .clear_input = true,
        .notice = format_session_information(
            transcript_,
            personas_,
            registry_.runtime_info(),
            default_agent_id_),
    };
}

SessionUpdate SessionController::agent_information() {
    if (busy()) {
        return busy_notice();
    }
    return {
        .render_needed = true,
        .clear_input = true,
        .notice = format_personas_notice(
            personas_, registry_.runtime_info(), default_agent_id_),
    };
}

SessionUpdate SessionController::set_default_agent(std::string_view handle) {
    if (busy()) {
        return busy_notice();
    }
    SessionUpdate update{.clear_input = true};
    if (handle.empty()) {
        update.notice = "Usage: /@AgentName";
        return update;
    }
    const HandleResolution result = personas_.resolve_handle(handle);
    if (result.match != HandleMatch::resolved) {
        update.notice = format_handle_resolution_notice(handle, result, personas_);
        return update;
    }
    default_agent_id_ = result.persona->id;
    update.notice = "Default agent is now " + result.persona->name;
    return update;
}

SessionUpdate SessionController::request_stop() {
    SessionUpdate update;
    if (!batch_) {
        update.notice = "No generation is active";
        return update;
    }

    if (!batch_->abort_requested) {
        batch_->abort_requested = true;
        std::optional<std::size_t> retained_foreground;
        if (active_) {
            retained_foreground = batch_->foreground_index;
        }
        registry_.begin_abort_cleanup(
            batch_->staged_batch_id,
            retained_foreground);
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

SessionUpdate SessionController::handle_agent_event(AgentEvent event) {
    SessionUpdate update;
    std::visit(
        [this, &update](const auto& value) { apply(value, update); },
        event);
    return update;
}

void SessionController::apply(const AgentDelta& event, SessionUpdate& update) {
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
    update.render_needed = true;
}

void SessionController::apply(const AgentCompleted& event, SessionUpdate& update) {
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
    update.render_needed = true;
    update.notice = "";
    finish_batch_run(update);
}

void SessionController::apply(const AgentCancelled& event, SessionUpdate& update) {
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
    update.render_needed = true;
    update.notice = "Generation stopped";
    batch_->stop_notice_recorded = true;
    finish_batch_run(update);
}

void SessionController::apply(const AgentFailed& event, SessionUpdate& update) {
    if (matches(event.request_id)) {
        fail_active_response(event.message, active_->agent_id, update);
        finish_batch_run(update);
    }
}

void SessionController::fail_active_response(
    std::string message,
    ParticipantId participant_id,
    SessionUpdate& update) {
    TranscriptEntry error = make_error_entry(
        next_entry_id_++,
        std::move(message),
        active_->request_id,
        std::move(participant_id));
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
    update.render_needed = true;
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

SessionUpdate SessionController::receive() {
    SessionUpdate update;
    AgentEvent event = AgentCompleted{};
    while (true) {
        const ChannelReadStatus status = registry_.try_receive(event);
        if (status == ChannelReadStatus::empty) {
            break;
        }
        if (status == ChannelReadStatus::closed) {
            update.end_session = true;
            break;
        }
        merge_update(update, handle_agent_event(std::move(event)));
    }
    poll_abort_cleanup(update);
    return update;
}

void SessionController::shutdown() {
    if (shutdown_) {
        return;
    }
    shutdown_ = true;
    if (batch_) {
        batch_->abort_requested = true;
    }
    registry_.cancel_all();
    registry_.stop();
    (void)receive();
    if (batch_) {
        registry_.discard_batch(batch_->staged_batch_id);
    }
    active_.reset();
    abandon_batch();
}

} // namespace cha
