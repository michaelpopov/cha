#include "ui/console/line_reader.h"

#include <gtest/gtest.h>

#include <vector>

namespace cha {
namespace {

TEST(LineReader, BuffersPartialBytesAndReturnsSeveralLinesInOrder) {
    LineReader reader;
    EXPECT_TRUE(reader.append("hel").empty());
    EXPECT_EQ(reader.append("lo\n"), std::vector<std::string>({"hello"}));
    EXPECT_EQ(
        reader.append("a\nb\nc\n"),
        std::vector<std::string>({"a", "b", "c"}));
}

TEST(LineReader, ConcatenatesContinuationChainsWithoutNewlines) {
    LineReader reader;
    EXPECT_EQ(
        reader.append("first\\\nsecond\n"),
        std::vector<std::string>({"firstsecond"}));
    EXPECT_EQ(
        reader.append("a\\\nb\\\nc\n"),
        std::vector<std::string>({"abc"}));
}

TEST(LineReader, FlushesEveryUnterminatedFinalForm) {
    LineReader tail;
    EXPECT_TRUE(tail.append("tail").empty());
    EXPECT_EQ(tail.flush(), std::vector<std::string>({"tail"}));

    LineReader slash;
    EXPECT_TRUE(slash.append("a\\").empty());
    EXPECT_EQ(slash.flush(), std::vector<std::string>({"a"}));

    LineReader continued_tail;
    EXPECT_TRUE(continued_tail.append("first\\\nsecond").empty());
    EXPECT_EQ(
        continued_tail.flush(),
        std::vector<std::string>({"firstsecond"}));

    LineReader empty_continuation;
    EXPECT_TRUE(empty_continuation.append("first\\\n").empty());
    EXPECT_EQ(
        empty_continuation.flush(),
        std::vector<std::string>({"first"}));

    LineReader empty;
    EXPECT_TRUE(empty.append("").empty());
    EXPECT_TRUE(empty.flush().empty());
}

TEST(LineReader, PreservesSubmittedEmptyLinesWithoutInventingOneAtEof) {
    LineReader reader;
    EXPECT_EQ(reader.append("\n"), std::vector<std::string>({""}));
    EXPECT_TRUE(reader.flush().empty());
}

} // namespace
} // namespace cha
