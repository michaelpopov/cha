#include "ui/web/session_projection.h"

#include <gtest/gtest.h>

namespace cha::web {
namespace {

TEST(SessionProjection, MovesCompleteCoreStateIntoTheProtocolDto) {
    SessionState state{
        .characters = {
            {"reviewer", "Reviewer", "Checks details"},
            {"guide", "guide", "Explains things"},
        },
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
        .forum_default_character_id = "guide",
    };
    const WebPresentationState presentation{
        .notice = "Current notice",
        .lifecycle = SessionLifecycle::stopping,
        .shutdown_reason = ShutdownReason::server_stopping,
    };

    const SessionSnapshot snapshot = to_snapshot(
        descriptor, std::move(state), presentation);

    EXPECT_EQ(snapshot, (SessionSnapshot{
        .forum = {"forum", "Forum", "guide", {
            {"guide", "guide", "Explains things"},
            {"reviewer", "Reviewer", "Checks details"},
        }},
        .session_id = "session",
        .session_label = "Label",
        .characters = {
            {"reviewer", "Reviewer", "Checks details"},
            {"guide", "guide", "Explains things"},
        },
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

    // The publication path builds exactly one owning transcript copy, so the
    // mapper must move its strings rather than copy them. Assert on the
    // moved-from source: a mapper that copies leaves these populated.
    for (const cha::TranscriptEntry& entry : state.transcript) {
        EXPECT_TRUE(entry.text.empty());
        EXPECT_TRUE(entry.display_name.empty());
    }
    for (const CharacterInfo& character : state.characters) {
        EXPECT_TRUE(character.name.empty());
    }
    EXPECT_TRUE(state.default_agent_id.empty());
    EXPECT_TRUE(state.generation.reasoning_text.empty());
}

TEST(SessionProjection, MapsAnsweringStoppingAndInactiveGenerationStates) {
    const SessionDescriptor descriptor{{"forum", "session"}, "Forum", "Label", "guide"};
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
