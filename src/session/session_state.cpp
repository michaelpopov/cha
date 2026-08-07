#include "session/session_state.h"

#include <utility>

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

std::optional<SessionAppendProjection> session_text_append_since(
    const TranscriptView& transcript,
    const AppendGenerationView& generation,
    const ParticipantId& default_agent_id,
    const SessionStateCursor& cursor) {
    if (cursor.history_epoch != transcript.history_epoch
        || cursor.default_agent_id != default_agent_id
        || cursor.entry_count != transcript.entries.size()
        || cursor.open_entry_id != transcript.open_entry_id
        || !cursor.request_id || !generation.active
        || generation.request_id != cursor.request_id
        || generation.phase != cursor.phase) {
        return std::nullopt;
    }
    if (generation.phase == ResponsePhase::reasoning) {
        if (transcript.revision != cursor.revision
            || generation.reasoning_text.size() <= cursor.reasoning_length) {
            return std::nullopt;
        }
        // All of the non-text continuity fields were matched above. Updating
        // the scalar cursor directly keeps this owner-thread hot path from
        // materializing an owning SessionState (and copying its transcript).
        SessionStateCursor next = cursor;
        next.revision = transcript.revision;
        next.reasoning_length = generation.reasoning_text.size();
        return SessionAppendProjection{
            .append = {ReasoningTextTarget{*generation.request_id},
                       std::string(generation.reasoning_text.substr(cursor.reasoning_length))},
            .cursor = std::move(next),
        };
    }
    if (generation.phase != ResponsePhase::answering
        || transcript.revision <= cursor.revision || !transcript.open_entry_id
        || transcript.entries.empty()
        // Reasoning may arrive after answer chunks. It cannot be hidden inside
        // an answer append, so require that the other appendable text is also
        // unchanged before proving this target.
        || generation.reasoning_text.size() != cursor.reasoning_length) {
        return std::nullopt;
    }
    const TranscriptEntry& entry = transcript.entries.back();
    if (entry.id != *transcript.open_entry_id || entry.status != EntryStatus::streaming
        || entry.request_id != cursor.request_id
        || entry.text.size() <= cursor.answer_length) {
        return std::nullopt;
    }
    // The checks above prove that the cursor differs only by answer growth
    // and the transcript revision caused by that growth.
    SessionStateCursor next = cursor;
    next.revision = transcript.revision;
    next.answer_length = entry.text.size();
    return SessionAppendProjection{
        .append = {EntryTextTarget{entry.id}, entry.text.substr(cursor.answer_length)},
        .cursor = std::move(next),
    };
}

} // namespace cha
