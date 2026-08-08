#include "ui/web/sse_mailbox.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <type_traits>

namespace cha::web {
namespace {

using namespace std::chrono_literals;

static_assert(std::is_same_v<
    decltype(SseMailbox::Next{}.payload),
    std::shared_ptr<const SsePayload>>);

SessionSnapshot streaming_snapshot(std::string text = "") {
    return {
        .transcript = {{
            .id = 42,
            .kind = EntryKind::agent,
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

TEST(SseMailbox, MergesCompatibleAppendsWithoutConsumingSequence) {
    SseMailbox mailbox;
    const SseMailbox::Stream stream = mailbox.begin_stream({streaming_snapshot("a")});
    ASSERT_TRUE(mailbox.next(stream, 1ms).payload);
    mailbox.written(stream);

    EXPECT_EQ(
        mailbox.publish_append({EntryTextTarget{42}, "\xE2\x82\xAC"}),
        AppendPublishResult::Accepted);
    EXPECT_EQ(
        mailbox.publish_append({EntryTextTarget{42}, "!"}),
        AppendPublishResult::Accepted);
    const SseMailbox::Next merged = mailbox.next(stream, 1ms);
    ASSERT_TRUE(merged.open);
    const auto* append = std::get_if<AppendEvent>(&*merged.payload);
    ASSERT_NE(append, nullptr);
    EXPECT_EQ(append->seq, 0U);
    EXPECT_EQ(append->text, "\xE2\x82\xAC!");
    mailbox.written(stream);

    EXPECT_EQ(
        mailbox.publish_append({EntryTextTarget{42}, "?"}),
        AppendPublishResult::Accepted);
    const SseMailbox::Next next = mailbox.next(stream, 1ms);
    const auto* following = std::get_if<AppendEvent>(&*next.payload);
    ASSERT_NE(following, nullptr);
    EXPECT_EQ(following->seq, 1U);
    mailbox.written(stream);
    EXPECT_EQ(mailbox.end_stream(stream), 1U);
}

TEST(SseMailbox, RejectsAppendsItCannotRepresentWithoutLosingPendingWork) {
    SseMailbox mailbox;
    const SseMailbox::Stream stream = mailbox.begin_stream({streaming_snapshot("a")});
    ASSERT_TRUE(mailbox.next(stream, 1ms).payload);
    mailbox.written(stream);

    // A pending snapshot cannot absorb a later append, and rejection must not
    // disturb the payload the browser is still owed.
    EXPECT_EQ(
        mailbox.publish_append({EntryTextTarget{42}, "b"}),
        AppendPublishResult::Accepted);
    SessionSnapshot structural_snapshot = streaming_snapshot("ab");
    structural_snapshot.notice = "structural change";
    mailbox.publish(SnapshotEvent{std::move(structural_snapshot)});
    EXPECT_EQ(
        mailbox.publish_append({EntryTextTarget{42}, "c"}),
        AppendPublishResult::SnapshotRequired);
    const SseMailbox::Next structural = mailbox.next(stream, 1ms);
    ASSERT_TRUE(structural.open);
    const auto* snapshot = std::get_if<SnapshotEvent>(structural.payload.get());
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->snapshot.transcript[0].text, "ab");
    EXPECT_EQ(snapshot->snapshot.notice, "structural change");
    mailbox.written(stream);

    // The base target established by the last snapshot is an answer entry, so
    // a reasoning append is not representable.
    EXPECT_EQ(
        mailbox.publish_append({ReasoningTextTarget{7}, "think"}),
        AppendPublishResult::SnapshotRequired);
    // An empty append is a controller contract error, reported conservatively.
    EXPECT_EQ(
        mailbox.publish_append({EntryTextTarget{42}, ""}),
        AppendPublishResult::SnapshotRequired);
    EXPECT_FALSE(mailbox.next(stream, 1ms).payload);
    EXPECT_EQ(mailbox.end_stream(stream), 1U);
}

TEST(SseMailbox, AcceptsAppendsWithNoStreamToRepair) {
    SseMailbox mailbox;

    EXPECT_EQ(
        mailbox.publish_append({EntryTextTarget{42}, "b"}),
        AppendPublishResult::Accepted);

    const SseMailbox::Stream stream = mailbox.begin_stream({streaming_snapshot("a")});
    mailbox.close();
    EXPECT_EQ(
        mailbox.publish_append({EntryTextTarget{42}, "b"}),
        AppendPublishResult::Accepted);
    EXPECT_EQ(mailbox.end_stream(stream), 0U);
}

TEST(SseMailbox, AppendWithoutASnapshotBaseRequiresASnapshot) {
    SseMailbox mailbox;
    SessionSnapshot idle;
    const SseMailbox::Stream stream = mailbox.begin_stream({std::move(idle)});
    ASSERT_TRUE(mailbox.next(stream, 1ms).payload);
    mailbox.written(stream);

    EXPECT_EQ(
        mailbox.publish_append({EntryTextTarget{42}, "b"}),
        AppendPublishResult::SnapshotRequired);
    EXPECT_FALSE(mailbox.next(stream, 1ms).payload);
    mailbox.end_stream(stream);
}

TEST(SseMailbox, ReturnedPayloadRemainsImmutableAfterWriterAcknowledgement) {
    SseMailbox mailbox;
    const SseMailbox::Stream stream =
        mailbox.begin_stream({streaming_snapshot("stable")});
    const SseMailbox::Next next = mailbox.next(stream, 1ms);
    ASSERT_TRUE(next.payload);

    mailbox.written(stream);
    mailbox.publish(SnapshotEvent{streaming_snapshot("replacement")});

    const auto* snapshot = std::get_if<SnapshotEvent>(next.payload.get());
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->snapshot.transcript[0].text, "stable");
}

TEST(SseMailbox, FinalDrainCompletesAfterPendingSnapshotCollapse) {
    SseMailbox mailbox;
    const SseMailbox::Stream stream = mailbox.begin_stream({streaming_snapshot("old")});
    mailbox.publish(SnapshotEvent{streaming_snapshot("final")});

    const SseMailbox::Next final = mailbox.next(stream, 1ms);
    ASSERT_TRUE(final.open);
    ASSERT_TRUE(std::holds_alternative<SnapshotEvent>(*final.payload));
    EXPECT_EQ(std::get<SnapshotEvent>(*final.payload).snapshot.transcript[0].text, "final");

    auto drained = std::async(std::launch::async, [&] {
        return mailbox.wait_for_written(100ms);
    });
    mailbox.written(stream);
    EXPECT_TRUE(drained.get());
}

TEST(SseMailbox, FinalDrainCompletesWhenStreamEnds) {
    SseMailbox mailbox;
    const SseMailbox::Stream stream = mailbox.begin_stream({streaming_snapshot()});
    ASSERT_TRUE(mailbox.next(stream, 1ms).payload);

    auto drained = std::async(std::launch::async, [&] {
        return mailbox.wait_for_written(100ms);
    });
    mailbox.end_stream(stream);
    EXPECT_TRUE(drained.get());
}

TEST(SseMailbox, HigherPriorityShutdownInterruptsFinalDrain) {
    SseMailbox mailbox;
    const SseMailbox::Stream stream =
        mailbox.begin_stream({streaming_snapshot()});
    ASSERT_TRUE(mailbox.next(stream, 1ms).payload);

    auto drained = std::async(std::launch::async, [&] {
        return mailbox.wait_for_written(5s);
    });
    EXPECT_EQ(drained.wait_for(10ms), std::future_status::timeout);

    mailbox.interrupt_final_drain();
    ASSERT_EQ(drained.wait_for(1s), std::future_status::ready);
    EXPECT_FALSE(drained.get());
}

TEST(SseMailbox, NewStreamStartsCollapseCountAfterItsInitialSnapshot) {
    SseMailbox mailbox;
    const SseMailbox::Stream replaced =
        mailbox.begin_stream({streaming_snapshot("old")});
    mailbox.publish(SnapshotEvent{streaming_snapshot("older")});

    // Replacing an active stream is not a production transition, but it
    // exercises the ordering invariant directly: pre-existing pending state
    // must not contribute to the new stream's disconnect count.
    const SseMailbox::Stream current =
        mailbox.begin_stream({streaming_snapshot("current")});
    EXPECT_NE(replaced.id, current.id);
    EXPECT_EQ(mailbox.end_stream(current), 0U);
}

} // namespace
} // namespace cha::web
