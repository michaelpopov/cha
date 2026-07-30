#include "util/concurrent_queue.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <optional>
#include <string>
#include <vector>

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

TEST(ConcurrentQueue, CloseWakesEveryBlockedConsumer) {
    using namespace std::chrono_literals;

    ConcurrentQueue<int> queue;
    std::vector<std::future<std::optional<int>>> consumers;
    consumers.reserve(4);
    for (int index = 0; index < 4; ++index) {
        consumers.push_back(std::async(
            std::launch::async,
            [&queue] { return queue.get(); }));
    }
    for (auto& consumer : consumers) {
        EXPECT_EQ(consumer.wait_for(20ms), std::future_status::timeout);
    }

    queue.close();

    for (auto& consumer : consumers) {
        ASSERT_EQ(consumer.wait_for(1s), std::future_status::ready);
        EXPECT_EQ(consumer.get(), std::nullopt);
    }
}

TEST(ConcurrentQueue, CloseDrainsQueuedValuesBeforeConsumersObserveClosure) {
    using namespace std::chrono_literals;

    ConcurrentQueue<int> queue;
    ASSERT_TRUE(queue.push(1));
    ASSERT_TRUE(queue.push(2));
    std::vector<std::future<std::optional<int>>> consumers;
    consumers.reserve(4);
    for (int index = 0; index < 4; ++index) {
        consumers.push_back(std::async(
            std::launch::async,
            [&queue] { return queue.get(); }));
    }
    queue.close();

    std::vector<int> values;
    std::size_t closed_consumers{};
    for (auto& consumer : consumers) {
        ASSERT_EQ(consumer.wait_for(1s), std::future_status::ready);
        const std::optional<int> value = consumer.get();
        if (value) {
            values.push_back(*value);
        } else {
            ++closed_consumers;
        }
    }
    std::sort(values.begin(), values.end());
    EXPECT_EQ(values, (std::vector<int>{1, 2}));
    EXPECT_EQ(closed_consumers, 2U);
}

TEST(ConcurrentQueue, CloseWithDeliversItsTerminalValueToOnlyOneConsumer) {
    using namespace std::chrono_literals;

    ConcurrentQueue<int> queue;
    std::vector<std::future<std::optional<int>>> consumers;
    consumers.reserve(3);
    for (int index = 0; index < 3; ++index) {
        consumers.push_back(std::async(
            std::launch::async,
            [&queue] { return queue.get(); }));
    }
    for (auto& consumer : consumers) {
        EXPECT_EQ(consumer.wait_for(20ms), std::future_status::timeout);
    }

    queue.close_with(11);

    std::size_t terminal_values{};
    std::size_t closed_consumers{};
    for (auto& consumer : consumers) {
        ASSERT_EQ(consumer.wait_for(1s), std::future_status::ready);
        const std::optional<int> value = consumer.get();
        if (value) {
            EXPECT_EQ(*value, 11);
            ++terminal_values;
        } else {
            ++closed_consumers;
        }
    }
    EXPECT_EQ(terminal_values, 1U);
    EXPECT_EQ(closed_consumers, 2U);
}

} // namespace
} // namespace cha
