#pragma once

#include "chat/transcript.h"

#include <string>
#include <variant>

namespace cha {

// One semantic provider fragment. Request identity is attached when a batch
// publishes a CompletionEventDelta.
enum class CompletionDeltaKind {
    reasoning,
    answer,
};

struct CompletionDelta {
    CompletionDeltaKind kind{CompletionDeltaKind::answer};
    std::string text;
};

struct CompletionEventDelta {
    RequestId request_id{};
    CompletionDeltaKind kind{CompletionDeltaKind::answer};
    std::string text;
};

struct CompletionCompleted {
    RequestId request_id{};
};

struct CompletionCancelled {
    RequestId request_id{};
};

struct CompletionFailed {
    RequestId request_id{};
    std::string message;
};

using CompletionEvent = std::variant<
    CompletionEventDelta,
    CompletionCompleted,
    CompletionCancelled,
    CompletionFailed>;

} // namespace cha
