#include "chat_coordinator.h"

#include "command.h"
#include "mention.h"

#include <sstream>
#include <utility>

namespace cha {
namespace {
std::vector<std::unique_ptr<CompletionBackend>> one_backend(
    std::unique_ptr<CompletionBackend> backend) {
    std::vector<std::unique_ptr<CompletionBackend>> result;
    result.push_back(std::move(backend));
    return result;
}
void merge_update(CoordinatorUpdate& all, CoordinatorUpdate one) {
    all.render_needed = all.render_needed || one.render_needed;
    all.end_session = all.end_session || one.end_session;
    all.clear_input = all.clear_input || one.clear_input;
    if (one.notice) {
        all.notice = std::move(one.notice);
    }
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

ChatCoordinator::ChatCoordinator(
    AgentDefinition definition,
    std::filesystem::path path,
    ConversationRestore restored)
    : ChatCoordinator(
          std::vector<AgentDefinition>{std::move(definition)},
          std::move(path),
          std::move(restored)) {
}

ChatCoordinator::ChatCoordinator(
    std::unique_ptr<CompletionBackend> backend,
    std::filesystem::path path,
    ConversationRestore restored)
    : ChatCoordinator(
          one_backend(std::move(backend)), std::move(path), std::move(restored)) {
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
        journal_.fail_turn(turn.request_id, turn.error_entry);
        conversation_.add_entry(turn.error_entry);
    }
}
const Conversation& ChatCoordinator::conversation() const { return conversation_; }
bool ChatCoordinator::generating() const { return active_.has_value(); }
GenerationStatus ChatCoordinator::generation_status() const {
    return {active_.has_value(), active_ ? active_->agent_name : ""};
}
bool ChatCoordinator::show_addressing() const { return show_addressing_; }
const AgentRoster& ChatCoordinator::roster() const { return registry_.roster(); }
int ChatCoordinator::notification_fd() const { return registry_.notification_fd(); }

CoordinatorUpdate ChatCoordinator::handle_input(std::string input) {
    CoordinatorUpdate update;
    if (input.empty()) {
        return update;
    }
    const Command command = parse_command(input);
    if (active_) {
        if (command.kind == CommandKind::stop && command.argument.empty()) {
            update = request_stop();
            update.clear_input = true;
            return update;
        }
        update.notice = "Generation in progress; use /stop, Esc, or Ctrl-C";
        return update;
    }
    if (command.kind == CommandKind::text) {
        return submit(std::move(input));
    }
    if (!command.argument.empty() && command.kind != CommandKind::unknown) {
        update.clear_input = true;
        update.notice = "Command does not accept arguments";
        return update;
    }
    update.clear_input = true;
    switch (command.kind) {
    case CommandKind::clear:
        clear();
        update.render_needed = true;
        update.notice = "Conversation cleared";
        break;
    case CommandKind::stop:
        update.notice = "No generation is active";
        break;
    case CommandKind::exit:
        update.end_session = true;
        break;
    case CommandKind::agents:
        add_notice(roster_notice());
        update.render_needed = true;
        update.notice = "";
        break;
    case CommandKind::set_default: {
        if (command.handle.empty()) {
            update.notice = "Usage: /@AgentName";
            break;
        }
        const HandleResolution result = roster().resolve_handle(command.handle);
        if (result.match != HandleMatch::resolved) {
            update.notice = handle_notice(command.handle, result);
            break;
        }
        default_agent_id_ = result.agent->id;
        update.notice = "Default agent is now " + result.agent->name;
        break;
    }
    case CommandKind::info: {
        std::ostringstream text;
        text << "Transcript entries: " << conversation_.snapshot().entries.size()
             << "\n" << roster_notice();
        add_notice(text.str());
        update.render_needed = true;
        update.notice = "";
        break;
    }
    case CommandKind::unknown:
        update.notice =
            "Unknown command. Commands: /clear, /info, /agents, /@Name, /stop, /exit";
        break;
    case CommandKind::text:
        break;
    }
    return update;
}

CoordinatorUpdate ChatCoordinator::submit(std::string input) {
    CoordinatorUpdate update;
    const AddressedPrompt parsed = parse_addressed_prompt(input);
    const AgentInfo* target = nullptr;
    if (parsed.handle.empty()) {
        target = roster().find(default_agent_id_);
    } else {
        const HandleResolution resolution = roster().resolve_handle(parsed.handle);
        if (resolution.match != HandleMatch::resolved) {
            update.notice = handle_notice(parsed.handle, resolution);
            return update;
        }
        target = resolution.agent;
    }
    if (!target) {
        throw std::logic_error("Default agent is not in the current roster");
    }
    if (parsed.text.empty()) {
        update.notice = "Prompt for @" + target->name + " is empty";
        return update;
    }
    update.clear_input = true;
    const RequestId request_id = next_request_id_++;
    CompletionRequest request{
        .request_id = request_id,
        .prompt = make_human_entry(
            next_entry_id_++, target->id, target->name, parsed.text, request_id),
    };
    journal_.start_turn(request.request_id, request.prompt);
    try {
        conversation_.add_entry(request.prompt);
        request.conversation_revision = conversation_.revision();
    } catch (...) {
        ConversationEntry error = make_error_entry(
            next_entry_id_++,
            "Failed to add the submitted prompt to the conversation",
            request.request_id,
            target->id);
        journal_.fail_turn(request.request_id, error);
        throw;
    }
    active_ = ActiveTurn{
        .request_id = request.request_id,
        .response_entry_id = next_entry_id_++,
        .agent_id = target->id,
        .agent_name = target->name,
        .response = {},
        .entry_open = false,
    };
    if (registry_.submit(std::move(request))) {
        update.render_needed = true;
        update.notice = "";
        return update;
    }
    fail_active_turn("Agent worker is closed", target->id, update);
    update.notice = "Request could not be dispatched";
    return update;
}

void ChatCoordinator::clear() {
    journal_.clear();
    conversation_.clear();
    show_addressing_ = roster().agents().size() > 1;
}

void ChatCoordinator::add_notice(std::string text) {
    ConversationEntry entry =
        make_notice_entry(next_entry_id_++, std::move(text));
    journal_.append(entry);
    conversation_.add_entry(std::move(entry));
}

std::string ChatCoordinator::handle_notice(std::string_view handle, const HandleResolution& resolution) const {
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
    result << "\nAny unambiguous prefix works.";
    for (const AgentInfo& agent : roster().agents()) {
        result << "\n" << (agent.id == default_agent_id_ ? "* " : "  ")
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
    registry_.cancel(active_->agent_id);
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
    if (!active_->entry_open) {
        conversation_.begin_entry(response_entry(CompletionStatus::streaming));
        active_->entry_open = true;
    }
    conversation_.append_to_entry(active_->response_entry_id, event.text);
    active_->response += event.text;
    update.render_needed = true;
}

void ChatCoordinator::apply(const AgentCompleted& event, CoordinatorUpdate& update) {
    if (!matches(event.request_id)) {
        return;
    }
    if (active_->response.empty()) {
        fail_active_turn(
            "Agent completed without text content", active_->agent_id, update);
        return;
    }
    journal_.complete_turn(
        event.request_id, response_entry(CompletionStatus::complete));
    finish_response_entry(CompletionStatus::complete);
    active_.reset();
    update.render_needed = true;
    update.notice = "";
}

void ChatCoordinator::apply(const AgentCancelled& event, CoordinatorUpdate& update) {
    if (!matches(event.request_id)) {
        return;
    }
    if (active_->entry_open) {
        journal_.cancel_turn(
            event.request_id, response_entry(CompletionStatus::cancelled));
        finish_response_entry(CompletionStatus::cancelled);
    } else {
        journal_.cancel_turn(event.request_id, std::nullopt);
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
    journal_.fail_turn(active_->request_id, error);
    if (active_->entry_open) {
        conversation_.discard_entry(active_->response_entry_id);
    }
    conversation_.add_entry(std::move(error));
    active_.reset();
    update.render_needed = true;
    update.notice = "Generation failed";
}

void ChatCoordinator::finish_response_entry(CompletionStatus status) {
    if (active_->entry_open) {
        conversation_.finish_entry(active_->response_entry_id, status);
    } else {
        conversation_.add_entry(response_entry(status));
    }
}

ConversationEntry ChatCoordinator::response_entry(CompletionStatus status) const {
    return make_agent_entry(
        active_->response_entry_id,
        active_->agent_id,
        active_->agent_name,
        active_->response,
        status,
        active_->request_id);
}

bool ChatCoordinator::matches(RequestId request_id) const {
    return active_ && active_->request_id == request_id;
}

} // namespace cha
