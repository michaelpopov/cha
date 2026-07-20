#pragma once

#include "agent_info.h"
#include "agent_protocol.h"

#include <atomic>
#include <optional>
#include <string>

namespace cha {

class Conversation;
class ConversationJournal;

// Summarizes coordinator work that may require rendering or a status-line update.
struct CoordinatorUpdate {
    bool render_needed{};
    bool channel_closed{};
    std::optional<std::string> notice;
};

// Serializes chat turns and is the sole owner of transcript and journal mutations during a run.
class ChatCoordinator {
public:
    ChatCoordinator(
        AgentInfo agent_info,
        ConversationJournal& journal,
        std::atomic_bool& cancellation,
        Conversation& conversation,
        RequestId next_request_id = 1);

    [[nodiscard]] const Conversation& conversation() const;
    [[nodiscard]] const AgentInfo& agent_info() const;
    [[nodiscard]] bool generating() const;

    [[nodiscard]] std::string submit(std::string prompt, CompletionRequestChannel& requests);
    void clear();
    void add_system_message(std::string text);
    void request_stop();
    CoordinatorUpdate receive(AgentEventChannel& events);
    void shutdown(CompletionRequestChannel& requests);

private:
    // Tracks the exact response message associated with the only active single-agent turn.
    struct ActiveTurn {
        RequestId request_id{};
        std::string response;
        bool message_open{};
    };

    void apply(const AgentDelta& event, CoordinatorUpdate& update);
    void apply(const AgentCompleted& event, CoordinatorUpdate& update);
    void apply(const AgentCancelled& event, CoordinatorUpdate& update);
    void apply(const AgentFailed& event, CoordinatorUpdate& update);
    void finish_response_message();
    [[nodiscard]] bool matches(RequestId request_id) const;

    AgentInfo agent_info_;
    ConversationJournal& journal_;
    std::atomic_bool& cancellation_;
    Conversation& conversation_;
    RequestId next_request_id_;
    std::optional<ActiveTurn> active_;
};

} // namespace cha
