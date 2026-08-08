#include "web/text_multicast.h"

#include <gtest/gtest.h>

namespace cha::web {
namespace {

TEST(MulticastInput, ParsesAllCharacterPromptAndLiteralAtEscape) {
    EXPECT_EQ(
        parse_multicast_input("What time is it?"),
        (MulticastParseResult{MulticastInput{{}, "What time is it?"}}));
    EXPECT_EQ(
        parse_multicast_input("@@everyone please answer"),
        (MulticastParseResult{MulticastInput{{}, "@everyone please answer"}}));
}

TEST(MulticastInput, ParsesCommaAndWhitespaceSeparatedRecipients) {
    EXPECT_EQ(
        parse_multicast_input("@one, @two, @five. What time is it?"),
        (MulticastParseResult{MulticastInput{
            {"one", "two", "five"}, "What time is it?"}}));
    EXPECT_EQ(
        parse_multicast_input("@one @two @five What's time?"),
        (MulticastParseResult{MulticastInput{
            {"one", "two", "five"}, "What's time?"}}));
    EXPECT_EQ(
        parse_multicast_input("@one. @two What time?"),
        (MulticastParseResult{MulticastInput{{"one"}, "@two What time?"}}));
    EXPECT_EQ(
        parse_multicast_input("@one @two @@everyone"),
        (MulticastParseResult{MulticastInput{
            {"one", "two"}, "@everyone"}}));
    EXPECT_EQ(
        parse_multicast_input("@one , @two What time is it?"),
        (MulticastParseResult{MulticastInput{
            {"one", "two"}, "What time is it?"}}));
}

TEST(MulticastInput, RejectsMalformedRecipientLists) {
    EXPECT_EQ(
        parse_multicast_input(""),
        (MulticastParseResult{MulticastParseError::empty_prompt}));
    EXPECT_EQ(
        parse_multicast_input("@ What time?"),
        (MulticastParseResult{MulticastParseError::empty_handle}));
    EXPECT_EQ(
        parse_multicast_input("@one,, What time?"),
        (MulticastParseResult{MulticastParseError::unexpected_comma}));
    EXPECT_EQ(
        parse_multicast_input("@one , , @two What time?"),
        (MulticastParseResult{MulticastParseError::unexpected_comma}));
    EXPECT_EQ(
        parse_multicast_input("@one, What time?"),
        (MulticastParseResult{MulticastParseError::missing_separator}));
    EXPECT_EQ(
        parse_multicast_input("@one."),
        (MulticastParseResult{MulticastParseError::empty_prompt}));
}

} // namespace
} // namespace cha::web
