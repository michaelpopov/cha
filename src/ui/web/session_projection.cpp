#include "ui/web/session_projection.h"

#include <stdexcept>
#include <utility>

namespace cha::web {
namespace {

TranscriptKind web_kind(EntryKind kind) {
    switch (kind) {
    case EntryKind::human: return TranscriptKind::human;
    case EntryKind::agent: return TranscriptKind::agent;
    case EntryKind::notice: return TranscriptKind::notice;
    case EntryKind::error: return TranscriptKind::error;
    }
    throw std::logic_error("Unsupported transcript entry kind");
}

TranscriptStatus web_status(EntryStatus status) {
    switch (status) {
    case EntryStatus::complete: return TranscriptStatus::complete;
    case EntryStatus::streaming: return TranscriptStatus::streaming;
    case EntryStatus::cancelled: return TranscriptStatus::cancelled;
    case EntryStatus::failed: return TranscriptStatus::failed;
    }
    throw std::logic_error("Unsupported transcript entry status");
}

GenerationPhase web_phase(ResponsePhase phase) {
    switch (phase) {
    case ResponsePhase::waiting: return GenerationPhase::waiting;
    case ResponsePhase::reasoning: return GenerationPhase::reasoning;
    case ResponsePhase::answering: return GenerationPhase::answering;
    case ResponsePhase::stopping: return GenerationPhase::stopping;
    }
    throw std::logic_error("Unsupported generation phase");
}

} // namespace

SessionSnapshot to_snapshot(
    const SessionDescriptor& descriptor,
    SessionState&& state,
    const WebPresentationState& presentation) {
    SessionSnapshot snapshot{
        .forum = {
            .id = descriptor.identity.forum_id,
            .display_name = descriptor.forum_display_name,
            .default_character_id = state.default_agent_id,
        },
        .session_id = descriptor.identity.session_id,
        .session_label = descriptor.session_label,
        .default_character_id = std::move(state.default_agent_id),
        .notice = presentation.notice,
        .lifecycle = presentation.lifecycle,
        .shutdown_reason = presentation.shutdown_reason,
    };
    snapshot.characters.reserve(state.characters.size());
    snapshot.forum.members.reserve(state.characters.size());
    for (CharacterInfo& character : state.characters) {
        snapshot.forum.members.push_back({
            .id = character.id,
            .display_name = character.name,
        });
        snapshot.characters.push_back({
            .id = std::move(character.id),
            .display_name = std::move(character.name),
        });
    }
    snapshot.transcript.reserve(state.transcript.size());
    for (cha::TranscriptEntry& entry : state.transcript) {
        snapshot.transcript.push_back({
            .id = entry.id,
            .kind = web_kind(entry.kind),
            .participant_id = std::move(entry.participant_id),
            .display_name = std::move(entry.display_name),
            .addressed_to = std::move(entry.addressed_to),
            .addressed_to_name = std::move(entry.addressed_to_name),
            .text = std::move(entry.text),
            .status = web_status(entry.status),
            .request_id = entry.request_id,
        });
    }
    snapshot.generation = {
        .active = state.generation.active,
        .request_id = state.generation.request_id,
        .agent_id = std::move(state.generation.agent_id),
        .agent_name = std::move(state.generation.agent_name),
        .phase = web_phase(state.generation.phase),
        .reasoning_text = std::move(state.generation.reasoning_text),
    };
    return snapshot;
}

} // namespace cha::web
