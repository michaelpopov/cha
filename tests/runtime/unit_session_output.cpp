#include "runtime/session_output.h"

#include <gtest/gtest.h>

#include <string>

namespace cha::app {
namespace {

SessionSnapshot streaming_snapshot(std::string text = "a") {
    return {
        .transcript = {{
            .id = 42,
            .kind = EntryKind::character,
            .text = std::move(text),
            .status = EntryStatus::streaming,
        }},
        .generation = {
            .active = true,
            .request_id = 7,
            .phase = ResponsePhase::answering,
        },
    };
}

TEST(SessionOutput, MonotonicSequenceDoesNotResetOnLaterSnapshot) {
    SessionOutput output;
    output.attach();
    output.publish_snapshot(streaming_snapshot("a"));
    auto first = output.take();
    ASSERT_TRUE(first);
    EXPECT_EQ(first->kind, SessionOutputItem::Kind::snapshot);
    EXPECT_EQ(first->seq, 0U);
    output.acknowledge();

    EXPECT_EQ(
        output.publish_append({EntryTextTarget{42}, "b"}),
        AppendPublishResult::Accepted);
    auto append = output.take();
    ASSERT_TRUE(append);
    EXPECT_EQ(append->kind, SessionOutputItem::Kind::append);
    EXPECT_EQ(append->seq, 1U);
    output.acknowledge();

    output.publish_snapshot(streaming_snapshot("ab"));
    auto second = output.take();
    ASSERT_TRUE(second);
    EXPECT_EQ(second->kind, SessionOutputItem::Kind::snapshot);
    EXPECT_EQ(second->seq, 2U);
}

TEST(SessionOutput, DirtyProjectionWaitsForConsumerDemandAndAcknowledgement) {
    SessionOutput output;
    output.attach();
    output.publish_snapshot(streaming_snapshot("a"));
    const auto first = output.take();
    ASSERT_TRUE(first);
    output.require_snapshot();
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(output.publish_append({EntryTextTarget{42}, "b"}),
                  AppendPublishResult::Accepted);
        EXPECT_FALSE(output.snapshot_needed());
    }
    EXPECT_FALSE(output.take());
    output.acknowledge();
    EXPECT_FALSE(output.snapshot_needed());
    EXPECT_FALSE(output.take());
    EXPECT_TRUE(output.snapshot_needed());
    output.publish_snapshot(streaming_snapshot("latest"));
    output.require_snapshot(); // A pending projection becomes obsolete.
    EXPECT_FALSE(output.snapshot_needed());
    EXPECT_FALSE(output.take());
    EXPECT_TRUE(output.snapshot_needed());
    output.publish_snapshot(streaming_snapshot("newest"));
    const auto replacement = output.take();
    ASSERT_TRUE(replacement);
    EXPECT_EQ(replacement->seq, 1U);
    EXPECT_EQ(replacement->snapshot.transcript[0].text, "newest");
}

TEST(SessionOutput, MergesCompatibleAppendsAndBoundsPendingBytes) {
    SessionOutput output(4);
    output.attach();
    output.publish_snapshot(streaming_snapshot("a"));
    (void)output.take();
    output.acknowledge();

    EXPECT_EQ(
        output.publish_append({EntryTextTarget{42}, "bb"}),
        AppendPublishResult::Accepted);
    EXPECT_EQ(
        output.publish_append({EntryTextTarget{42}, "c"}),
        AppendPublishResult::Accepted);
    auto merged = output.take();
    ASSERT_TRUE(merged);
    EXPECT_EQ(merged->text, "bbc");
    output.acknowledge();

    EXPECT_EQ(
        output.publish_append({EntryTextTarget{42}, "xxxx"}),
        AppendPublishResult::Accepted);
    EXPECT_EQ(
        output.publish_append({EntryTextTarget{42}, "y"}),
        AppendPublishResult::SnapshotRequired);
    EXPECT_EQ(
        output.publish_append({EntryTextTarget{42}, "zzzzz"}),
        AppendPublishResult::SnapshotRequired);
}

TEST(SessionOutput, CloseKeepsTerminalPending) {
    SessionOutput output;
    output.attach();
    output.publish_snapshot(streaming_snapshot("final"));
    output.close();
    auto item = output.take();
    ASSERT_TRUE(item);
    EXPECT_EQ(item->snapshot.transcript[0].text, "final");
}

TEST(SessionOutput, OneInFlightUntilAcknowledged) {
    SessionOutput output;
    output.attach();
    output.publish_snapshot(streaming_snapshot("a"));
    auto first = output.take();
    ASSERT_TRUE(first);
    output.publish_snapshot(streaming_snapshot("b"));
    EXPECT_TRUE(output.has_pending());
    EXPECT_FALSE(output.take());
    output.acknowledge();
    auto second = output.take();
    ASSERT_TRUE(second);
    EXPECT_EQ(second->snapshot.transcript[0].text, "b");
    EXPECT_EQ(second->seq, 1U);
}

TEST(SessionOutput, ReplacedPendingPayloadsDoNotConsumeSequenceNumbers) {
    SessionOutput output;
    output.attach();
    output.publish_snapshot(streaming_snapshot("a"));
    auto first = output.take();
    ASSERT_TRUE(first);
    EXPECT_EQ(first->seq, 0U);

    output.publish_snapshot(streaming_snapshot("b"));
    output.publish_snapshot(streaming_snapshot("c"));
    EXPECT_EQ(output.next_sequence(), 1U);

    output.acknowledge();
    auto replacement = output.take();
    ASSERT_TRUE(replacement);
    EXPECT_EQ(replacement->snapshot.transcript[0].text, "c");
    EXPECT_EQ(replacement->seq, 1U);
}

} // namespace
} // namespace cha::app
