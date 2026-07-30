#include "util/thread_pool.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <optional>
#include <stdexcept>
#include <thread>

namespace cha {
namespace {

TEST(ThreadPool, RejectsZeroWorkers) {
    EXPECT_THROW((void)ThreadPool(0), std::invalid_argument);
}

TEST(ThreadPool, ExecutesAcceptedTasksAndDrainsOnStop) {
    ThreadPool pool(2);
    std::atomic_int completed{};
    std::promise<void> ready;
    std::future<void> ready_future = ready.get_future();

    ASSERT_TRUE(pool.submit([&] {
        completed.fetch_add(1, std::memory_order_relaxed);
        ready.set_value();
    }));
    ASSERT_TRUE(pool.submit([&] {
        completed.fetch_add(1, std::memory_order_relaxed);
    }));
    ready_future.wait();
    pool.stop();

    EXPECT_EQ(completed.load(std::memory_order_relaxed), 2);
    EXPECT_FALSE(pool.submit([] {}));
    EXPECT_NO_THROW(pool.stop());
}

TEST(ThreadPool, LimitsConcurrentTasksToItsWidth) {
    using namespace std::chrono_literals;

    ThreadPool pool(2);
    std::atomic_int entered{};
    std::atomic_int active{};
    std::atomic_int maximum{};
    std::atomic_bool release{};
    std::promise<void> two_entered;
    std::future<void> two_entered_future = two_entered.get_future();

    for (int index = 0; index < 3; ++index) {
        ASSERT_TRUE(pool.submit([&] {
            const int now = active.fetch_add(1, std::memory_order_acq_rel) + 1;
            int observed = maximum.load(std::memory_order_relaxed);
            while (observed < now
                   && !maximum.compare_exchange_weak(
                       observed, now, std::memory_order_relaxed)) {
            }
            if (entered.fetch_add(1, std::memory_order_acq_rel) + 1 == 2) {
                two_entered.set_value();
            }
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            active.fetch_sub(1, std::memory_order_acq_rel);
        }));
    }

    ASSERT_EQ(two_entered_future.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(entered.load(std::memory_order_acquire), 2);
    release.store(true, std::memory_order_release);
    pool.stop();
    EXPECT_EQ(maximum.load(std::memory_order_relaxed), 2);
}

TEST(ThreadPool, DestructorStopsAndJoinsAcceptedWork) {
    using namespace std::chrono_literals;

    std::optional<ThreadPool> pool(std::in_place, 1);
    std::atomic_bool release{};
    std::atomic_bool completed{};
    std::promise<void> entered;
    std::future<void> entered_future = entered.get_future();
    ASSERT_TRUE(pool->submit([&] {
        entered.set_value();
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        completed.store(true, std::memory_order_release);
    }));
    ASSERT_EQ(entered_future.wait_for(1s), std::future_status::ready);

    std::future<void> destroying = std::async(std::launch::async, [&pool] {
        pool.reset();
    });
    const std::future_status status = destroying.wait_for(20ms);
    release.store(true, std::memory_order_release);

    EXPECT_EQ(status, std::future_status::timeout);
    destroying.get();
    EXPECT_TRUE(completed.load(std::memory_order_acquire));
}

} // namespace
} // namespace cha
