#pragma once

#include "agents/agent_info.h"
#include "agents/agent_protocol.h"
#include "conversation/response_content.h"

#include <atomic>
#include <functional>
#include <string>

namespace cha {

// Classifies the terminal transport result of one completion request.
enum class CompletionOutcome {
    completed,
    cancelled,
    protocol_error,
    transport_error,
};

// Carries a completion outcome and an explanatory failure message when applicable.
struct CompletionResult {
    CompletionOutcome outcome{CompletionOutcome::completed};
    std::string message;
};

// Owns all backend-defined data needed after conversation request preparation.
struct RequestPayload {
    std::string bytes;
};

// Receives one semantic transport fragment without attaching request identity.
using CompletionDeltaSink = std::function<void(CompletionDelta)>;

// Defines the synchronous completion operations consumed by the agent execution thread.
class CompletionBackend {
public:
    virtual ~CompletionBackend() = default;

    [[nodiscard]] virtual RequestPayload prepare(
        const CompletionRequest& request,
        const ConversationReadView& conversation) = 0;
    [[nodiscard]] virtual CompletionResult perform(
        RequestPayload payload,
        const CompletionDeltaSink& on_delta,
        const std::atomic_bool& cancellation) = 0;
    [[nodiscard]] virtual AgentInfo info() const = 0;
    [[nodiscard]] virtual const std::string& agent_id() const = 0;
};

} // namespace cha
