#include "session/controller_update.h"

#include <gtest/gtest.h>

#include <string>

namespace cha {
namespace {

ControllerStateUpdate entry_append(EntryId id, std::string text) {
    return TextAppend{EntryTextTarget{id}, std::move(text)};
}

ControllerStateUpdate reasoning_append(RequestId id, std::string text) {
    return TextAppend{ReasoningTextTarget{id}, std::move(text)};
}

TEST(ControllerUpdate, DefaultConstructionMeansNoStateUpdate) {
    const ControllerUpdate update;

    EXPECT_FALSE(has_state_update(update));
    EXPECT_FALSE(requires_snapshot(update));
    EXPECT_EQ(text_append(update), nullptr);
    EXPECT_FALSE(update.input_consumed);
    EXPECT_FALSE(update.session_ended);
    EXPECT_FALSE(update.notice);
}

TEST(ControllerUpdate, RequireSnapshotDominatesAnEarlierAppend) {
    ControllerUpdate update{.state = entry_append(7, "text")};
    require_snapshot(update);

    EXPECT_TRUE(requires_snapshot(update));
    EXPECT_TRUE(has_state_update(update));
}

TEST(ControllerUpdate, NoStateUpdateIsTheMergeIdentity) {
    EXPECT_EQ(merge_state(NoStateUpdate{}, NoStateUpdate{}),
        ControllerStateUpdate{NoStateUpdate{}});
    EXPECT_EQ(merge_state(NoStateUpdate{}, entry_append(7, "a")),
        entry_append(7, "a"));
    EXPECT_EQ(merge_state(entry_append(7, "a"), NoStateUpdate{}),
        entry_append(7, "a"));
    EXPECT_EQ(merge_state(NoStateUpdate{}, SnapshotRequired{}),
        ControllerStateUpdate{SnapshotRequired{}});
    EXPECT_EQ(merge_state(SnapshotRequired{}, NoStateUpdate{}),
        ControllerStateUpdate{SnapshotRequired{}});
}

TEST(ControllerUpdate, SnapshotRequiredDominatesEveryOtherEffect) {
    EXPECT_EQ(merge_state(SnapshotRequired{}, entry_append(7, "a")),
        ControllerStateUpdate{SnapshotRequired{}});
    EXPECT_EQ(merge_state(entry_append(7, "a"), SnapshotRequired{}),
        ControllerStateUpdate{SnapshotRequired{}});
    EXPECT_EQ(merge_state(SnapshotRequired{}, SnapshotRequired{}),
        ControllerStateUpdate{SnapshotRequired{}});
}

TEST(ControllerUpdate, SameTargetAppendsConcatenateInEventOrder) {
    EXPECT_EQ(merge_state(entry_append(7, "one"), entry_append(7, " two")),
        entry_append(7, "one two"));
    EXPECT_EQ(
        merge_state(reasoning_append(3, "th"), reasoning_append(3, "ink")),
        reasoning_append(3, "think"));
}

TEST(ControllerUpdate, DifferentTargetAppendsRequireASnapshot) {
    EXPECT_EQ(merge_state(entry_append(7, "a"), entry_append(8, "b")),
        ControllerStateUpdate{SnapshotRequired{}});
    EXPECT_EQ(merge_state(entry_append(7, "a"), reasoning_append(7, "b")),
        ControllerStateUpdate{SnapshotRequired{}});
    EXPECT_EQ(merge_state(reasoning_append(3, "a"), reasoning_append(4, "b")),
        ControllerStateUpdate{SnapshotRequired{}});
}

TEST(ControllerUpdate, LifecycleFlagsCombineWithLogicalOr) {
    ControllerUpdate all{.input_consumed = true};
    merge(all, {.session_ended = true});

    EXPECT_TRUE(all.input_consumed);
    EXPECT_TRUE(all.session_ended);

    ControllerUpdate ended{.session_ended = true};
    merge(ended, {});
    EXPECT_TRUE(ended.session_ended);
}

TEST(ControllerUpdate, CompletionEventsDoNotManufactureInputConsumption) {
    ControllerUpdate all;
    merge(all, {.state = SnapshotRequired{}});

    EXPECT_FALSE(all.input_consumed);
}

TEST(ControllerUpdate, TheLastSuppliedNoticeWinsIncludingAClearingOne) {
    ControllerUpdate all{.notice = "first"};
    merge(all, {});
    EXPECT_EQ(all.notice, "first");

    merge(all, {.notice = "second"});
    EXPECT_EQ(all.notice, "second");

    merge(all, {.notice = ""});
    ASSERT_TRUE(all.notice);
    EXPECT_TRUE(all.notice->empty());
}

TEST(ControllerUpdate, AnAppendFollowedByCompletionRequestsASnapshot) {
    ControllerUpdate all{.state = entry_append(7, "answer")};
    merge(all, {.state = SnapshotRequired{}, .notice = ""});

    EXPECT_TRUE(requires_snapshot(all));
    ASSERT_TRUE(all.notice);
    EXPECT_TRUE(all.notice->empty());
}

} // namespace
} // namespace cha
