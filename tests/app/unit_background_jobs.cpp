#include "app/background_jobs.h"

#include <gtest/gtest.h>

#include <future>

namespace cha::app {
namespace {

using namespace std::chrono_literals;

TEST(BackgroundJobs, DeadlineLeavesUnfinishedWorkerOwnedAndClosesAdmission) {
    BackgroundJobs jobs;
    std::promise<void> started;
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic_bool saw_cancellation{};
    ASSERT_TRUE(jobs.launch([&](std::atomic_bool& cancelled) {
        started.set_value();
        released.wait(); // Deliberately ignore cancellation until released.
        saw_cancellation = cancelled.load();
    }));
    started.get_future().wait();

    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(jobs.join_until(start + 20ms));
    EXPECT_LT(std::chrono::steady_clock::now() - start, 500ms);
    EXPECT_FALSE(jobs.launch([](std::atomic_bool&) {}));

    // A timed-out worker remains owned and can finish safely before its
    // dependencies go away. Production retains the whole runtime if needed.
    release.set_value();
    EXPECT_TRUE(jobs.join_until(std::chrono::steady_clock::now() + 2s));
    jobs.join();
    EXPECT_TRUE(saw_cancellation.load());
}

TEST(BackgroundJobs, UnboundedJoinRequestsCancellationBeforeWaiting) {
    BackgroundJobs jobs;
    std::atomic_bool finished{};
    ASSERT_TRUE(jobs.launch([&](std::atomic_bool& cancelled) {
        while (!cancelled.load()) std::this_thread::yield();
        finished = true;
    }));
    jobs.join();
    EXPECT_TRUE(finished.load());
    EXPECT_FALSE(jobs.launch([](std::atomic_bool&) {}));
}

} // namespace
} // namespace cha::app
