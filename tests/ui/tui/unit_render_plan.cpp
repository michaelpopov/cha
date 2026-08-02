#include "transcript/transcript.h"
#include "support/test_transcript.h"
#include "ui/tui/render_plan.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cha {
namespace {

TranscriptEntry human(EntryId id, std::string text) {
    return test::human_entry(
        id, {"human", "You"}, {"guide-id", "Guide"}, std::move(text));
}

TranscriptEntry agent(EntryId id, std::string text) {
    return make_agent_entry(
        id,
        "guide-id",
        "Guide",
        std::move(text),
        EntryStatus::complete);
}

struct TestTranscript {
    std::vector<TranscriptEntry> entries;
    std::size_t revision{};
    std::optional<EntryId> open_entry_id;
    std::size_t history_epoch{};

    [[nodiscard]] TranscriptView view() const noexcept {
        return {
            entries,
            revision,
            open_entry_id,
            history_epoch,
        };
    }
};

TEST(TranscriptRenderPlanner, RebuildsInitiallyAndAfterWidthChanges) {
    TranscriptRenderPlanner planner;
    const TestTranscript transcript{
        .entries = {human(1, "Hello")},
        .revision = 1,
    };

    EXPECT_EQ(
        planner.plan(transcript.view(), 80).action,
        TranscriptRenderAction::rebuild);
    planner.commit(transcript.view(), 80);

    EXPECT_EQ(
        planner.plan(transcript.view(), 80).action,
        TranscriptRenderAction::none);
    EXPECT_EQ(
        planner.plan(transcript.view(), 40).action,
        TranscriptRenderAction::rebuild);
}

TEST(TranscriptRenderPlanner, AppendsAStreamingSuffixAndNewMessages) {
    TranscriptRenderPlanner planner;
    const TestTranscript initial{
        .entries = {
            human(1, "Question"),
            make_agent_entry(
                2,
                "guide-id",
                "Guide",
                "Partial",
                EntryStatus::streaming),
        },
        .revision = 2,
        .open_entry_id = 2,
    };
    planner.commit(initial.view(), 80);

    const TestTranscript streamed{
        .entries = {
            human(1, "Question"),
            make_agent_entry(
                2,
                "guide-id",
                "Guide",
                "Partial answer",
                EntryStatus::streaming),
        },
        .revision = 3,
        .open_entry_id = 2,
    };
    const TranscriptRenderPlan stream_plan =
        planner.plan(streamed.view(), 80);
    EXPECT_EQ(stream_plan.action, TranscriptRenderAction::append);
    EXPECT_TRUE(stream_plan.resumes_last_message);
    EXPECT_EQ(stream_plan.last_message_suffix, " answer");
    EXPECT_EQ(stream_plan.first_new_message, 2U);
    planner.commit(streamed.view(), 80);

    const TestTranscript with_new_message{
        .entries = {
            human(1, "Question"),
            agent(2, "Partial answer"),
            human(3, "Follow-up"),
        },
        .revision = 4,
    };
    const TranscriptRenderPlan new_message_plan =
        planner.plan(with_new_message.view(), 80);
    EXPECT_EQ(new_message_plan.action, TranscriptRenderAction::append);
    EXPECT_TRUE(new_message_plan.resumes_last_message);
    EXPECT_TRUE(new_message_plan.last_message_suffix.empty());
    EXPECT_EQ(new_message_plan.first_new_message, 2U);
}

TEST(TranscriptRenderPlanner, AppendsFromAnEmptyRenderedTranscript) {
    TranscriptRenderPlanner planner;
    const TestTranscript empty{.revision = 1};
    planner.commit(empty.view(), 80);

    const TestTranscript transcript{
        .entries = {human(1, "First")},
        .revision = 2,
    };
    const TranscriptRenderPlan plan =
        planner.plan(transcript.view(), 80);

    EXPECT_EQ(plan.action, TranscriptRenderAction::append);
    EXPECT_FALSE(plan.resumes_last_message);
    EXPECT_EQ(plan.first_new_message, 0U);
}

TEST(TranscriptRenderPlanner, RebuildsWhenHistoryOrTailIsNotAnAppend) {
    TranscriptRenderPlanner planner;
    const TestTranscript initial{
        .entries = {human(1, "First"), agent(2, "Second")},
        .revision = 1,
    };
    planner.commit(initial.view(), 80);

    const TestTranscript replaced_history{
        .entries = {human(1, "Changed"), agent(2, "Second")},
        .revision = 2,
        .history_epoch = 1,
    };
    EXPECT_EQ(
        planner.plan(replaced_history.view(), 80).action,
        TranscriptRenderAction::rebuild);

    const TestTranscript shortened_last{
        .entries = {human(1, "First"), agent(2, "Sec")},
        .revision = 3,
    };
    EXPECT_EQ(
        planner.plan(shortened_last.view(), 80).action,
        TranscriptRenderAction::rebuild);

    const TestTranscript fewer_messages{
        .entries = {human(1, "First")},
        .revision = 4,
    };
    EXPECT_EQ(
        planner.plan(fewer_messages.view(), 80).action,
        TranscriptRenderAction::rebuild);
}

TEST(TranscriptRenderPlanner, IgnoresARevisionOnlyChange) {
    TranscriptRenderPlanner planner;
    const TestTranscript initial{
        .entries = {human(1, "Same")},
        .revision = 1,
    };
    planner.commit(initial.view(), 80);

    const TestTranscript unchanged{
        .entries = {human(1, "Same")},
        .revision = 2,
    };
    EXPECT_EQ(
        planner.plan(unchanged.view(), 80).action,
        TranscriptRenderAction::none);
}

} // namespace
} // namespace cha
