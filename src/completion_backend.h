#pragma once

#include "agent_info.h"
#include "agent_protocol.h"

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

// Receives one transport-level text fragment without attaching worker request identity.
using CompletionDeltaSink = std::function<void(std::string)>;

// Defines the synchronous completion operations consumed by an AgentWorker.
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
