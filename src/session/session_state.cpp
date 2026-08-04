#include "session/session_state.h"

namespace cha {

std::optional<SessionStateCursor> session_state_cursor(const SessionState& state) {
    if (!state.generation.active || !state.generation.request_id) return std::nullopt;
    SessionStateCursor cursor{
        .revision = state.revision,
        .history_epoch = state.history_epoch,
        .entry_count = state.transcript.size(),
        .open_entry_id = state.open_entry_id,
        .default_agent_id = state.default_agent_id,
        .request_id = state.generation.request_id,
        .phase = state.generation.phase,
        .reasoning_length = state.generation.reasoning_text.size(),
    };
    if (state.generation.phase == ResponsePhase::reasoning) {
        return cursor;
    }
    if (state.generation.phase != ResponsePhase::answering || !cursor.open_entry_id
        || state.transcript.empty()) {
        return std::nullopt;
    }
    const TranscriptEntry& entry = state.transcript.back();
    if (entry.id != *cursor.open_entry_id || entry.status != EntryStatus::streaming
        || entry.request_id != cursor.request_id) {
        return std::nullopt;
    }
    cursor.answer_length = entry.text.size();
    return cursor;
}

} // namespace cha
