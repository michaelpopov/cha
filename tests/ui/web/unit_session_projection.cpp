#include "ui/web/session_projection.h"

#include <gtest/gtest.h>

namespace cha::web {
namespace {

TEST(SessionProjection, MovesCompleteCoreStateIntoTheProtocolDto) {
    SessionState state{
        .characters = {{"guide", "Guide"}, {"reviewer", "Reviewer"}},
        .default_agent_id = "reviewer",
        .transcript = {
            {1, EntryKind::human, "persona", "Persona", "guide", "Guide", "Question", EntryStatus::complete, 7},
            {2, EntryKind::agent, "guide", "Guide", {}, {}, "Partial", EntryStatus::streaming, 7},
            {3, EntryKind::notice, {}, "System", {}, {}, "Notice", EntryStatus::cancelled, std::nullopt},
            {4, EntryKind::error, "reviewer", "Error", {}, {}, "Failure", EntryStatus::failed, 8},
        },
        .revision = 42,
        .open_entry_id = 2,
        .history_epoch = 9,
        .generation = {
            .active = true,
            .request_id = 7,
            .agent_id = "guide",
            .agent_name = "Guide",
            .phase = ResponsePhase::reasoning,
            .reasoning_text = "Thinking",
        },
    };
    const SessionDescriptor descriptor{
        .identity = {"forum", "session"},
        .forum_display_name = "Forum",
        .session_label = "Label",
    };
    const WebPresentationState presentation{
        .notice = "Current notice",
        .lifecycle = SessionLifecycle::stopping,
        .shutdown_reason = ShutdownReason::server_stopping,
    };

    const SessionSnapshot snapshot = to_snapshot(
        descriptor, std::move(state), presentation);

    EXPECT_EQ(snapshot, (SessionSnapshot{
        .forum = {"forum", "Forum"},
        .session_id = "session",
        .session_label = "Label",
        .characters = {{"guide", "Guide"}, {"reviewer", "Reviewer"}},
        .default_character_id = "reviewer",
        .transcript = {
            {1, TranscriptKind::human, "persona", "Persona", "guide", "Guide", "Question", TranscriptStatus::complete, 7},
            {2, TranscriptKind::agent, "guide", "Guide", {}, {}, "Partial", TranscriptStatus::streaming, 7},
            {3, TranscriptKind::notice, {}, "System", {}, {}, "Notice", TranscriptStatus::cancelled, std::nullopt},
            {4, TranscriptKind::error, "reviewer", "Error", {}, {}, "Failure", TranscriptStatus::failed, 8},
        },
        .generation = {true, 7, "guide", "Guide", GenerationPhase::reasoning, "Thinking"},
        .notice = "Current notice",
        .lifecycle = SessionLifecycle::stopping,
        .shutdown_reason = ShutdownReason::server_stopping,
    }));
}

TEST(SessionProjection, MapsAnsweringStoppingAndInactiveGenerationStates) {
    const SessionDescriptor descriptor{{"forum", "session"}, "Forum", "Label"};
    const WebPresentationState presentation{.lifecycle = SessionLifecycle::running};

    for (const GenerationStatus& generation : std::vector<GenerationStatus>{
             {true, 11, "guide", "Guide", ResponsePhase::answering, "Reasoned"},
             {true, 12, "guide", "Guide", ResponsePhase::stopping, ""},
             {false, std::nullopt, "", "", ResponsePhase::waiting, ""},
         }) {
        const SessionSnapshot snapshot = to_snapshot(
            descriptor, SessionState{.generation = generation}, presentation);
        EXPECT_EQ(snapshot.generation, (GenerationState{
            generation.active,
            generation.request_id,
            generation.agent_id,
            generation.agent_name,
            generation.phase == ResponsePhase::answering ? GenerationPhase::answering
            : generation.phase == ResponsePhase::stopping ? GenerationPhase::stopping
            : GenerationPhase::waiting,
            generation.reasoning_text,
        }));
    }
}

} // namespace
} // namespace cha::web
