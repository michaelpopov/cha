#include "chat_coordinator.h"

#include "command.h"

#include <exception>
#include <sstream>
#include <utility>
#include <variant>

namespace cha {
namespace {

void merge_update(
    CoordinatorUpdate& combined,
    CoordinatorUpdate update) {
    combined.render_needed =
        combined.render_needed || update.render_needed;
    combined.end_session =
        combined.end_session || update.end_session;
    combined.clear_input =
        combined.clear_input || update.clear_input;
    if (update.notice) {
        combined.notice = std::move(update.notice);
    }
}

} // namespace

ChatCoordinator::ChatCoordinator(
    AgentDefinition definition,
    std::filesystem::path database_path,
    ConversationRestore restored)
  : journal_(std::move(database_path)),
    worker_(conversation_, std::move(definition)),
    agent_info_(worker_.info()) {
    initialize(std::move(restored));
}

ChatCoordinator::ChatCoordinator(
    std::unique_ptr<CompletionBackend> backend,
    std::filesystem::path database_path,
    ConversationRestore restored)
  : journal_(std::move(database_path)),
    worker_(conversation_, std::move(backend)),
    agent_info_(worker_.info()) {
    initialize(std::move(restored));
}

ChatCoordinator::~ChatCoordinator() {
    try {
        shutdown();
    } catch (...) {
    }
}

void ChatCoordinator::initialize(ConversationRestore restored) {
    conversation_.replace_entries(std::move(restored.entries));
    next_request_id_ = restored.next_request_id;
    next_entry_id_ = restored.next_entry_id;
    for (const InterruptedTurn& turn : restored.interrupted_turns) {
        journal_.fail_turn(turn.request_id, turn.error_entry);
        conversation_.add_entry(turn.error_entry);
    }
}

const Conversation& ChatCoordinator::conversation() const {
    return conversation_;
}

bool ChatCoordinator::generating() const {
    return active_.has_value();
}

int ChatCoordinator::notification_fd() const {
    return worker_.notification_fd();
}

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
    case CommandKind::info: {
        const std::size_t message_count =
            conversation_.snapshot().entries.size();
        std::ostringstream info;
        info << "Model: " << agent_info_.model << '\n'
             << "API: " << agent_info_.api << '\n'
             << "Streaming: " << (agent_info_.streaming ? "yes" : "no") << '\n'
             << "Transcript entries: " << message_count;
        add_notice(info.str());
        update.render_needed = true;
        update.notice = "";
        break;
    }
    case CommandKind::stop:
        update.notice = "No generation is active";
        break;
    case CommandKind::exit:
        update.end_session = true;
        break;
    case CommandKind::unknown:
        update.notice = "Unknown command. Commands: /clear, /info, /stop, /exit";
        break;
    case CommandKind::text:
        break;
    }
    return update;
}

CoordinatorUpdate ChatCoordinator::submit(std::string prompt) {
    CoordinatorUpdate update;
    update.clear_input = true;
    const RequestId request_id = next_request_id_++;
    CompletionRequest request{
        .request_id = request_id,
        .agent_id = agent_info_.id,
        .prompt = make_human_entry(next_entry_id_++, std::move(prompt), request_id),
    };

    journal_.start_turn(request.request_id, request.agent_id, request.prompt);
    try {
        conversation_.add_entry(request.prompt);
        request.conversation_revision = conversation_.revision();
    } catch (...) {
        const ConversationEntry error = make_error_entry(
            next_entry_id_++,
            "Failed to add the submitted prompt to the conversation",
            request.request_id);
        journal_.fail_turn(request.request_id, error);
        throw;
    }

    active_ = ActiveTurn{
        .request_id = request.request_id,
        .response_entry_id = next_entry_id_++,
    };
    try {
        if (worker_.submit(std::move(request))) {
            update.render_needed = true;
            update.notice = "";
            return update;
        }
    } catch (const std::exception& error) {
        fail_active_turn(
            "Failed to dispatch request: " + std::string(error.what()),
            {},
            update);
        update.notice = "Request could not be dispatched";
        return update;
    }

    fail_active_turn("Agent worker is closed", {}, update);
    update.notice = "Request could not be dispatched";
    return update;
}

void ChatCoordinator::clear() {
    journal_.clear();
    conversation_.clear();
}

void ChatCoordinator::add_notice(std::string text) {
    ConversationEntry entry =
        make_notice_entry(next_entry_id_++, std::move(text));
    journal_.append(entry);
    conversation_.add_entry(std::move(entry));
}

CoordinatorUpdate ChatCoordinator::request_stop() {
    CoordinatorUpdate update;
    if (!active_) {
        update.notice = "No generation is active";
        return update;
    }
    worker_.cancel();
    update.notice = "Stopping generation...";
    return update;
}

CoordinatorUpdate ChatCoordinator::receive() {
    CoordinatorUpdate update;
    AgentEvent event = AgentCompleted{};
    while (true) {
        const ChannelReadStatus status = worker_.try_receive(event);
        if (status == ChannelReadStatus::empty) {
            break;
        }
        if (status == ChannelReadStatus::closed) {
            update.end_session = true;
            break;
        }
        merge_update(
            update,
            handle_agent_event(std::move(event)));
    }
    return update;
}

CoordinatorUpdate ChatCoordinator::handle_agent_event(
    AgentEvent event) {
    CoordinatorUpdate update;
    std::visit(
        [this, &update](const auto& value) {
            apply(value, update);
        },
        event);
    return update;
}

void ChatCoordinator::shutdown() {
    if (shutdown_) {
        return;
    }
    if (active_) {
        worker_.cancel();
    }
    worker_.stop();
    (void)receive();
    shutdown_ = true;
}

void ChatCoordinator::apply(
    const AgentDelta& event,
    CoordinatorUpdate& update) {
    if (!matches(event.request_id) || event.text.empty()) {
        return;
    }
    if (!active_->entry_open) {
        conversation_.begin_entry(
            response_entry(CompletionStatus::streaming));
        active_->entry_open = true;
    }
    conversation_.append_to_entry(
        active_->response_entry_id,
        event.text);
    active_->response += event.text;
    update.render_needed = true;
}

void ChatCoordinator::apply(
    const AgentCompleted& event,
    CoordinatorUpdate& update) {
    if (!matches(event.request_id)) {
        return;
    }
    if (active_->response.empty()) {
        fail_active_turn(
            "Agent completed without text content",
            agent_info_.id,
            update);
        return;
    }
    journal_.complete_turn(
        event.request_id,
        response_entry(CompletionStatus::complete));
    finish_response_entry(CompletionStatus::complete);
    active_.reset();
    update.render_needed = true;
    update.notice = "";
}

void ChatCoordinator::apply(
    const AgentCancelled& event,
    CoordinatorUpdate& update) {
    if (!matches(event.request_id)) {
        return;
    }
    if (active_->entry_open) {
        journal_.cancel_turn(
            event.request_id,
            response_entry(CompletionStatus::cancelled));
        finish_response_entry(CompletionStatus::cancelled);
    } else {
        journal_.cancel_turn(event.request_id, std::nullopt);
    }
    active_.reset();
    update.render_needed = true;
    update.notice = "Generation stopped";
}

void ChatCoordinator::apply(
    const AgentFailed& event,
    CoordinatorUpdate& update) {
    if (!matches(event.request_id)) {
        return;
    }
    fail_active_turn(event.message, agent_info_.id, update);
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

ConversationEntry ChatCoordinator::response_entry(
    CompletionStatus status) const {
    return make_agent_entry(
        active_->response_entry_id,
        agent_info_.id,
        agent_info_.name,
        active_->response,
        status,
        active_->request_id);
}

bool ChatCoordinator::matches(RequestId request_id) const {
    return active_ && active_->request_id == request_id;
}

} // namespace cha
