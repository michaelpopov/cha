#include "media/media_resources.h"

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
    EXPECT_FALSE(resources.read_chunk("view-1", id, 3, 0));
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

TEST(MediaResources, ActiveClipsExposeBoundedChunksBeforeCompletion) {
    MediaResources resources;
    auto stream = std::make_shared<AudioStream>();
    const auto id = resources.add_stream("view", 1, ResourceKind::speech, stream);
    EXPECT_FALSE(resources.read_chunk("other", id, 1, 0));
    EXPECT_FALSE(resources.read_chunk("view", id, 2, 0));
    EXPECT_FALSE(resources.read_chunk("view", id, 1, 1));
    const auto pending = resources.read_chunk("view", id, 1, 0);
    ASSERT_TRUE(pending);
    EXPECT_TRUE(pending->body.empty());
    EXPECT_FALSE(pending->complete);

    stream->append("audio/mpeg", std::string(70 * 1024, 'a'));
    const auto first = resources.read_chunk("view", id, 1, 0);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->body.size(), 64 * 1024);
    EXPECT_EQ(first->mime_type, "audio/mpeg");
    EXPECT_FALSE(first->complete);
    EXPECT_FALSE(resources.read("view", id, 1));
    stream->finish();
    const auto last = resources.read_chunk("view", id, 1, first->body.size());
    ASSERT_TRUE(last);
    EXPECT_EQ(last->body.size(), 6 * 1024);
    EXPECT_TRUE(last->complete);
    ASSERT_TRUE(resources.read("view", id, 1));
    EXPECT_EQ(resources.read("view", id, 1)->body.size(), 70 * 1024);
    EXPECT_TRUE(resources.release("view", id));
    EXPECT_FALSE(resources.read_chunk("view", id, 1, 0));
}

TEST(MediaResources, PartialFailureIsNotSuccessfulEndOfAudio) {
    MediaResources resources;
    auto stream = std::make_shared<AudioStream>();
    const auto id = resources.add_stream("view", 1, ResourceKind::speech, stream);
    stream->append("audio/mpeg", "partial");
    stream->fail();
    const auto chunk = resources.read_chunk("view", id, 1, 0);
    ASSERT_TRUE(chunk);
    EXPECT_TRUE(chunk->failed);
    EXPECT_TRUE(chunk->body.empty());
    EXPECT_FALSE(chunk->complete);
    EXPECT_FALSE(resources.read("view", id, 1));
}

} // namespace
} // namespace cha::app
