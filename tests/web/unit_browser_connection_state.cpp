#include "web/browser_connection_state.h"

#include <gtest/gtest.h>

using namespace std::chrono_literals;

namespace cha::web {
namespace {

TEST(BrowserConnectionState, OnlyMatchingCloseDetachesTheActiveStream) {
    BrowserConnectionState state;
    const auto start = BrowserConnectionState::Clock::time_point{};
    state.published(start);
    const auto first = state.accept();
    ASSERT_TRUE(first);
    EXPECT_FALSE(state.accept());
    EXPECT_FALSE(state.close(*first + 1, start + 3ms));
    EXPECT_FALSE(state.deadline(false, 10ms, 20ms));
    EXPECT_TRUE(state.close(*first, start + 4ms));
    EXPECT_FALSE(state.close(*first, start + 5ms));
    EXPECT_EQ(state.deadline(false, 10ms, 20ms), start + 14ms);
}

TEST(BrowserConnectionState, UsesOneAbsoluteDeadlineFromDisconnection) {
    BrowserConnectionState state;
    const auto start = BrowserConnectionState::Clock::time_point{};
    state.published(start);
    EXPECT_EQ(state.deadline(false, 10ms, 30ms), start + 10ms);
    EXPECT_EQ(state.deadline(true, 10ms, 30ms), start + 30ms);
    const auto connection = state.accept();
    ASSERT_TRUE(connection);
    EXPECT_FALSE(state.deadline(false, 10ms, 30ms));
    ASSERT_TRUE(state.close(*connection, start + 4ms));
    EXPECT_EQ(state.deadline(true, 10ms, 30ms), start + 34ms);
}

} // namespace
} // namespace cha::web
