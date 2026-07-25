#include "util/event_channel.h"

#include <gtest/gtest.h>

#include <future>
#include <optional>
#include <string>
#include <thread>

namespace cha {
namespace {

TEST(EventChannel, DeliversTypedValuesInOrder) {
    EventChannel<std::string> channel;
    EXPECT_TRUE(channel.push("first"));
    EXPECT_TRUE(channel.push("second"));

    EXPECT_EQ(channel.get(), std::optional<std::string>{"first"});
    EXPECT_EQ(channel.get(), std::optional<std::string>{"second"});
}

TEST(EventChannel, CloseUnblocksAWaitingReaderAndRemainsClosed) {
    EventChannel<int> channel;
    std::promise<std::optional<int>> result_promise;
    auto result = result_promise.get_future();
    std::thread reader([&] { result_promise.set_value(channel.get()); });

    channel.close();
    EXPECT_EQ(result.get(), std::nullopt);
    reader.join();

    EXPECT_EQ(channel.get(), std::nullopt);
    EXPECT_FALSE(channel.push(7));
    int value = 0;
    EXPECT_EQ(channel.try_get(value), ChannelReadStatus::closed);
}

TEST(EventChannel, DrainsQueuedValuesBeforeReportingClosed) {
    EventChannel<int> channel;
    ASSERT_TRUE(channel.push(1));
    channel.close();

    EXPECT_EQ(channel.get(), std::optional<int>{1});
    EXPECT_EQ(channel.get(), std::nullopt);
}

} // namespace
} // namespace cha
