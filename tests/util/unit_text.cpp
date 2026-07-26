#include "util/path_name.h"
#include "util/text.h"

#include <gtest/gtest.h>

#include <filesystem>

namespace cha {
namespace {

TEST(Text, TrimsAllStandardWhitespaceWithoutCopying) {
    const std::string_view input = "\r\n \tvalue \v\f";

    EXPECT_EQ(trim_view(input), "value");
    EXPECT_TRUE(trim_view(" \t\r\n").empty());
}

TEST(Text, FindsTheFirstStandardWhitespaceCharacter) {
    EXPECT_EQ(find_whitespace("command\targument"), 7U);
    EXPECT_EQ(find_whitespace("command\nargument"), 7U);
    EXPECT_EQ(find_whitespace("command"), std::string_view::npos);
}

TEST(Text, FoldsOnlyAsciiLetters) {
    EXPECT_EQ(fold_ascii("Ada_123"), "ada_123");
    EXPECT_EQ(
        fold_ascii("\xD0\x98\xD0\xB2\xD0\xB0\xD0\xBD"),
        "\xD0\x98\xD0\xB2\xD0\xB0\xD0\xBD");
}

TEST(PathName, AcceptsOneSafePathComponent) {
    EXPECT_NO_THROW(require_path_component("session-1", "sessions"));
}

TEST(PathName, RejectsEmptySpecialAndNestedPaths) {
    const std::filesystem::path source = "personas";

    EXPECT_THROW(require_path_component("", source), std::runtime_error);
    EXPECT_THROW(require_path_component(".", source), std::runtime_error);
    EXPECT_THROW(require_path_component("..", source), std::runtime_error);
    EXPECT_THROW(require_path_component("nested/forum", source), std::runtime_error);
    EXPECT_THROW(require_path_component("/absolute", source), std::runtime_error);
}

} // namespace
} // namespace cha
