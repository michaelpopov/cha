#pragma once

#include "conversation.h"
#include "event_channel.h"
#include "request_id.h"

#include <string>
#include <string_view>
#include <variant>

namespace cha {

// Carries the new prompt and correlation data for one completion request.
struct CompletionRequest {
    RequestId request_id{};
    std::size_t conversation_revision{};
    ConversationEntry prompt;
};

// Validates the target and typed prompt invariants of one completion request.
void validate_completion_request(
    const CompletionRequest& request,
    std::string_view expected_agent_id);

// Validates that a locked conversation is the exact context queued for a request.
void validate_completion_context(
    const CompletionRequest& request,
    const ConversationReadView& conversation);

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
