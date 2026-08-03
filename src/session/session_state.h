#pragma once

#include "agents/agent.h"
#include "session/generation_status.h"
#include "transcript/transcript.h"

#include <cstddef>
#include <optional>
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

} // namespace cha
