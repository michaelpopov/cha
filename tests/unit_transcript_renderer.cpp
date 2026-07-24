#include "transcript_renderer.h"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace cha {
namespace {

ConversationEntry human(EntryId id, std::string text) {
    return make_human_entry(id, "guide-id", "Guide", std::move(text));
}

ConversationEntry agent(EntryId id, std::string text) {
    return make_agent_entry(
        id, "guide-id", "Guide", std::move(text), CompletionStatus::complete);
}

ConversationEntry reasoning_agent(
    EntryId id,
    std::string reasoning,
    std::string answer,
    CompletionStatus status = CompletionStatus::streaming) {
    return make_agent_entry(
        id,
        "guide-id",
        "Guide",
        {
            .reasoning = std::move(reasoning),
            .answer = std::move(answer),
        },
        status);
}

class RecordingSurface final : public TranscriptSurface {
public:
    void attributes(TranscriptAttributes value) override {
        current = value;
        operations.push_back({value, {}});
    }

    void write(std::string_view text) override {
        operations.push_back({current, std::string(text)});
        output += text;
    }

    struct Operation {
        TranscriptAttributes attributes;
        std::string text;
    };

    TranscriptAttributes current{TranscriptAttributes::normal};
    std::vector<Operation> operations;
    std::string output;
};

TEST(TranscriptLabel, DistinguishesKindsEvenWhenDisplayNamesCollide) {
    EXPECT_EQ(
        transcript_entry_label(human(1, "Question"), false),
        "[You] ");
    EXPECT_EQ(
        transcript_entry_label(
            make_agent_entry(2, "agent-id", "You", "Answer", CompletionStatus::complete), false),
        "[Agent: You] ");
    EXPECT_EQ(transcript_entry_label(make_notice_entry(3, "Notice"), false), "[System] ");
    EXPECT_EQ(transcript_entry_label(make_error_entry(4, "Failure"), false), "[Error] ");
}

TEST(TranscriptRenderPlanner, RebuildsInitiallyAndAfterWidthChanges) {
    TranscriptRenderPlanner planner;
    const ConversationSnapshot snapshot{
        .entries = {human(1, "Hello")},
        .revision = 1,
    };

    EXPECT_EQ(planner.plan(snapshot, 80).action, TranscriptRenderAction::rebuild);
    planner.commit(snapshot, 80);

    EXPECT_EQ(planner.plan(snapshot, 80).action, TranscriptRenderAction::none);
    EXPECT_EQ(planner.plan(snapshot, 40).action, TranscriptRenderAction::rebuild);
}

TEST(TranscriptRenderPlanner, AppendsAStreamingSuffixAndNewMessages) {
    TranscriptRenderPlanner planner;
    const ConversationSnapshot initial{
        .entries = {
            human(1, "Question"),
            make_agent_entry(
                2, "guide-id", "Guide", "Partial", CompletionStatus::streaming),
        },
        .revision = 2,
        .open_entry_id = 2,
    };
    planner.commit(initial, 80);

    const ConversationSnapshot streamed{
        .entries = {
            human(1, "Question"),
            make_agent_entry(
                2, "guide-id", "Guide", "Partial answer", CompletionStatus::streaming),
        },
        .revision = 3,
        .open_entry_id = 2,
    };
    const TranscriptRenderPlan stream_plan = planner.plan(streamed, 80);
    EXPECT_EQ(stream_plan.action, TranscriptRenderAction::append);
    EXPECT_TRUE(stream_plan.resumes_last_message);
    EXPECT_EQ(stream_plan.last_message_suffix, " answer");
    EXPECT_EQ(stream_plan.first_new_message, 2U);
    planner.commit(streamed, 80);

    const ConversationSnapshot with_new_message{
        .entries = {
            human(1, "Question"),
            agent(2, "Partial answer"),
            human(3, "Follow-up"),
        },
        .revision = 4,
    };
    const TranscriptRenderPlan new_message_plan = planner.plan(with_new_message, 80);
    EXPECT_EQ(new_message_plan.action, TranscriptRenderAction::append);
    EXPECT_TRUE(new_message_plan.resumes_last_message);
    EXPECT_TRUE(new_message_plan.last_message_suffix.empty());
    EXPECT_EQ(new_message_plan.first_new_message, 2U);
}

TEST(TranscriptRenderPlanner, AppendsFromAnEmptyRenderedTranscript) {
    TranscriptRenderPlanner planner;
    planner.commit({.revision = 1}, 80);

    const ConversationSnapshot snapshot{
        .entries = {human(1, "First")},
        .revision = 2,
    };
    const TranscriptRenderPlan plan = planner.plan(snapshot, 80);

    EXPECT_EQ(plan.action, TranscriptRenderAction::append);
    EXPECT_FALSE(plan.resumes_last_message);
    EXPECT_EQ(plan.first_new_message, 0U);
}

TEST(TranscriptRenderPlanner, RebuildsWhenRenderedContentIsNotAnAppend) {
    TranscriptRenderPlanner planner;
    const ConversationSnapshot initial{
        .entries = {human(1, "First"), agent(2, "Second")},
        .revision = 1,
    };
    planner.commit(initial, 80);

    const ConversationSnapshot changed_prefix{
        .entries = {human(1, "Changed"), agent(2, "Second")},
        .revision = 2,
        .history_epoch = 1,
    };
    EXPECT_EQ(planner.plan(changed_prefix, 80).action, TranscriptRenderAction::rebuild);

    const ConversationSnapshot shortened_last{
        .entries = {human(1, "First"), agent(2, "Sec")},
        .revision = 3,
    };
    EXPECT_EQ(planner.plan(shortened_last, 80).action, TranscriptRenderAction::rebuild);

    const ConversationSnapshot fewer_messages{
        .entries = {human(1, "First")},
        .revision = 4,
    };
    EXPECT_EQ(planner.plan(fewer_messages, 80).action, TranscriptRenderAction::rebuild);
}

TEST(TranscriptRenderPlanner, IgnoresARevisionOnlyChange) {
    TranscriptRenderPlanner planner;
    const ConversationSnapshot initial{
        .entries = {human(1, "Same")},
        .revision = 1,
    };
    planner.commit(initial, 80);

    const ConversationSnapshot unchanged{
        .entries = {human(1, "Same")},
        .revision = 2,
    };
    EXPECT_EQ(planner.plan(unchanged, 80).action, TranscriptRenderAction::none);
}

TEST(TranscriptRenderPlanner, HandlesReasoningAndAnswerPhaseBoundaries) {
    TranscriptRenderPlanner planner;
    const ConversationSnapshot initial{
        .entries = {reasoning_agent(1, "Think", "")},
        .revision = 1,
        .open_entry_id = 1,
    };
    planner.commit(initial, 80);

    ConversationSnapshot reasoning_growth{
        .entries = {reasoning_agent(1, "Thinking", "")},
        .revision = 2,
        .open_entry_id = 1,
    };
    TranscriptRenderPlan plan = planner.plan(reasoning_growth, 80);
    EXPECT_EQ(plan.action, TranscriptRenderAction::append);
    EXPECT_EQ(plan.suffix_kind, CompletionDeltaKind::reasoning);
    EXPECT_EQ(plan.last_message_suffix, "ing");
    planner.commit(reasoning_growth, 80);

    ConversationSnapshot first_answer{
        .entries = {reasoning_agent(1, "Thinking", "Answer")},
        .revision = 3,
        .open_entry_id = 1,
    };
    EXPECT_EQ(
        planner.plan(first_answer, 80).action,
        TranscriptRenderAction::rebuild);
    planner.commit(first_answer, 80);

    ConversationSnapshot answer_growth{
        .entries = {reasoning_agent(1, "Thinking", "Answer grows")},
        .revision = 4,
        .open_entry_id = 1,
    };
    plan = planner.plan(answer_growth, 80);
    EXPECT_EQ(plan.action, TranscriptRenderAction::append);
    EXPECT_EQ(plan.suffix_kind, CompletionDeltaKind::answer);
    EXPECT_EQ(plan.last_message_suffix, " grows");
    planner.commit(answer_growth, 80);

    ConversationSnapshot late_reasoning{
        .entries = {reasoning_agent(1, "Thinking again", "Answer grows")},
        .revision = 5,
        .open_entry_id = 1,
    };
    EXPECT_EQ(
        planner.plan(late_reasoning, 80).action,
        TranscriptRenderAction::rebuild);
}

TEST(TranscriptRendering, LabelsReasoningAndRestoresNormalAttributes) {
    RecordingSurface surface;
    write_transcript_entry(
        surface,
        reasoning_agent(
            1,
            "Compare constraints",
            "Use the second option",
            CompletionStatus::complete),
        false);

    EXPECT_EQ(
        surface.output,
        "[Agent: Guide]\n[Reasoning]\nCompare constraints\n\n"
        "Use the second option");
    EXPECT_EQ(surface.current, TranscriptAttributes::normal);

    bool reasoning_was_dim = false;
    bool answer_was_normal = false;
    for (const RecordingSurface::Operation& operation : surface.operations) {
        if (operation.text == "Compare constraints") {
            reasoning_was_dim =
                operation.attributes == TranscriptAttributes::dim;
        }
        if (operation.text == "Use the second option") {
            answer_was_normal =
                operation.attributes == TranscriptAttributes::normal;
        }
    }
    EXPECT_TRUE(reasoning_was_dim);
    EXPECT_TRUE(answer_was_normal);

    write_transcript_entry(surface, human(2, "Ordinary"), false);
    EXPECT_EQ(surface.current, TranscriptAttributes::normal);
    EXPECT_EQ(surface.operations.back().attributes, TranscriptAttributes::normal);
}

TEST(TranscriptRendering, IncrementalAndInputInitializationRestoreNormalAttributes) {
    RecordingSurface surface;
    surface.current = TranscriptAttributes::bold_dim;
    write_transcript_suffix(
        surface,
        CompletionDeltaKind::reasoning,
        " more thought");
    EXPECT_EQ(surface.current, TranscriptAttributes::normal);
    ASSERT_GE(surface.operations.size(), 3U);
    EXPECT_EQ(
        surface.operations[surface.operations.size() - 2].attributes,
        TranscriptAttributes::dim);

    surface.current = TranscriptAttributes::dim;
    write_transcript_suffix(
        surface,
        CompletionDeltaKind::answer,
        " more answer");
    EXPECT_EQ(surface.current, TranscriptAttributes::normal);

    surface.current = TranscriptAttributes::dim;
    initialize_transcript_surface(surface);
    EXPECT_EQ(surface.current, TranscriptAttributes::normal);
}

TEST(TranscriptLayout, AccountsForWrappingOffsetsAndControlCharacters) {
    EXPECT_EQ(layout_rows("abcd", 4), 2);
    EXPECT_EQ(layout_rows("ab", 4, 3), 2);
    EXPECT_EQ(layout_rows("a\nb", 10), 2);
    EXPECT_EQ(layout_rows("a\rb", 10), 1);
    EXPECT_EQ(layout_rows("\t", 8), 2);
    EXPECT_EQ(layout_rows(L"ab\ncd", 10), 2);
}

TEST(TranscriptViewport, FollowsOutputUntilTheUserScrolls) {
    TranscriptViewport viewport;
    viewport.update(30, 10);
    EXPECT_EQ(viewport.top(), 20);
    EXPECT_TRUE(viewport.follows_output());

    viewport.scroll_up();
    EXPECT_EQ(viewport.top(), 15);
    EXPECT_FALSE(viewport.follows_output());

    viewport.update(40, 10);
    EXPECT_EQ(viewport.top(), 15);

    viewport.scroll_down();
    EXPECT_EQ(viewport.top(), 20);
    EXPECT_FALSE(viewport.follows_output());
    viewport.scroll_down();
    EXPECT_EQ(viewport.top(), 25);
    EXPECT_FALSE(viewport.follows_output());
    viewport.scroll_down();
    EXPECT_EQ(viewport.top(), 30);
    EXPECT_TRUE(viewport.follows_output());
}

TEST(TranscriptViewport, ClampsPositionWhenContentShrinks) {
    TranscriptViewport viewport;
    viewport.update(40, 10);
    viewport.scroll_up();
    EXPECT_EQ(viewport.top(), 25);

    viewport.update(12, 10);
    EXPECT_EQ(viewport.top(), 2);
    EXPECT_FALSE(viewport.follows_output());
}

} // namespace
} // namespace cha
