#pragma once

#include "chat/transcript.h"

#include <string>
#include <variant>

namespace cha {

// One semantic provider fragment. Request identity is attached when a provider
// request publishes a GenerationEventDelta.
enum class GenerationDeltaKind {
    reasoning,
    answer,
};

struct GenerationDelta {
    GenerationDeltaKind kind{GenerationDeltaKind::answer};
    std::string text;
};

struct GenerationEventDelta {
    RequestId request_id{};
    GenerationDeltaKind kind{GenerationDeltaKind::answer};
    std::string text;
};

struct GenerationCompleted {
    RequestId request_id{};
};

struct GenerationCancelled {
    RequestId request_id{};
};

struct GenerationFailed {
    RequestId request_id{};
    std::string message;
};

using GenerationEvent = std::variant<
    GenerationEventDelta,
    GenerationCompleted,
    GenerationCancelled,
    GenerationFailed>;

} // namespace cha
