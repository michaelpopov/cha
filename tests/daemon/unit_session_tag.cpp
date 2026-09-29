#include "daemon/session_tag.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace cha::daemon {
namespace {

TEST(SessionTag, FormatsTheMarkdownReferenceLine) {
    EXPECT_EQ(
        format_session_tag("lobby", "abc"),
        "[//]: # (cha lobby/abc)");
}

TEST(SessionTag, ReadsTheFirstLineAfterLeadingWhitespace) {
    const auto tag = parse_session_tag_text(
        "  \t[//]: # (cha lobby/session-1)\nbody\n");
    ASSERT_TRUE(tag);
    EXPECT_EQ(tag->forum_id, "lobby");
    EXPECT_EQ(tag->session_id, "session-1");

    const auto after_blank_lines = parse_session_tag_text(
        "\n \r\n\t[//]: # (cha lobby/session-2)\r\nbody\n");
    ASSERT_TRUE(after_blank_lines);
    EXPECT_EQ(after_blank_lines->session_id, "session-2");
}

TEST(SessionTag, RejectsMalformedLines) {
    EXPECT_FALSE(parse_session_tag_text("hello"));
    EXPECT_FALSE(parse_session_tag_text("[//]: # (cha lobby)"));
    EXPECT_FALSE(parse_session_tag_text("[//]: # (cha lobby/)"));
    EXPECT_FALSE(parse_session_tag_text("[//]: # (cha /session)"));
    EXPECT_FALSE(parse_session_tag_text("[//]: # (cha lobby/session/extra)"));
    EXPECT_FALSE(parse_session_tag_text("[//]: # (cha lobby/bad id)"));
    EXPECT_FALSE(parse_session_tag_text("x[//]: # (cha lobby/session)"));
    EXPECT_FALSE(parse_session_tag_text(
        "body\n[//]: # (cha lobby/session)\n"));
    EXPECT_FALSE(parse_session_tag_text(""));
    EXPECT_FALSE(parse_session_tag_text(" \n\n"));
}

TEST(SessionTag, ExtractsOrderedTextPartsAndIgnoresNonText) {
    const nlohmann::json message{
        {"role", "assistant"},
        {"content",
         nlohmann::json::array(
             {{{"type", "image_url"}, {"image_url", {{"url", "x"}}}},
              {{"text", "ignored malformed"}},
              {{"type", "text"}, {"text", "  "}},
              {{"type", "text"}, {"text", "[//]: # (cha lobby/p1)"}},
              "ignored"})},
    };
    EXPECT_EQ(
        message_text(message),
        "  [//]: # (cha lobby/p1)");
    const auto tag = parse_session_tag_text(message_text(message));
    ASSERT_TRUE(tag);
    EXPECT_EQ(tag->session_id, "p1");
}

TEST(SessionTag, SelectsTheNewestMatchingAssistantTag) {
    const nlohmann::json messages = nlohmann::json::array({
        {{"role", "user"}, {"content", "[//]: # (cha other/user-tag)"}},
        {{"role", "assistant"},
         {"content", "[//]: # (cha lobby/old)\nfirst"}},
        {{"role", "assistant"}, {"content", "no tag here"}},
        {{"role", "system"}, {"content", "[//]: # (cha lobby/system)"}},
        {{"role", "assistant"},
         {"content", "  [//]: # (cha lobby/new)\nsecond"}},
        {{"role", "user"}, {"content", "next"}},
    });
    const SessionTagScanResult result = find_session_tag(messages);
    EXPECT_EQ(result.status, SessionTagScan::found);
    EXPECT_EQ(result.tag.forum_id, "lobby");
    EXPECT_EQ(result.tag.session_id, "new");
}

TEST(SessionTag, NewerMalformedTagDoesNotHideAnOlderMatch) {
    const nlohmann::json messages = nlohmann::json::array({
        {{"role", "assistant"}, {"content", "[//]: # (cha lobby/kept)"}},
        {{"role", "assistant"}, {"content", "[//]: # (cha broken"}},
        {{"role", "user"}, {"content", "go"}},
    });
    const SessionTagScanResult result = find_session_tag(messages);
    EXPECT_EQ(result.status, SessionTagScan::found);
    EXPECT_EQ(result.tag.session_id, "kept");
}

TEST(SessionTag, ReportsNoAssistantAndMissingTags) {
    const nlohmann::json none = nlohmann::json::array({
        {{"role", "user"}, {"content", "hello"}},
        {{"role", "system"}, {"content", "[//]: # (cha lobby/hidden)"}},
    });
    EXPECT_EQ(find_session_tag(none).status, SessionTagScan::no_assistant);

    const nlohmann::json missing = nlohmann::json::array({
        {{"role", "assistant"}, {"content", "untagged"}},
        {{"role", "user"}, {"content", "hello"}},
    });
    EXPECT_EQ(find_session_tag(missing).status, SessionTagScan::missing);
}

TEST(SessionTag, ReadsTheFirstAssistantTitle) {
    const nlohmann::json messages = nlohmann::json::array({
        {{"role", "user"}, {"content", "Pasted Title"}},
        {{"role", "assistant"}, {"content", "A session title\r\n\r\nFirst reply"}},
        {{"role", "assistant"}, {"content", "Later reply"}},
    });
    EXPECT_EQ(first_assistant_title(messages), "A session title");
    EXPECT_FALSE(first_assistant_title(nlohmann::json::array({
        {{"role", "assistant"}, {"content", " \n\n"}},
    })));
    EXPECT_FALSE(first_assistant_title(nlohmann::json::array({
        {{"role", "assistant"}, {"content", "Ordinary assistant reply"}},
    })));
    EXPECT_FALSE(first_assistant_title(nlohmann::json::array({
        {{"role", "assistant"}, {"content", "Title\nReply without blank line"}},
    })));
}

} // namespace
} // namespace cha::daemon
