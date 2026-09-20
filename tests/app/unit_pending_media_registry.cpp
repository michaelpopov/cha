#include "app/pending_media_registry.h"

#include <gtest/gtest.h>

namespace cha::app {
namespace {

TEST(PendingMediaRegistry, QueuedCompletedReplyRemainsCancellable) {
    MediaResources resources;
    PendingMediaRegistry pending(resources);
    const auto request = pending.remember("view", 1);
    const auto id = resources.add("view", 1, ResourceKind::speech, {"audio/mpeg", "audio"});
    ASSERT_TRUE(pending.set_resource(request, id));
    {
        PendingMediaRegistry::Cleanup cleanup{pending, request};
    }
    ASSERT_TRUE(resources.read("view", id, 1));
    pending.cancel("view", 1);
    EXPECT_TRUE(request->cancelled->load());
    EXPECT_FALSE(resources.read("view", id, 1));
}

TEST(PendingMediaRegistry, CancellationCannotPublishOrEraseAReplacementRequest) {
    MediaResources resources;
    PendingMediaRegistry pending(resources);
    const auto old = pending.remember("view", 1);
    pending.cancel("view", 1);
    const auto replacement = pending.remember("view", 1);
    pending.forget(old);
    EXPECT_FALSE(pending.set_resource(old, "late-resource"));

    const auto id = resources.add("view", 1, ResourceKind::speech, {"audio/mpeg", "audio"});
    EXPECT_TRUE(pending.set_resource(replacement, id));
    pending.cancel("view", 1);
    EXPECT_TRUE(replacement->cancelled->load());
    EXPECT_FALSE(resources.read("view", id, 1));
}

TEST(PendingMediaRegistry, ReleaseAndConnectionTeardownRemoveTheirAssociations) {
    MediaResources resources;
    PendingMediaRegistry pending(resources);
    const auto first = pending.remember("first", 1);
    const auto second = pending.remember("second", 1);
    const auto id = resources.add("first", 1, ResourceKind::speech, {"audio/mpeg", "audio"});
    ASSERT_TRUE(pending.set_resource(first, id));
    pending.release_resource("second", id);
    EXPECT_TRUE(resources.read("first", id, 1));
    pending.release_resource("first", id);
    EXPECT_FALSE(resources.read("first", id, 1));
    EXPECT_FALSE(pending.set_resource(first, "already-released"));
    pending.cancel_connection("first");
    EXPECT_FALSE(second->cancelled->load());
    pending.cancel_all();
    EXPECT_TRUE(second->cancelled->load());
    EXPECT_FALSE(pending.set_resource(second, "late-resource"));
}

} // namespace
} // namespace cha::app
