#pragma once

#include "agents/agent.h"
#include "agents/agent_registry.h"
#include "application/generation_status.h"
#include "conversation/conversation.h"
#include "application/session_database.h"

#include <optional>
#include <string>
#include <string_view>

namespace cha {

// What the caller must do after a ResponseController call: redraw when the transcript changed,
// and show the notice when one is set.
struct ResponseUpdate {
    bool render_needed{};
    std::optional<std::string> notice;
};

// Owns the single in-flight response, keeping turn mechanics out of ChatCoordinator. It starts a
// turn by making the prompt durable and submitting it, then applies each AgentEvent to the live
// Conversation and the ConversationJournal until exactly one terminal state is reached. It drives
// AgentRegistry for submission and cancellation, holds the entry and request ID counters restored
// with a session, and reports the current GenerationStatus.
class ResponseController {
public:
    ResponseController(
        Conversation& conversation,
        ConversationJournal& journal,
        AgentRegistry& registry);

    void restore(ConversationRestore restored);

    GenerationStatus generation_status() const;
    RequestId next_request_id() const { return next_request_id_; }
    EntryId next_entry_id() const { return next_entry_id_; }

    // Starts a response for the already-resolved target agent. On success, notice is set to "".
    ResponseUpdate start(
        std::string text,
        const AgentInfo& target);

    ResponseUpdate apply(AgentEvent event);

private:
    struct ActiveResponse {
        RequestId request_id{};
        EntryId response_entry_id{};
        ParticipantId agent_id;
        std::string agent_name;
        ResponsePhase phase{ResponsePhase::waiting};
    };

    void apply(const AgentDelta& event, ResponseUpdate& update);
    void apply(const AgentCompleted& event, ResponseUpdate& update);
    void apply(const AgentCancelled& event, ResponseUpdate& update);
    void apply(const AgentFailed& event, ResponseUpdate& update);
    void fail_active_response(
        std::string message,
        ParticipantId participant_id,
        ResponseUpdate& update);
    void finish_response_entry(CompletionStatus status);
    ConversationEntry response_entry(CompletionStatus status) const;
    bool matches(RequestId request_id) const;

    Conversation& conversation_;
    ConversationJournal& journal_;
    AgentRegistry& registry_;
    RequestId next_request_id_{1};
    EntryId next_entry_id_{1};
    std::optional<ActiveResponse> active_;
};

} // namespace cha
