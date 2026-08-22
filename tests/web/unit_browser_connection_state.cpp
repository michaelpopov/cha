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
    EXPECT_FALSE(first.superseded_connection_id);
    EXPECT_FALSE(state.close(first.connection_id + 1, start + 3ms));
    EXPECT_FALSE(state.deadline(false, 10ms, 20ms));
    EXPECT_TRUE(state.close(first.connection_id, start + 4ms));
    EXPECT_FALSE(state.close(first.connection_id, start + 5ms));
    EXPECT_EQ(state.deadline(false, 10ms, 20ms), start + 14ms);
}

// The reader moves from one device to the next and the newest one wins, so a
// second connection is granted immediately instead of waiting for the first
// device's stream to end.
TEST(BrowserConnectionState, ASecondConnectionTakesTheSessionOver) {
    BrowserConnectionState state;
    const auto start = BrowserConnectionState::Clock::time_point{};
    state.published(start);
    const auto first = state.accept();
    const auto second = state.accept();
    EXPECT_NE(second.connection_id, first.connection_id);
    ASSERT_TRUE(second.superseded_connection_id);
    EXPECT_EQ(*second.superseded_connection_id, first.connection_id);
    // The displaced device tears its stream down late; that close names a
    // connection which is no longer the active one and changes nothing.
    EXPECT_FALSE(state.close(first.connection_id, start + 4ms));
    EXPECT_FALSE(state.deadline(false, 10ms, 20ms));
    EXPECT_TRUE(state.close(second.connection_id, start + 5ms));
    EXPECT_EQ(state.deadline(false, 10ms, 20ms), start + 15ms);
}

TEST(BrowserConnectionState, UsesOneAbsoluteDeadlineFromDisconnection) {
    BrowserConnectionState state;
    const auto start = BrowserConnectionState::Clock::time_point{};
    state.published(start);
    EXPECT_EQ(state.deadline(false, 10ms, 30ms), start + 10ms);
    EXPECT_EQ(state.deadline(true, 10ms, 30ms), start + 30ms);
    const auto connection = state.accept();
    EXPECT_FALSE(state.deadline(false, 10ms, 30ms));
    ASSERT_TRUE(state.close(connection.connection_id, start + 4ms));
    EXPECT_EQ(state.deadline(true, 10ms, 30ms), start + 34ms);
}

} // namespace
} // namespace cha::web
