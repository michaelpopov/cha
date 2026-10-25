#include "app/background_jobs.h"

#include <gtest/gtest.h>

#include <future>

namespace cha::app {
namespace {

using namespace std::chrono_literals;

TEST(BackgroundJobs, SharedFlagIsUsedByWorkerAndShutdown) {
    for (const bool bounded : {false, true}) {
        BackgroundJobs jobs;
        auto cancel = std::make_shared<std::atomic_bool>(false);
        std::promise<void> started;
        std::atomic_bool same_flag{};
        ASSERT_TRUE(jobs.launch([&](std::atomic_bool& flag) {
            same_flag = &flag == cancel.get();
            started.set_value();
            while (!flag.load()) std::this_thread::yield();
        }, cancel));
        started.get_future().wait();
        if (bounded) EXPECT_TRUE(jobs.join_until(std::chrono::steady_clock::now() + 2s));
        else jobs.join();
        EXPECT_TRUE(same_flag.load());
        EXPECT_TRUE(cancel->load());
    }
}

TEST(BackgroundJobs, CallerCanCancelWithoutClosingAdmission) {
    BackgroundJobs jobs;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    std::promise<void> finished;
    ASSERT_TRUE(jobs.launch([&](std::atomic_bool& flag) {
        while (!flag.load()) std::this_thread::yield();
        finished.set_value();
    }, cancel));
    cancel->store(true);
    EXPECT_EQ(finished.get_future().wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(jobs.launch([](std::atomic_bool&) {}));
    jobs.join();
}

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
