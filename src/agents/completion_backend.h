#pragma once

#include "agents/completion_context.h"
#include "agents/completion_event.h"
#include "chat/character.h"

#include <atomic>
#include <functional>
#include <string>

namespace cha {

// Public operational information about one initialized completion backend. It
// is safe to show in diagnostics and never carries connection secrets.
struct CompletionBackendInfo {
    CharacterMetadata character;
    std::string model;
    std::string api;
    bool streaming{};
};

// Classifies the terminal transport result of one completion request.
enum class CompletionOutcome {
    completed,
    cancelled,
    protocol_error,
    transport_error,
};

// How one call to CompletionBackend::perform() ended. The message explains the failure outcomes
// and is meant to reach the persona unchanged.
struct CompletionResult {
    CompletionOutcome outcome{CompletionOutcome::completed};
    std::string message;
};

// The request a backend built for itself, opaque to its caller. Preparation
// consumes immutable input; performing the slow call is a separate step.
struct RequestPayload {
    std::string bytes;
};

// Receives one semantic transport fragment without attaching request identity.
using CompletionDeltaSink = std::function<void(CompletionDelta)>;

// The provider boundary of the completion runtime: all one execution needs to answer one request,
// with no knowledge of HTTP, JSON, or any particular vendor. An implementation builds a payload
// from CompletionInput, then performs a single synchronous completion,
// reporting output through the delta sink and stopping when the cancellation
// flag is set. Tests supply their own implementation instead of reaching the
// network.
class CompletionBackend {
public:
    virtual ~CompletionBackend() = default;

    virtual RequestPayload prepare(const CompletionInput& input) = 0;
    virtual CompletionResult perform(
        RequestPayload payload,
        const CompletionDeltaSink& on_delta,
        const std::atomic_bool& cancellation) = 0;
    virtual CompletionBackendInfo info() const = 0;
};

} // namespace cha
