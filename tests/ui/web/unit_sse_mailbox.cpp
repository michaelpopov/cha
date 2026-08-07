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

    mailbox.publish_append({EntryTextTarget{42}, "\xE2\x82\xAC"}, streaming_snapshot("a\xE2\x82\xAC"));
    mailbox.publish_append({EntryTextTarget{42}, "!"}, streaming_snapshot("a\xE2\x82\xAC!"));
    const SseMailbox::Next merged = mailbox.next(stream, 1ms);
    ASSERT_TRUE(merged.open);
    const auto* append = std::get_if<AppendEvent>(&*merged.payload);
    ASSERT_NE(append, nullptr);
    EXPECT_EQ(append->seq, 0U);
    EXPECT_EQ(append->text, "\xE2\x82\xAC!");
    mailbox.written(stream);

    mailbox.publish_append({EntryTextTarget{42}, "?"}, streaming_snapshot("a\xE2\x82\xAC!?"));
    const SseMailbox::Next next = mailbox.next(stream, 1ms);
    const auto* following = std::get_if<AppendEvent>(&*next.payload);
    ASSERT_NE(following, nullptr);
    EXPECT_EQ(following->seq, 1U);
    mailbox.written(stream);
    EXPECT_EQ(mailbox.end_stream(stream), 1U);
}

TEST(SseMailbox, StructuralOrPendingSnapshotAppendCollapsesToSnapshot) {
    SseMailbox mailbox;
    const SseMailbox::Stream stream = mailbox.begin_stream({streaming_snapshot("a")});
    ASSERT_TRUE(mailbox.next(stream, 1ms).payload);
    mailbox.written(stream);

    mailbox.publish_append(
        {EntryTextTarget{42}, "b"}, streaming_snapshot("ab"));
    SessionSnapshot structural_snapshot = streaming_snapshot("ab");
    structural_snapshot.notice = "structural change";
    mailbox.publish(SnapshotEvent{std::move(structural_snapshot)});
    const SseMailbox::Next structural = mailbox.next(stream, 1ms);
    ASSERT_TRUE(structural.open);
    const auto* snapshot = std::get_if<SnapshotEvent>(structural.payload.get());
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->snapshot.transcript[0].text, "ab");
    EXPECT_EQ(snapshot->snapshot.notice, "structural change");
    mailbox.written(stream);

    mailbox.publish_append({ReasoningTextTarget{7}, "think"}, streaming_snapshot("ab"));
    const SseMailbox::Next incompatible = mailbox.next(stream, 1ms);
    EXPECT_TRUE(std::holds_alternative<SnapshotEvent>(*incompatible.payload));
    mailbox.written(stream);
    EXPECT_EQ(mailbox.end_stream(stream), 1U);
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
