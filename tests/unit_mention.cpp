#include "mention.h"

#include <gtest/gtest.h>

namespace cha {

TEST(Mention, PreservesOrdinaryInputAndRecognizesAddressing) {
    EXPECT_EQ(parse_addressed_prompt("   ordinary").handle, "");
    EXPECT_EQ(parse_addressed_prompt("   ordinary").text, "   ordinary");
    EXPECT_EQ(parse_addressed_prompt("  @Ada hello").handle, "Ada");
    EXPECT_EQ(parse_addressed_prompt("  @Ada hello").text, "hello");
    EXPECT_EQ(parse_addressed_prompt("  @@Ada hello").text, "  @Ada hello");
}

TEST(Mention, TreatsEmptyMentionFormsAsLiteralText) {
    EXPECT_EQ(parse_addressed_prompt("@").text, "@");
    EXPECT_EQ(parse_addressed_prompt("@ hello").text, "@ hello");
    EXPECT_EQ(parse_addressed_prompt("   ").text, "   ");
}

TEST(Mention, PreservesMentionBodyAndDoesNotInterpretPunctuationOrUnicode) {
    const AddressedPrompt punctuation = parse_addressed_prompt("@Ada, hello\nworld");
    EXPECT_EQ(punctuation.handle, "Ada,");
    EXPECT_EQ(punctuation.text, "hello\nworld");

    const AddressedPrompt unicode = parse_addressed_prompt("@Иван привет");
    EXPECT_EQ(unicode.handle, "Иван");
    EXPECT_EQ(unicode.text, "привет");

    const AddressedPrompt escaped = parse_addressed_prompt("  @@Ada hello");
    EXPECT_TRUE(escaped.handle.empty());
    EXPECT_EQ(escaped.text, "  @Ada hello");
}

} // namespace cha
