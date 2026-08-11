#include "web/session_projection.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace cha::web {
namespace {

// Backing storage a borrowed ControllerView points into. Tests mutate or
// destroy it after projection to prove the snapshot retained no borrow.
struct BackingState {
    std::vector<CharacterMetadata> characters;
    std::string default_character_id;
    std::vector<cha::TranscriptEntry> transcript;
    std::string character_id;
    std::string character_display_name;
    std::string reasoning_text;

    [[nodiscard]] ControllerView view() const {
        return {
            .characters = characters,
            .default_character_id = default_character_id,
            .transcript = {
                .entries = transcript,
                .revision = 42,
                .open_entry_id = 2,
                .history_epoch = 9,
            },
            .generation = {
                .active = true,
                .request_id = 7,
                .character_id = character_id,
                .character_display_name = character_display_name,
                .phase = ResponsePhase::reasoning,
                .reasoning_text = reasoning_text,
            },
        };
    }
};

BackingState populated_state() {
    return {
        .characters = {
            {"reviewer", "Reviewer", "Checks details"},
            {"guide", "guide", "Explains things"},
        },
        .default_character_id = "reviewer",
        .transcript = {
            {1, EntryKind::human, "persona", "Persona", "guide", "Guide", "Question", EntryStatus::complete, 7},
            {2, EntryKind::character, "guide", "Guide", {}, {}, "Partial", EntryStatus::streaming, 7},
            {3, EntryKind::notice, {}, "System", {}, {}, "Notice", EntryStatus::cancelled, std::nullopt},
            {4, EntryKind::error, "reviewer", "Error", {}, {}, "Failure", EntryStatus::failed, 8},
        },
        .character_id = "guide",
        .character_display_name = "Guide",
        .reasoning_text = "Thinking",
    };
}

SessionDescriptor test_descriptor() {
    return {
        .identity = {"forum", "session"},
        .forum_display_name = "Forum",
        .session_label = "Label",
        .forum_default_character_id = "guide",
        .forum_default_persona_id = "persona",
        .forum_default_persona_display_name = "Persona",
    };
}

TEST(SessionProjection, CopiesABorrowedControllerViewIntoTheProtocolDto) {
    const BackingState state = populated_state();
    const WebPresentationState presentation{
        .notice = "Current notice",
        .lifecycle = SessionLifecycle::stopping,
        .shutdown_reason = ShutdownReason::server_stopping,
    };

    const SessionSnapshot snapshot =
        to_snapshot(test_descriptor(), state.view(), presentation);

    EXPECT_EQ(snapshot, (SessionSnapshot{
        .forum = {"forum", "Forum", "guide", "persona", "Persona", {
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
            {2, EntryKind::character, "guide", "Guide", {}, {}, "Partial", EntryStatus::streaming, 7},
            {3, EntryKind::notice, {}, "System", {}, {}, "Notice", EntryStatus::cancelled, std::nullopt},
            {4, EntryKind::error, "reviewer", "Error", {}, {}, "Failure", EntryStatus::failed, 8},
        },
        .generation = {true, 7, "guide", "Guide", ResponsePhase::reasoning, "Thinking"},
        .notice = "Current notice",
        .lifecycle = SessionLifecycle::stopping,
        .shutdown_reason = ShutdownReason::server_stopping,
    }));
}

TEST(SessionProjection, RetainsNoBorrowIntoTheControllerBackingState) {
    SessionSnapshot snapshot;
    {
        BackingState state = populated_state();
        snapshot = to_snapshot(test_descriptor(), state.view(), {});

        // Mutating the backing values before they are destroyed catches a
        // retained span as well as a retained string view.
        state.characters.clear();
        state.default_character_id = "gone";
        state.transcript.clear();
        state.character_display_name = "gone";
        state.reasoning_text = "gone";
    }

    ASSERT_EQ(snapshot.characters.size(), 2U);
    EXPECT_EQ(snapshot.characters.front().id, "reviewer");
    EXPECT_EQ(snapshot.forum.members.size(), 2U);
    EXPECT_EQ(snapshot.default_character_id, "reviewer");
    ASSERT_EQ(snapshot.transcript.size(), 4U);
    EXPECT_EQ(snapshot.transcript.back().text, "Failure");
    EXPECT_EQ(snapshot.generation.character_display_name, "Guide");
    EXPECT_EQ(snapshot.generation.reasoning_text, "Thinking");
}

TEST(SessionProjection, ProjectsAnEmptyControllerView) {
    const SessionSnapshot snapshot = to_snapshot(
        test_descriptor(),
        ControllerView{},
        {.lifecycle = SessionLifecycle::running});

    EXPECT_TRUE(snapshot.characters.empty());
    EXPECT_TRUE(snapshot.forum.members.empty());
    EXPECT_TRUE(snapshot.transcript.empty());
    EXPECT_TRUE(snapshot.default_character_id.empty());
    EXPECT_FALSE(snapshot.generation.active);
    EXPECT_FALSE(snapshot.notice);
    EXPECT_EQ(snapshot.lifecycle, SessionLifecycle::running);
    EXPECT_EQ(snapshot.session_id, "session");
}

} // namespace
} // namespace cha::web
