#include "chat_coordinator.h"

#include "conversation.h"
#include "conversation_file.h"

#include <exception>
#include <utility>
#include <variant>

namespace cha {

ChatCoordinator::ChatCoordinator(
    AgentInfo agent_info,
    ConversationJournal& journal,
    std::atomic_bool& cancellation,
    Conversation& conversation,
    RequestId next_request_id)
  : agent_info_(std::move(agent_info)),
    journal_(journal),
    cancellation_(cancellation),
    conversation_(conversation),
    next_request_id_(next_request_id) {
}

const Conversation& ChatCoordinator::conversation() const {
    return conversation_;
}

const AgentInfo& ChatCoordinator::agent_info() const {
    return agent_info_;
}

bool ChatCoordinator::generating() const {
    return active_.has_value();
}

std::string ChatCoordinator::submit(std::string prompt, CompletionRequestChannel& requests) {
    if (active_) {
        return "Generation in progress; use /stop, Esc, or Ctrl-C";
    }

    const ConversationSnapshot snapshot = conversation_.snapshot();
    CompletionRequest request{
        .request_id = next_request_id_++,
        .agent_id = agent_info_.id,
        .history = snapshot.messages,
        .prompt = std::move(prompt),
    };

    journal_.start_turn(request.request_id, request.agent_id, request.prompt);
    try {
        conversation_.add_message(std::string(user_author), request.prompt);
    } catch (...) {
        journal_.fail_turn(request.request_id, "Failed to add the submitted prompt to the conversation");
        throw;
    }

    cancellation_.store(false, std::memory_order_release);
    active_ = ActiveTurn{.request_id = request.request_id};
    try {
        if (requests.push(std::move(request))) {
            return {};
        }
    } catch (const std::exception& error) {
        CoordinatorUpdate ignored;
        apply(
            AgentFailed{active_->request_id, "Failed to dispatch request: " + std::string(error.what())},
            ignored);
        return "Request could not be dispatched";
    }

    CoordinatorUpdate ignored;
    apply(AgentFailed{active_->request_id, "Request channel is closed"}, ignored);
    return "Request could not be dispatched";
}

void ChatCoordinator::clear() {
    if (active_) {
        return;
    }
    journal_.clear();
    conversation_.clear();
}

void ChatCoordinator::add_system_message(std::string text) {
    ConversationMessage message{std::string(system_author), std::move(text)};
    journal_.append(message);
    conversation_.add_message(std::move(message.author), std::move(message.text));
}

void ChatCoordinator::request_stop() {
    if (active_) {
        cancellation_.store(true, std::memory_order_release);
    }
}

CoordinatorUpdate ChatCoordinator::receive(AgentEventChannel& events) {
    CoordinatorUpdate update;
    AgentEvent event = AgentCompleted{};
    while (true) {
        const ChannelReadStatus status = events.try_get(event);
        if (status == ChannelReadStatus::empty) {
            break;
        }
        if (status == ChannelReadStatus::closed) {
            update.channel_closed = true;
            break;
        }
        std::visit([this, &update](const auto& value) { apply(value, update); }, event);
    }
    return update;
}

void ChatCoordinator::shutdown(CompletionRequestChannel& requests) {
    if (active_) {
        cancellation_.store(true, std::memory_order_release);
    }
    requests.close();
}

void ChatCoordinator::apply(const AgentDelta& event, CoordinatorUpdate& update) {
    if (!matches(event.request_id) || event.text.empty()) {
        return;
    }
    if (!active_->message_open) {
        conversation_.begin_message(agent_info_.name);
        active_->message_open = true;
    }
    conversation_.append_to_message(event.text);
    active_->response += event.text;
    update.render_needed = true;
}

void ChatCoordinator::apply(const AgentCompleted& event, CoordinatorUpdate& update) {
    if (!matches(event.request_id)) {
        return;
    }
    journal_.complete_turn(event.request_id, agent_info_.name, active_->response);
    finish_response_message();
    active_.reset();
    cancellation_.store(false, std::memory_order_release);
    update.render_needed = true;
    update.notice = "";
}

void ChatCoordinator::apply(const AgentCancelled& event, CoordinatorUpdate& update) {
    if (!matches(event.request_id)) {
        return;
    }
    journal_.cancel_turn(event.request_id, agent_info_.name, active_->response);
    finish_response_message();
    active_.reset();
    cancellation_.store(false, std::memory_order_release);
    update.render_needed = true;
    update.notice = "Generation stopped";
}

void ChatCoordinator::apply(const AgentFailed& event, CoordinatorUpdate& update) {
    if (!matches(event.request_id)) {
        return;
    }
    journal_.fail_turn(event.request_id, event.message);
    if (active_->message_open) {
        conversation_.discard_message();
    }
    conversation_.add_message(std::string(system_author), "Error: " + event.message);
    active_.reset();
    cancellation_.store(false, std::memory_order_release);
    update.render_needed = true;
    update.notice = "Generation failed";
}

void ChatCoordinator::finish_response_message() {
    if (active_->message_open) {
        conversation_.finish_message();
    } else {
        conversation_.add_message(agent_info_.name, {});
    }
}

bool ChatCoordinator::matches(RequestId request_id) const {
    return active_ && active_->request_id == request_id;
}

} // namespace cha
