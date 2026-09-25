#pragma once

#include "chat/transcript.h"

#include <cstdint>
#include <optional>
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
    bool web_search_used{false};
};

struct GenerationCompleted {
    RequestId request_id{};
    std::optional<std::uint64_t> input_tokens;
    std::optional<std::uint64_t> output_tokens;
};

struct GenerationCancelled {
    RequestId request_id{};
    std::optional<std::uint64_t> input_tokens;
    std::optional<std::uint64_t> output_tokens;
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
