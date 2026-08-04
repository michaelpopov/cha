#pragma once

#include "agents/agent.h"
#include "session/generation_status.h"
#include "transcript/transcript.h"

#include <cstddef>
#include <optional>
#include <string>
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
// controller value. Web maps these targets to its stable wire names.
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

} // namespace cha
