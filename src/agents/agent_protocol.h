#pragma once

#include "conversation/conversation.h"
#include "agents/event_channel.h"
#include "conversation/request_id.h"
#include "conversation/response_content.h"

#include <string>
#include <variant>

namespace cha {

// Carries the new prompt and correlation data for one completion request.
struct CompletionRequest {
    RequestId request_id{};
    ConversationEntry prompt;
};

// Carries one streamed response fragment for an identified request.
struct AgentDelta {
    RequestId request_id{};
    CompletionDeltaKind kind{CompletionDeltaKind::answer};
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
using AgentEventChannel = EventChannel<AgentEvent>;

} // namespace cha
