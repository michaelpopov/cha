#include "web/session_projection.h"
#include "support/test_workspace.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

namespace cha::web {
namespace {

// Backing storage a borrowed ControllerView points into. Tests mutate or
// destroy it after projection to prove the snapshot retained no borrow.
struct BackingState {
    std::string default_character_id;
    std::string default_persona_id{"persona"};
    std::vector<cha::TranscriptEntry> transcript;
    std::string character_id;
    std::string character_display_name;
    std::string reasoning_text;

    [[nodiscard]] ControllerView view() const {
        return {
            .default_character_id = default_character_id,
            .default_persona_id = default_persona_id,
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
        .default_character_id = "reviewer",
        .default_persona_id = "reviewer_persona",
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

void publish_projection_workspace() {
    const auto definition = [](std::string id, std::string name, std::string description) {
        return CharacterDefinition{
            .character = {
                .id = std::move(id),
                .display_name = std::move(name),
                .description = std::move(description),
            },
            .provider = {.id = "test", .config = {
                .host = "127.0.0.1",
                .port = 1,
                .model = "test-model",
                .web_search = WebSearchMode::off,
            }},
        };
    };
    (void)test::publish_test_workspace(
        {
            definition("reviewer", "Reviewer", "Checks details"),
            definition("guide", "guide", "Explains things"),
        },
        {{.id = "reviewer_persona", .display_name = "Reviewer persona"}},
        "guide",
        {},
        {"forum", "session"});
}

SessionDescriptor test_descriptor() {
    return {
        .identity = {"forum", "session"},
        .forum_display_name = "Forum",
        .session_label = "Label",
        .forum_default_character_id = "guide",
    };
}

TEST(SessionProjection, CopiesABorrowedControllerViewIntoTheProtocolDto) {
    publish_projection_workspace();
    const BackingState state = populated_state();
    const WebPresentationState presentation{
        .notice = "Current notice",
        .lifecycle = SessionLifecycle::stopping,
        .shutdown_reason = ShutdownReason::server_stopping,
    };

    const SessionSnapshot snapshot =
        to_snapshot(test_descriptor(), state.view(), presentation);

    EXPECT_EQ(snapshot, (SessionSnapshot{
        // A session descriptor carries no forum description, so a snapshot's
        // forum leaves it unset. Discovery is where it is read.
        .forum = {"forum", "Forum", std::nullopt, "guide", "reviewer_persona", "Reviewer persona", {
            {"guide", "guide", "Explains things"},
            {"reviewer", "Reviewer", "Checks details"},
        }},
        .session_id = "session",
        .session_label = "Label",
        .characters = {
            {"guide", "guide", "Explains things"},
            {"reviewer", "Reviewer", "Checks details"},
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
    publish_projection_workspace();
    SessionSnapshot snapshot;
    {
        BackingState state = populated_state();
        snapshot = to_snapshot(test_descriptor(), state.view(), {});

        // Mutating the backing values before they are destroyed catches a
        // retained string view.
        state.default_character_id = "gone";
        state.transcript.clear();
        state.character_display_name = "gone";
        state.reasoning_text = "gone";
    }

    ASSERT_EQ(snapshot.characters.size(), 2U);
    EXPECT_EQ(snapshot.characters.front().id, "guide");
    EXPECT_EQ(snapshot.forum.members.size(), 2U);
    EXPECT_EQ(snapshot.default_character_id, "reviewer");
    ASSERT_EQ(snapshot.transcript.size(), 4U);
    EXPECT_EQ(snapshot.transcript.back().text, "Failure");
    EXPECT_EQ(snapshot.generation.character_display_name, "Guide");
    EXPECT_EQ(snapshot.generation.reasoning_text, "Thinking");
}

TEST(SessionProjection, ProjectsWorkspaceDataWithEmptySessionState) {
    publish_projection_workspace();
    const SessionSnapshot snapshot = to_snapshot(
        test_descriptor(),
        ControllerView{.default_persona_id = "reviewer_persona"},
        {.lifecycle = SessionLifecycle::running});

    EXPECT_EQ(snapshot.characters.size(), 2U);
    EXPECT_EQ(snapshot.forum.members.size(), 2U);
    EXPECT_TRUE(snapshot.transcript.empty());
    EXPECT_TRUE(snapshot.default_character_id.empty());
    EXPECT_FALSE(snapshot.generation.active);
    EXPECT_FALSE(snapshot.notice);
    EXPECT_EQ(snapshot.lifecycle, SessionLifecycle::running);
    EXPECT_EQ(snapshot.session_id, "session");
}

} // namespace
} // namespace cha::web
