#pragma once

#include "agents/agent.h"

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

// How one call to CompletionBackend::perform() ended. The message explains the failure outcomes
// and is meant to reach the user unchanged.
struct CompletionResult {
    CompletionOutcome outcome{CompletionOutcome::completed};
    std::string message;
};

// The request a backend built for itself, opaque to its caller. It exists so that reading the
// transcript and performing the slow call can be separate steps: prepare() runs under the
// transcript lock, perform() runs without it.
struct RequestPayload {
    std::string bytes;
};

// Receives one semantic transport fragment without attaching request identity.
using CompletionDeltaSink = std::function<void(CompletionDelta)>;

// The provider boundary of the agent runtime: all AgentRegistry needs to answer one request,
// with no knowledge of HTTP, JSON, or any particular vendor. An implementation builds a payload
// from a TranscriptReadView, then performs a single synchronous completion, reporting output
// through the delta sink and stopping when the cancellation flag is set. Tests supply their own
// implementation instead of reaching the network.
class CompletionBackend {
public:
    virtual ~CompletionBackend() = default;

    virtual RequestPayload prepare(
        const CompletionRequest& request,
        const TranscriptReadView& transcript) = 0;
    virtual CompletionResult perform(
        RequestPayload payload,
        const CompletionDeltaSink& on_delta,
        const std::atomic_bool& cancellation) = 0;
    virtual AgentInfo info() const = 0;
    virtual const std::string& agent_id() const = 0;
};

} // namespace cha
