#include "util/concurrent_queue.h"
#include "util/event_fd_notifier.h"

#include <gtest/gtest.h>

#include <poll.h>

namespace cha {
namespace {

TEST(EventFdNotifier, AcknowledgeThenDrainClearsACoalescedWake) {
    EventFdNotifier notifier;
    ConcurrentQueue<int> queue;

    ASSERT_TRUE(queue.push(1));
    notifier.wake();
    ASSERT_TRUE(queue.push(2));
    notifier.wake();

    pollfd descriptor{notifier.descriptor(), POLLIN, 0};
    ASSERT_EQ(::poll(&descriptor, 1, 0), 1);
    ASSERT_NE(descriptor.revents & POLLIN, 0);

    notifier.acknowledge();
    int value = 0;
    EXPECT_EQ(queue.try_get(value), ChannelReadStatus::value);
    EXPECT_EQ(value, 1);
    EXPECT_EQ(queue.try_get(value), ChannelReadStatus::value);
    EXPECT_EQ(value, 2);
    EXPECT_EQ(queue.try_get(value), ChannelReadStatus::empty);

    descriptor.revents = 0;
    EXPECT_EQ(::poll(&descriptor, 1, 0), 0);
}

} // namespace
} // namespace cha
