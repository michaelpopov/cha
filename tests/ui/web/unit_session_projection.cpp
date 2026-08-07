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
    const cha::TranscriptEntry* const transcript_storage =
        state.transcript.data();

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
            {1, EntryKind::human, "persona", "Persona", "guide", "Guide", "Question", EntryStatus::complete, 7},
            {2, EntryKind::agent, "guide", "Guide", {}, {}, "Partial", EntryStatus::streaming, 7},
            {3, EntryKind::notice, {}, "System", {}, {}, "Notice", EntryStatus::cancelled, std::nullopt},
            {4, EntryKind::error, "reviewer", "Error", {}, {}, "Failure", EntryStatus::failed, 8},
        },
        .generation = {true, 7, "guide", "Guide", ResponsePhase::reasoning, "Thinking"},
        .notice = "Current notice",
        .lifecycle = SessionLifecycle::stopping,
        .shutdown_reason = ShutdownReason::server_stopping,
    }));

    // Projection reuses the core transcript storage rather than building a
    // third entry model at the protocol boundary.
    EXPECT_EQ(snapshot.transcript.data(), transcript_storage);
}

} // namespace
} // namespace cha::web
