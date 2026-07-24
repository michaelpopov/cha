#pragma once

#include "agents/agent_protocol.h"
#include "agents/agent_registry.h"
#include "agents/agent_roster.h"
#include "application/generation_status.h"
#include "conversation/conversation.h"
#include "storage/session_database.h"

#include <optional>
#include <string>
#include <string_view>

namespace cha {

struct ResponseUpdate {
    bool render_needed{};
    std::optional<std::string> notice;
};

// Owns the single in-flight agent response: conversation/journal mutations and event application.
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
