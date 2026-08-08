#pragma once

#include "chat/transcript.h"

#include <optional>
#include <string>
#include <variant>

namespace cha {

// The operation made no externally visible core-state change. It may still
// carry a notice or a lifecycle flag.
struct NoStateUpdate {
    bool operator==(const NoStateUpdate&) const = default;
};

// At least one structural or non-append-only value changed, so an incremental
// text event cannot represent the transition.
struct SnapshotRequired {
    bool operator==(const SnapshotRequired&) const = default;
};

// A transport-neutral identity for text that one operation extended. The sole
// frontend serializes these values directly.
struct EntryTextTarget {
    EntryId entry_id{};
    bool operator==(const EntryTextTarget&) const = default;
};

struct ReasoningTextTarget {
    RequestId request_id{};
    bool operator==(const ReasoningTextTarget&) const = default;
};

using TextTarget = std::variant<EntryTextTarget, ReasoningTextTarget>;

// All externally visible core-state change made by the operation is this
// non-empty suffix, appended to exactly the value named by target. The text is
// owned, so it can safely cross into transport state.
struct TextAppend {
    TextTarget target;
    std::string text;
    bool operator==(const TextAppend&) const = default;
};

using ControllerStateUpdate =
    std::variant<NoStateUpdate, SnapshotRequired, TextAppend>;

// The observable effect of one command or applied generation event, classified by
// the controller at the moment it mutates its own state.
struct ControllerUpdate {
    ControllerStateUpdate state{NoStateUpdate{}};
    bool input_consumed{};
    bool session_ended{};
    // nullopt leaves a frontend's current notice unchanged, an empty string
    // clears it, and a non-empty string replaces it.
    std::optional<std::string> notice;

    bool operator==(const ControllerUpdate&) const = default;
};

// A bounded generation-event drain. A full batch is deliberately conservative: it
// tells an owner loop to try another drain before sleeping, even when the last
// event happened to empty the channel. `full` is queue scheduling information
// and is deliberately kept out of ControllerUpdate.
struct ControllerEventBatch {
    ControllerUpdate update;
    bool full{};
};

[[nodiscard]] inline bool requires_snapshot(
    const ControllerStateUpdate& state) noexcept {
    return std::holds_alternative<SnapshotRequired>(state);
}

[[nodiscard]] inline bool has_state_update(
    const ControllerStateUpdate& state) noexcept {
    return !std::holds_alternative<NoStateUpdate>(state);
}

[[nodiscard]] inline TextAppend* text_append(
    ControllerStateUpdate& state) noexcept {
    return std::get_if<TextAppend>(&state);
}

[[nodiscard]] inline const TextAppend* text_append(
    const ControllerStateUpdate& state) noexcept {
    return std::get_if<TextAppend>(&state);
}

[[nodiscard]] inline bool requires_snapshot(
    const ControllerUpdate& update) noexcept {
    return requires_snapshot(update.state);
}

[[nodiscard]] inline bool has_state_update(
    const ControllerUpdate& update) noexcept {
    return has_state_update(update.state);
}

[[nodiscard]] inline const TextAppend* text_append(
    const ControllerUpdate& update) noexcept {
    return text_append(update.state);
}

// Records a structural mutation. SnapshotRequired dominates every other state
// effect, so a multi-step command helper can call this from the branch that
// performs the mutation without inspecting what was already recorded.
inline void require_snapshot(ControllerUpdate& update) noexcept {
    update.state = SnapshotRequired{};
}

// Combines the effects of two operations applied in this order. Two appends to
// one target concatenate; every mixed or ambiguous combination becomes
// SnapshotRequired.
[[nodiscard]] ControllerStateUpdate merge_state(
    ControllerStateUpdate all,
    ControllerStateUpdate one);

// Merges one later operation into an accumulated update. Lifecycle flags are
// combined with logical OR and the last supplied notice wins, including an
// empty clearing notice.
void merge(ControllerUpdate& all, ControllerUpdate one);

} // namespace cha
