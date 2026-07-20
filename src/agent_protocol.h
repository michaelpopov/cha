#pragma once

#include "conversation.h"
#include "event_channel.h"
#include "request_id.h"

#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cha {

// Captures all state needed for one completion so the worker never reads mutable conversation state.
struct CompletionRequest {
    RequestId request_id{};
    std::string agent_id;
    std::vector<ConversationEntry> history;
    ConversationEntry prompt;
};

// Validates the target and typed prompt invariants of one immutable completion request.
void validate_completion_request(
    const CompletionRequest& request,
    std::string_view expected_agent_id);

// Carries one streamed response fragment for an identified request.
struct AgentDelta {
    RequestId request_id{};
    std::string text;
};

// Marks successful completion of an identified request.
struct AgentCompleted {
    RequestId request_id{};
};

// Marks cancellation of an identified request, possibly after response fragments were emitted.
struct AgentCancelled {
    RequestId request_id{};
};

// Reports a terminal inference failure for an identified request.
struct AgentFailed {
    RequestId request_id{};
    std::string message;
};

using AgentEvent = std::variant<AgentDelta, AgentCompleted, AgentCancelled, AgentFailed>;
using CompletionRequestChannel = EventChannel<CompletionRequest>;
using AgentEventChannel = EventChannel<AgentEvent>;

} // namespace cha
