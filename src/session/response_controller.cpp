#include "session/response_controller.h"

#include <exception>
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

} // namespace

ResponseController::ResponseController(
    Conversation& conversation,
    ConversationJournal& journal,
    AgentRegistry& registry)
    : conversation_(conversation),
      journal_(journal),
      registry_(registry) {}

void ResponseController::restore(ConversationRestore restored) {
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

GenerationStatus ResponseController::generation_status() const {
    return {
        active_.has_value(),
        active_ ? active_->agent_name : "",
        active_ ? active_->phase : ResponsePhase::waiting,
    };
}

ResponseUpdate ResponseController::start(
    std::string text,
    const AgentInfo& target) {
    ResponseUpdate update;
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
        conversation_.add_entry(request.prompt);
    } catch (...) {
        ConversationEntry error = make_error_entry(
            next_entry_id_++,
            "Failed to add the submitted prompt to the conversation",
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

ResponseUpdate ResponseController::apply(AgentEvent event) {
    ResponseUpdate update;
    std::visit(
        [this, &update](const auto& value) { apply(value, update); },
        event);
    return update;
}

void ResponseController::apply(const AgentDelta& event, ResponseUpdate& update) {
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

void ResponseController::apply(const AgentCompleted& event, ResponseUpdate& update) {
    if (!matches(event.request_id)) {
        return;
    }
    if (active_->phase != ResponsePhase::answering) {
        fail_active_response(
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

void ResponseController::apply(const AgentCancelled& event, ResponseUpdate& update) {
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

void ResponseController::apply(const AgentFailed& event, ResponseUpdate& update) {
    if (matches(event.request_id)) {
        fail_active_response(event.message, active_->agent_id, update);
    }
}

void ResponseController::fail_active_response(
    std::string message,
    ParticipantId participant_id,
    ResponseUpdate& update) {
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

void ResponseController::finish_response_entry(CompletionStatus status) {
    conversation_.finish_entry(active_->response_entry_id, status);
}

ConversationEntry ResponseController::response_entry(CompletionStatus status) const {
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

bool ResponseController::matches(RequestId request_id) const {
    return active_ && active_->request_id == request_id;
}

} // namespace cha
