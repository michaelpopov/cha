#include "app/media_resources.h"

#include <gtest/gtest.h>

namespace cha::app {
namespace {

TEST(MediaResources, IssuesOpaqueHandlesAndRejectsUnknownOrForeignReads) {
    MediaResources resources;
    const std::string id = resources.add(
        "view-1", 3, ResourceKind::speech, {"audio/mpeg", "bytes"});
    EXPECT_TRUE(MediaResources::valid_id(id));
    EXPECT_EQ(resources.url_for(id), "/media/" + id);
    EXPECT_FALSE(MediaResources::valid_id("../secret"));
    EXPECT_FALSE(MediaResources::valid_id("r../x"));
    EXPECT_FALSE(MediaResources::valid_id(""));

    const auto body = resources.read("view-1", id, 3);
    ASSERT_TRUE(body);
    EXPECT_EQ(body->mime_type, "audio/mpeg");
    EXPECT_EQ(body->body, "bytes");
    EXPECT_FALSE(resources.read("view-2", id, 3));
    EXPECT_FALSE(resources.read("view-1", id, 4));
    EXPECT_FALSE(resources.read("view-1", "r999", 3));
    EXPECT_FALSE(resources.read("view-1", "../r1", 3));

    EXPECT_TRUE(resources.release("view-1", id));
    EXPECT_FALSE(resources.read("view-1", id, 3));
    EXPECT_FALSE(resources.release("view-1", id));
}

TEST(MediaResources, RevokesOnConnectionLossAndSessionClear) {
    MediaResources resources;
    const FullSessionId session{"lobby", "chat"};
    const std::string speech = resources.add(
        "view-1", 1, ResourceKind::speech, {"audio/mpeg", "a"});
    const std::string cached = resources.add(
        "view-1", 1, ResourceKind::entry_audio, {"audio/wav", "b"}, session, 2);
    const std::string other = resources.add(
        "view-2", 1, ResourceKind::speech, {"audio/mpeg", "c"});
    resources.revoke_session(session);
    EXPECT_FALSE(resources.read("view-1", cached, 1));
    EXPECT_TRUE(resources.read("view-1", speech, 1));
    resources.revoke_connection("view-1");
    EXPECT_FALSE(resources.read("view-1", speech, 1));
    EXPECT_TRUE(resources.read("view-2", other, 1));
    resources.revoke_all();
    EXPECT_FALSE(resources.read("view-2", other, 1));
}

} // namespace
} // namespace cha::app
