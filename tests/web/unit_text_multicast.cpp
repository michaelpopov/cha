#include "web/text_input.h"

#include <gtest/gtest.h>

namespace cha {
namespace {

TEST(MulticastInput, ParsesRecipientsAndLiteralText) {
    struct Case {
        const char* input;
        MulticastInput expected;
    };
    const Case cases[]{
        {"What time is it?", {{}, "What time is it?"}},
        {"@@everyone please answer", {{}, "@everyone please answer"}},
        {"@one, @two, @five. What time is it?", {{"one", "two", "five"}, "What time is it?"}},
        {"@one @two @five What's time?", {{"one", "two", "five"}, "What's time?"}},
        {"@one. @two What time?", {{"one"}, "@two What time?"}},
        {"@one @two @@everyone", {{"one", "two"}, "@everyone"}},
        {"@one , @two What time is it?", {{"one", "two"}, "What time is it?"}},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.input);
        EXPECT_EQ(parse_multicast_input(item.input), MulticastParseResult{item.expected});
    }
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
} // namespace cha
