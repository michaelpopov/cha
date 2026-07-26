#include "util/uv_event_loop.h"

#include <gtest/gtest.h>

#include <thread>

namespace cha {
namespace {

TEST(UvEventLoop, DeliversAndCoalescesCrossThreadNotifications) {
    UvEventLoop event_loop;

    std::thread producer([&event_loop] {
        event_loop.wake();
        event_loop.wake();
    });
    producer.join();

    event_loop.run_once();

    EXPECT_TRUE(event_loop.take_notification());
    EXPECT_FALSE(event_loop.take_notification());
}

} // namespace
} // namespace cha
