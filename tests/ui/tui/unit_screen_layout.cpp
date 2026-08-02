#include "ui/tui/screen_layout.h"

#include <gtest/gtest.h>

namespace cha {
namespace {

TEST(TranscriptLayout, AccountsForWrappingOffsetsAndControlCharacters) {
    EXPECT_EQ(layout_rows("abcd", 4), 2);
    EXPECT_EQ(layout_rows("ab", 4, 3), 2);
    EXPECT_EQ(layout_rows("a\nb", 10), 2);
    EXPECT_EQ(layout_rows("a\rb", 10), 1);
    EXPECT_EQ(layout_rows("\t", 8), 2);
    EXPECT_EQ(layout_rows(L"ab\ncd", 10), 2);
}

TEST(TranscriptViewport, FollowsOutputUntilThePersonaScrolls) {
    TranscriptViewport viewport;
    viewport.update(30, 10);
    EXPECT_EQ(viewport.top(), 20);
    EXPECT_TRUE(viewport.follows_output());

    viewport.scroll_up();
    EXPECT_EQ(viewport.top(), 15);
    EXPECT_FALSE(viewport.follows_output());

    viewport.update(40, 10);
    EXPECT_EQ(viewport.top(), 15);

    viewport.scroll_down();
    EXPECT_EQ(viewport.top(), 20);
    EXPECT_FALSE(viewport.follows_output());
    viewport.scroll_down();
    EXPECT_EQ(viewport.top(), 25);
    EXPECT_FALSE(viewport.follows_output());
    viewport.scroll_down();
    EXPECT_EQ(viewport.top(), 30);
    EXPECT_TRUE(viewport.follows_output());
}

TEST(TranscriptViewport, ClampsPositionWhenContentShrinks) {
    TranscriptViewport viewport;
    viewport.update(40, 10);
    viewport.scroll_up();
    EXPECT_EQ(viewport.top(), 25);

    viewport.update(12, 10);
    EXPECT_EQ(viewport.top(), 2);
    EXPECT_FALSE(viewport.follows_output());
}

} // namespace
} // namespace cha
