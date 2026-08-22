#pragma once

#include "agents/model_context.h"
#include "agents/generation_event.h"
#include "chat/character.h"

#include <atomic>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace cha {

// Classifies the terminal transport result of one model request.
enum class GenerationOutcome {
    completed,
    cancelled,
    protocol_error,
    transport_error,
};

// Token counts reported by a provider for one completed generation. A provider
// may omit either count, especially when a request did not finish normally.
struct GenerationTokenUsage {
    std::optional<std::size_t> input_tokens;
    std::optional<std::size_t> output_tokens;
    std::optional<std::size_t> cache_read_tokens;
};

// How one call to ModelBackend::perform() ended. The message explains the failure outcomes
// and is meant to reach the persona unchanged.
struct GenerationResult {
    GenerationOutcome outcome{GenerationOutcome::completed};
    std::string message;
    GenerationTokenUsage usage;
};

// The request a backend built for itself, opaque to its caller. Preparation
// consumes immutable input; performing the slow call is a separate step.
struct RequestPayload {
    std::string bytes;
    // Optional Responses session_id header; omitted when empty.
    std::optional<std::string> session_id;
};

// Receives one semantic transport fragment without attaching request identity.
using GenerationDeltaSink = std::function<void(GenerationDelta)>;

// The provider boundary of the generation runtime: all one execution needs to answer one request,
// with no knowledge of HTTP, JSON, or any particular vendor. An implementation builds a payload
// from GenerationRequest, then performs a single synchronous generation,
// reporting output through the delta sink and stopping when the cancellation
// flag is set. Tests supply their own implementation instead of reaching the
// network.
class ModelBackend {
public:
    virtual ~ModelBackend() = default;

    virtual RequestPayload prepare(const GenerationRequest& input) = 0;
    virtual GenerationResult perform(
        RequestPayload payload,
        const GenerationDeltaSink& on_delta,
        const std::atomic_bool& cancellation) = 0;
};

} // namespace cha
