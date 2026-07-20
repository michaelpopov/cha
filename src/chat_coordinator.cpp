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
    RequestId next_request_id,
    EntryId next_entry_id)
  : agent_info_(std::move(agent_info)),
    journal_(journal),
    cancellation_(cancellation),
    conversation_(conversation),
    next_request_id_(next_request_id),
    next_entry_id_(next_entry_id) {
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
    const RequestId request_id = next_request_id_++;
    ConversationEntry prompt_entry = make_human_entry(next_entry_id_++, std::move(prompt), request_id);
    CompletionRequest request{
        .request_id = request_id,
        .agent_id = agent_info_.id,
        .history = snapshot.entries,
        .prompt = prompt_entry,
    };

    journal_.start_turn(request.request_id, request.agent_id, request.prompt);
    try {
        conversation_.add_entry(std::move(prompt_entry));
    } catch (...) {
        const ConversationEntry error = make_error_entry(
            next_entry_id_++,
            "Failed to add the submitted prompt to the conversation",
            request.request_id);
        journal_.fail_turn(request.request_id, error);
        throw;
    }

    cancellation_.store(false, std::memory_order_release);
    active_ = ActiveTurn{
        .request_id = request.request_id,
        .response_entry_id = next_entry_id_++,
    };
    try {
        if (requests.push(std::move(request))) {
            return {};
        }
    } catch (const std::exception& error) {
        CoordinatorUpdate ignored;
        fail_active_turn(
            "Failed to dispatch request: " + std::string(error.what()),
            {},
            ignored);
        return "Request could not be dispatched";
    }

    CoordinatorUpdate ignored;
    fail_active_turn("Request channel is closed", {}, ignored);
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
    ConversationEntry entry = make_notice_entry(next_entry_id_++, std::move(text));
    journal_.append(entry);
    conversation_.add_entry(std::move(entry));
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
    journal_.complete_turn(event.request_id, response_entry(CompletionStatus::complete));
    finish_response_entry(CompletionStatus::complete);
    active_.reset();
    cancellation_.store(false, std::memory_order_release);
    update.render_needed = true;
    update.notice = "";
}

void ChatCoordinator::apply(const AgentCancelled& event, CoordinatorUpdate& update) {
    if (!matches(event.request_id)) {
        return;
    }
    journal_.cancel_turn(event.request_id, response_entry(CompletionStatus::cancelled));
    finish_response_entry(CompletionStatus::cancelled);
    active_.reset();
    cancellation_.store(false, std::memory_order_release);
    update.render_needed = true;
    update.notice = "Generation stopped";
}

void ChatCoordinator::apply(const AgentFailed& event, CoordinatorUpdate& update) {
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
    cancellation_.store(false, std::memory_order_release);
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
