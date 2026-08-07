#pragma once

#include "agents/agent.h"
#include "session/generation_status.h"
#include "transcript/transcript.h"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cha {

// An owning, owner-thread-produced read model of one live controller. It is
// the transport-neutral boundary for consumers that cannot borrow Transcript.
struct SessionState {
    std::vector<CharacterInfo> characters;
    ParticipantId default_agent_id;
    std::vector<TranscriptEntry> transcript;
    std::size_t revision{};
    std::optional<EntryId> open_entry_id;
    std::size_t history_epoch{};
    GenerationStatus generation;

    bool operator==(const SessionState&) const = default;
};

// A transport-neutral target for text that was proven to extend one live
// controller value. The sole frontend serializes these values directly.
struct EntryTextTarget {
    EntryId entry_id{};
    bool operator==(const EntryTextTarget&) const = default;
};

struct ReasoningTextTarget {
    RequestId request_id{};
    bool operator==(const ReasoningTextTarget&) const = default;
};

using SessionTextTarget = std::variant<EntryTextTarget, ReasoningTextTarget>;

struct SessionTextAppend {
    SessionTextTarget target;
    std::string text;
    bool operator==(const SessionTextAppend&) const = default;
};

// Compact owner-thread continuity proof. It deliberately retains no
// transcript text, reasoning text, characters, or presentation state.
struct SessionStateCursor {
    std::size_t revision{};
    std::size_t history_epoch{};
    std::size_t entry_count{};
    std::optional<EntryId> open_entry_id;
    ParticipantId default_agent_id;
    std::optional<RequestId> request_id;
    ResponsePhase phase{ResponsePhase::waiting};
    std::size_t reasoning_length{};
    std::size_t answer_length{};
    bool operator==(const SessionStateCursor&) const = default;
};

struct SessionAppendProjection {
    SessionTextAppend append;
    SessionStateCursor cursor;
};

// Builds a cursor only for a well-formed, currently appendable core state.
// The caller keeps it beside a published presentation snapshot, never as a
// second owning transcript copy.
[[nodiscard]] std::optional<SessionStateCursor> session_state_cursor(
    const SessionState& state);

// The generation facts an append proof reads. Agent identity is deliberately
// absent: it cannot change without also changing the active request, which the
// proof already compares.
struct AppendGenerationView {
    bool active{};
    std::optional<RequestId> request_id;
    ResponsePhase phase{ResponsePhase::waiting};
    std::string_view reasoning_text;
};

// The consuming half of the cursor contract whose producing half is
// session_state_cursor(). It is pure, so it is tested directly rather than
// through a live controller. Any ambiguity returns no append, which obliges
// the caller to publish a full replacement instead.
[[nodiscard]] std::optional<SessionAppendProjection> session_text_append_since(
    const TranscriptView& transcript,
    const AppendGenerationView& generation,
    const ParticipantId& default_agent_id,
    const SessionStateCursor& cursor);

} // namespace cha
