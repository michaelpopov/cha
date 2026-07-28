#include "util/concurrent_queue.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <optional>
#include <string>

namespace cha {
namespace {

TEST(ConcurrentQueue, ReportsNonBlockingStateAndPreservesOrder) {
    ConcurrentQueue<std::string> queue;
    std::string value;

    EXPECT_EQ(queue.try_get(value), ChannelReadStatus::empty);
    ASSERT_TRUE(queue.push("first"));
    ASSERT_TRUE(queue.push("second"));
    EXPECT_EQ(queue.try_get(value), ChannelReadStatus::value);
    EXPECT_EQ(value, "first");

    queue.close();
    EXPECT_EQ(queue.try_get(value), ChannelReadStatus::value);
    EXPECT_EQ(value, "second");
    EXPECT_EQ(queue.try_get(value), ChannelReadStatus::closed);
    EXPECT_FALSE(queue.push("third"));
}

TEST(ConcurrentQueue, PushWakesABlockedConsumer) {
    using namespace std::chrono_literals;

    ConcurrentQueue<int> queue;
    std::future<std::optional<int>> result = std::async(
        std::launch::async,
        [&queue] { return queue.get(); });
    EXPECT_EQ(result.wait_for(20ms), std::future_status::timeout);

    ASSERT_TRUE(queue.push(7));
    if (result.wait_for(1s) != std::future_status::ready) {
        queue.close();
        result.wait();
        FAIL() << "Timed out waiting for pushed queue value";
        return;
    }
    EXPECT_EQ(result.get(), std::optional<int>{7});
}

TEST(ConcurrentQueue, CloseWakesABlockedConsumerAndIsIdempotent) {
    using namespace std::chrono_literals;

    ConcurrentQueue<int> queue;
    std::future<std::optional<int>> result = std::async(
        std::launch::async,
        [&queue] { return queue.get(); });
    EXPECT_EQ(result.wait_for(20ms), std::future_status::timeout);

    queue.close();
    queue.close();
    if (result.wait_for(1s) != std::future_status::ready) {
        queue.close();
        result.wait();
        FAIL() << "Timed out waiting for closed queue";
        return;
    }
    EXPECT_EQ(result.get(), std::nullopt);
    EXPECT_EQ(queue.get(), std::nullopt);
    EXPECT_FALSE(queue.push(7));
}

TEST(ConcurrentQueue, CloseWithDeliversOneFinalValueAfterQueuedValues) {
    ConcurrentQueue<std::string> queue;
    ASSERT_TRUE(queue.push("first"));
    ASSERT_TRUE(queue.push("second"));

    queue.close_with("terminal");
    queue.close_with("ignored");
    queue.close();

    std::string value;
    ASSERT_EQ(queue.try_get(value), ChannelReadStatus::value);
    EXPECT_EQ(value, "first");
    ASSERT_EQ(queue.try_get(value), ChannelReadStatus::value);
    EXPECT_EQ(value, "second");
    ASSERT_EQ(queue.try_get(value), ChannelReadStatus::value);
    EXPECT_EQ(value, "terminal");
    EXPECT_EQ(queue.try_get(value), ChannelReadStatus::closed);
    EXPECT_FALSE(queue.push("late"));
}

TEST(ConcurrentQueue, CloseWithWakesABlockedConsumerWithTheFinalValue) {
    using namespace std::chrono_literals;

    ConcurrentQueue<int> queue;
    std::future<std::optional<int>> result = std::async(
        std::launch::async,
        [&queue] { return queue.get(); });
    EXPECT_EQ(result.wait_for(20ms), std::future_status::timeout);

    queue.close_with(11);
    if (result.wait_for(1s) != std::future_status::ready) {
        queue.close();
        result.wait();
        FAIL() << "Timed out waiting for final queue value";
        return;
    }
    EXPECT_EQ(result.get(), std::optional<int>{11});
    EXPECT_EQ(queue.get(), std::nullopt);
}

} // namespace
} // namespace cha
