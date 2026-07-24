#include "application/chat_coordinator.h"

#include <exception>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace cha {
namespace {
void merge_update(CoordinatorUpdate& all, CoordinatorUpdate one) {
    all.render_needed = all.render_needed || one.render_needed;
    all.end_session = all.end_session || one.end_session;
    all.clear_input = all.clear_input || one.clear_input;
    if (one.notice) {
        all.notice = std::move(one.notice);
    }
}

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
} // namespace

ChatCoordinator::ChatCoordinator(
    std::vector<AgentDefinition> definitions,
    std::filesystem::path path,
    ConversationRestore restored)
    : journal_(std::move(path)),
      registry_(conversation_, std::move(definitions)),
      default_agent_id_(registry_.roster().first().id) {
    initialize(std::move(restored));
}

ChatCoordinator::ChatCoordinator(
    std::vector<std::unique_ptr<CompletionBackend>> backends,
    std::filesystem::path path,
    ConversationRestore restored)
    : journal_(std::move(path)),
      registry_(conversation_, std::move(backends)),
      default_agent_id_(registry_.roster().first().id) {
    initialize(std::move(restored));
}

ChatCoordinator::~ChatCoordinator() {
    try {
        shutdown();
    } catch (...) {
    }
}

void ChatCoordinator::initialize(ConversationRestore restored) {
    const std::string_view sole = registry_.roster().first().id;
    show_addressing_ = registry_.roster().agents().size() > 1;
    if (!show_addressing_) {
        for (const ConversationEntry& entry : restored.entries) {
            const bool foreign =
                (entry.kind == EntryKind::agent && entry.participant_id != sole)
                || (entry.kind == EntryKind::human && entry.addressed_to != sole);
            if (foreign) {
                show_addressing_ = true;
                break;
            }
        }
    }
    conversation_.replace_entries(std::move(restored.entries));
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
        conversation_.add_entry(turn.error_entry);
    }
}
const Conversation& ChatCoordinator::conversation() const { return conversation_; }
bool ChatCoordinator::generating() const { return active_.has_value(); }
GenerationStatus ChatCoordinator::generation_status() const {
    return {
        active_.has_value(),
        active_ ? active_->agent_name : "",
        active_ ? active_->phase : ResponsePhase::waiting,
    };
}
bool ChatCoordinator::show_addressing() const { return show_addressing_; }
const AgentRoster& ChatCoordinator::roster() const { return registry_.roster(); }
int ChatCoordinator::notification_fd() const { return registry_.notification_fd(); }

CoordinatorUpdate ChatCoordinator::submit_prompt(
    std::string text,
    std::string handle) {
    if (active_) {
        return generation_in_progress();
    }
    if (text.empty() && handle.empty()) {
        return {};
    }
    return submit(std::move(text), std::move(handle));
}

CoordinatorUpdate ChatCoordinator::clear_conversation() {
    if (active_) {
        return generation_in_progress();
    }
    clear();
    return {
        .render_needed = true,
        .clear_input = true,
        .notice = "Conversation cleared",
    };
}

CoordinatorUpdate ChatCoordinator::session_information() {
    if (active_) {
        return generation_in_progress();
    }
    std::ostringstream text;
    text << "Transcript entries: " << conversation_.snapshot().entries.size()
         << " | " << roster_notice();
    return {
        .render_needed = true,
        .clear_input = true,
        .notice = text.str(),
    };
}

CoordinatorUpdate ChatCoordinator::agent_information() {
    if (active_) {
        return generation_in_progress();
    }
    return {
        .render_needed = true,
        .clear_input = true,
        .notice = roster_notice(),
    };
}

CoordinatorUpdate ChatCoordinator::set_default_agent(
    std::string_view handle) {
    if (active_) {
        return generation_in_progress();
    }
    CoordinatorUpdate update{.clear_input = true};
    if (handle.empty()) {
        update.notice = "Usage: /@AgentName";
        return update;
    }
    const HandleResolution result = roster().resolve_handle(handle);
    if (result.match != HandleMatch::resolved) {
        update.notice = handle_notice(handle, result);
        return update;
    }
    default_agent_id_ = result.agent->id;
    update.notice = "Default agent is now " + result.agent->name;
    return update;
}

CoordinatorUpdate ChatCoordinator::submit(
    std::string text,
    std::string handle) {
    CoordinatorUpdate update;
    const AgentInfo* target = nullptr;
    if (handle.empty()) {
        target = roster().find(default_agent_id_);
    } else {
        const HandleResolution resolution = roster().resolve_handle(handle);
        if (resolution.match != HandleMatch::resolved) {
            update.notice = handle_notice(handle, resolution);
            return update;
        }
        target = resolution.agent;
    }
    if (!target) {
        throw std::logic_error("Default agent is not in the current roster");
    }
    if (text.empty()) {
        update.notice = "Prompt for @" + target->name + " is empty";
        return update;
    }
    update.clear_input = true;
    const RequestId request_id = next_request_id_++;
    CompletionRequest request{
        .request_id = request_id,
        .prompt = make_human_entry(
            next_entry_id_++, target->id, target->name, std::move(text), request_id),
    };
    persist(
        request_action(
            "persist start of",
            request.request_id,
            target->name),
        [this, &request] {
            journal_.start_turn(request.request_id, request.prompt);
        });
    try {
        conversation_.add_entry(request.prompt);
    } catch (...) {
        ConversationEntry error = make_error_entry(
            next_entry_id_++,
            "Failed to add the submitted prompt to the conversation",
            request.request_id,
            target->id);
        persist(
            request_action(
                "persist failure of",
                request.request_id,
                target->name),
            [this, &request, &error] {
                journal_.fail_turn(request.request_id, error);
            });
        throw;
    }
    active_ = ActiveTurn{
        .request_id = request.request_id,
        .response_entry_id = next_entry_id_++,
        .agent_id = target->id,
        .agent_name = target->name,
        .phase = ResponsePhase::waiting,
    };
    if (registry_.submit(std::move(request))) {
        update.render_needed = true;
        update.notice = "";
        return update;
    }
    fail_active_turn("Agent execution is unavailable", target->id, update);
    update.notice = "Request could not be dispatched";
    return update;
}

CoordinatorUpdate ChatCoordinator::generation_in_progress() const {
    CoordinatorUpdate update;
    update.notice = std::string(generation_in_progress_notice);
    return update;
}

void ChatCoordinator::clear() {
    persist("persist /clear", [this] { journal_.clear(); });
    conversation_.clear();
    show_addressing_ = roster().agents().size() > 1;
}

std::string ChatCoordinator::handle_notice(
    std::string_view handle,
    const HandleResolution& resolution) const {
    if (resolution.match == HandleMatch::unknown) {
        return "Unknown agent @" + std::string(handle)
            + ". Agents in this room: " + roster().handle_list();
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

std::string ChatCoordinator::roster_notice() const {
    std::ostringstream result;
    result << "Agents in this room (" << roster().agents().size()
           << "), * marks the default.";
    result << " Any unambiguous prefix works.";
    for (const AgentInfo& agent : roster().agents()) {
        result << " | " << (agent.id == default_agent_id_ ? "* " : "")
               << "@" << agent.name << "  " << agent.model << "  "
               << agent.api << "  "
               << (agent.streaming ? "streaming" : "non-streaming");
    }
    return result.str();
}

CoordinatorUpdate ChatCoordinator::request_stop() {
    CoordinatorUpdate update;
    if (!active_) {
        update.notice = "No generation is active";
        return update;
    }
    registry_.cancel();
    update.notice = "Stopping generation...";
    return update;
}

CoordinatorUpdate ChatCoordinator::receive() {
    CoordinatorUpdate update;
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

CoordinatorUpdate ChatCoordinator::handle_agent_event(AgentEvent event) {
    CoordinatorUpdate update;
    std::visit(
        [this, &update](const auto& value) { apply(value, update); }, event);
    return update;
}

void ChatCoordinator::shutdown() {
    if (shutdown_) {
        return;
    }
    registry_.stop();
    (void)receive();
    shutdown_ = true;
}

void ChatCoordinator::apply(const AgentDelta& event, CoordinatorUpdate& update) {
    if (!matches(event.request_id) || event.text.empty()) {
        return;
    }
    if (active_->phase == ResponsePhase::waiting) {
        conversation_.begin_entry(response_entry(CompletionStatus::streaming));
    }
    conversation_.append_to_entry(
        active_->response_entry_id, event.kind, event.text);
    if (event.kind == CompletionDeltaKind::answer) {
        active_->phase = ResponsePhase::answering;
    } else if (active_->phase == ResponsePhase::waiting) {
        active_->phase = ResponsePhase::reasoning;
    }
    update.render_needed = true;
}

void ChatCoordinator::apply(const AgentCompleted& event, CoordinatorUpdate& update) {
    if (!matches(event.request_id)) {
        return;
    }
    if (active_->phase != ResponsePhase::answering) {
        fail_active_turn(
            "Agent completed without answer content", active_->agent_id, update);
        return;
    }
    const ConversationEntry response =
        response_entry(CompletionStatus::complete);
    persist(
        request_action(
            "persist completion of",
            event.request_id,
            active_->agent_name),
        [this, &event, &response] {
            journal_.complete_turn(event.request_id, response);
        });
    finish_response_entry(CompletionStatus::complete);
    active_.reset();
    update.render_needed = true;
    update.notice = "";
}

void ChatCoordinator::apply(const AgentCancelled& event, CoordinatorUpdate& update) {
    if (!matches(event.request_id)) {
        return;
    }
    if (active_->phase == ResponsePhase::answering) {
        const ConversationEntry response =
            response_entry(CompletionStatus::cancelled);
        persist(
            request_action(
                "persist cancellation of",
                event.request_id,
                active_->agent_name),
            [this, &event, &response] {
                journal_.cancel_turn(event.request_id, response);
            });
        finish_response_entry(CompletionStatus::cancelled);
    } else {
        persist(
            request_action(
                "persist cancellation of",
                event.request_id,
                active_->agent_name),
            [this, &event] {
                journal_.cancel_turn(event.request_id, std::nullopt);
            });
        if (active_->phase == ResponsePhase::reasoning) {
            finish_response_entry(CompletionStatus::cancelled);
        }
    }
    active_.reset();
    update.render_needed = true;
    update.notice = "Generation stopped";
}

void ChatCoordinator::apply(const AgentFailed& event, CoordinatorUpdate& update) {
    if (matches(event.request_id)) {
        fail_active_turn(event.message, active_->agent_id, update);
    }
}

void ChatCoordinator::fail_active_turn(
    std::string message,
    ParticipantId participant_id,
    CoordinatorUpdate& update) {
    ConversationEntry error = make_error_entry(
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
    if (active_->phase != ResponsePhase::waiting) {
        conversation_.discard_entry(active_->response_entry_id);
    }
    conversation_.add_entry(std::move(error));
    active_.reset();
    update.render_needed = true;
    update.notice = "Generation failed";
}

void ChatCoordinator::finish_response_entry(CompletionStatus status) {
    conversation_.finish_entry(active_->response_entry_id, status);
}

ConversationEntry ChatCoordinator::response_entry(CompletionStatus status) const {
    std::string text;
    if (active_->phase != ResponsePhase::waiting) {
        const ConversationReadView view = conversation_.read();
        const std::span<const ConversationEntry> entries = view.entries();
        if (!view.open_entry_id()
            || *view.open_entry_id() != active_->response_entry_id
            || entries.empty()
            || entries.back().id != active_->response_entry_id) {
            throw std::logic_error(
                "Active response does not match the open conversation entry");
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

bool ChatCoordinator::matches(RequestId request_id) const {
    return active_ && active_->request_id == request_id;
}

} // namespace cha
