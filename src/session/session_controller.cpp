#include "session/session_controller.h"

#include <exception>
#include <sstream>
#include <stdexcept>
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

std::string format_handle_notice(
    std::string_view handle,
    const HandleResolution& resolution,
    const ForumPersonas& personas) {
    if (resolution.match == HandleMatch::unknown) {
        return "Unknown agent @" + std::string(handle)
            + ". Personas in this forum: " + personas.handle_list();
    }
    std::string result =
        "Ambiguous agent @" + std::string(handle) + ": matches ";
    for (std::size_t i = 0; i < resolution.candidates.size(); ++i) {
        if (i) {
            result += ", ";
        }
        result += "@" + resolution.candidates[i]->name;
    }
    return result + ". Type more of the name.";
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
    SessionRestore restored) {
    return std::unique_ptr<SessionController>(new SessionController(
        std::move(backends),
        std::move(database_path),
        notifier,
        std::move(restored)));
}

SessionController::SessionController(
    std::vector<AgentDefinition> definitions,
    std::filesystem::path path,
    WakeNotifier& notifier,
    SessionRestore restored)
    : journal_(std::move(path)),
      registry_(transcript_, std::move(definitions), notifier),
      personas_(make_forum_personas(registry_.runtime_info())),
      default_agent_id_(personas_.first().id) {
    initialize(std::move(restored));
}

SessionController::SessionController(
    std::vector<std::unique_ptr<CompletionBackend>> backends,
    std::filesystem::path path,
    WakeNotifier& notifier,
    SessionRestore restored)
    : journal_(std::move(path)),
      registry_(transcript_, std::move(backends), notifier),
      personas_(make_forum_personas(registry_.runtime_info())),
      default_agent_id_(personas_.first().id) {
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
    return {
        .active = active_.has_value(),
        .agent_name = active_ ? active_->agent_name : "",
        .phase = active_ ? active_->phase : ResponsePhase::waiting,
        .reasoning_text = active_ ? active_->reasoning_text : "",
    };
}

SessionUpdate SessionController::busy_notice() const {
    return {.notice = std::string(generation_in_progress_notice)};
}

SessionUpdate SessionController::submit_prompt(
    std::string text,
    std::string handle) {
    if (active_) {
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
            update.notice = format_handle_notice(
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

    update.clear_input = true;
    merge_update(update, start_response(std::move(text), *target));
    return update;
}

SessionUpdate SessionController::start_response(
    std::string text,
    const PersonaInfo& target) {
    SessionUpdate update;
    const RequestId request_id = next_request_id_++;
    CompletionRequest request{
        .request_id = request_id,
        .prompt = make_human_entry(
            next_entry_id_++, target.id, target.name, std::move(text), request_id),
    };
    persist(
        request_action(
            "persist start of",
            request.request_id,
            target.name),
        [this, &request] {
            journal_.start_turn(request.request_id, request.prompt);
        });
    try {
        transcript_.add_entry(request.prompt);
    } catch (...) {
        TranscriptEntry error = make_error_entry(
            next_entry_id_++,
            "Failed to add the submitted prompt to the transcript",
            request.request_id,
            target.id);
        persist(
            request_action(
                "persist failure of",
                request.request_id,
                target.name),
            [this, &request, &error] {
                journal_.fail_turn(request.request_id, error);
            });
        throw;
    }
    active_ = ActiveResponse{
        .request_id = request.request_id,
        .response_entry_id = next_entry_id_++,
        .agent_id = target.id,
        .agent_name = target.name,
        .phase = ResponsePhase::waiting,
    };
    if (registry_.submit(std::move(request))) {
        update.render_needed = true;
        update.notice = "";
        return update;
    }
    fail_active_response("Agent execution is unavailable", target.id, update);
    update.notice = "Request could not be dispatched";
    return update;
}

SessionUpdate SessionController::clear_transcript() {
    if (active_) {
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
    if (active_) {
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
    if (active_) {
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
    if (active_) {
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

SessionUpdate SessionController::session_information() {
    if (active_) {
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
    if (active_) {
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
    if (active_) {
        return busy_notice();
    }
    SessionUpdate update{.clear_input = true};
    if (handle.empty()) {
        update.notice = "Usage: /@AgentName";
        return update;
    }
    const HandleResolution result = personas_.resolve_handle(handle);
    if (result.match != HandleMatch::resolved) {
        update.notice = format_handle_notice(handle, result, personas_);
        return update;
    }
    default_agent_id_ = result.persona->id;
    update.notice = "Default agent is now " + result.persona->name;
    return update;
}

SessionUpdate SessionController::request_stop() {
    SessionUpdate update;
    if (!active_) {
        update.notice = "No generation is active";
        return update;
    }
    registry_.cancel();
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
}

void SessionController::apply(const AgentFailed& event, SessionUpdate& update) {
    if (matches(event.request_id)) {
        fail_active_response(event.message, active_->agent_id, update);
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
        const TranscriptReadView view = transcript_.read();
        const std::span<const TranscriptEntry> entries = view.entries();
        if (!view.open_entry_id()
            || *view.open_entry_id() != active_->response_entry_id
            || entries.empty()
            || entries.back().id != active_->response_entry_id) {
            throw std::logic_error(
                "Active response does not match the open transcript entry");
        }
        text = entries.back().text;
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
    return update;
}

void SessionController::shutdown() {
    if (shutdown_) {
        return;
    }
    shutdown_ = true;
    registry_.stop();
    (void)receive();
}

} // namespace cha
