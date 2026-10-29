#include "session/summary_prompt_cache.h"
#include "util/logging.h"

#include <gtest/gtest.h>

#include <condition_variable>
#include <future>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

namespace cha {
namespace {
using namespace std::chrono_literals;

TEST(SummaryPromptCache, ReusesExactBytesUntilTenMinutesAndTimestampsAfterRead) {
    auto now = std::chrono::steady_clock::time_point{};
    int reads = 0;
    SummaryPromptCache cache([&](const auto&) -> std::optional<std::string> {
        ++reads;
        now += 1s;
        return "snapshot " + std::to_string(reads);
    }, [&] { return now; });
    std::atomic_bool cancelled{false};
    EXPECT_EQ(cache.load(cancelled), "snapshot 1");
    now += 599999ms;
    EXPECT_EQ(cache.load(cancelled), "snapshot 1");
    EXPECT_EQ(reads, 1);
    now += 1ms;
    EXPECT_EQ(cache.load(cancelled), "snapshot 2");
    EXPECT_EQ(reads, 2);
}

TEST(SummaryPromptCache, EmptySuccessIsCachedAndFailuresNeverServeExpiredContent) {
    auto now = std::chrono::steady_clock::time_point{};
    int reads = 0;
    std::optional<std::string> value = "old";
    SummaryPromptCache cache([&](const auto&) { ++reads; return value; }, [&] { return now; });
    std::atomic_bool cancelled{false};
    EXPECT_EQ(cache.load(cancelled), "old");
    now += 10min;
    value.reset();
    EXPECT_TRUE(cache.load(cancelled).empty());
    EXPECT_TRUE(cache.load(cancelled).empty());
    EXPECT_EQ(reads, 3);
    value = "";
    EXPECT_TRUE(cache.load(cancelled).empty());
    value = "new";
    now += 9min;
    EXPECT_TRUE(cache.load(cancelled).empty());
    EXPECT_EQ(reads, 4);
    now += 1min;
    EXPECT_EQ(cache.load(cancelled), "new");
}

TEST(SummaryPromptCache, PartialCancellationIsNotPublishedButCompletedSnapshotSurvivesLateCancellation) {
    std::atomic_bool cancelled{true};
    int reads = 0;
    bool complete = false;
    SummaryPromptCache cache([&](const auto&) -> std::optional<std::string> {
        ++reads;
        cancelled = true;
        return complete ? std::optional<std::string>{"complete"} : std::nullopt;
    });
    EXPECT_TRUE(cache.load(cancelled).empty());
    EXPECT_EQ(reads, 0);
    cancelled = false;
    EXPECT_TRUE(cache.load(cancelled).empty());
    complete = true;
    cancelled = false;
    EXPECT_EQ(cache.load(cancelled), "complete");
    cancelled = false;
    EXPECT_EQ(cache.load(cancelled), "complete");
    EXPECT_EQ(reads, 2);
}

TEST(SummaryPromptCache, ConcurrentWorkersShareOneRefreshAndSeparateConversationsDoNot) {
    std::atomic_bool cancelled{false};
    std::atomic_int reads{0};
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    SummaryPromptCache cache([&](const auto&) -> std::optional<std::string> {
        ++reads;
        entered.set_value();
        gate.wait();
        return "shared";
    });
    std::vector<std::future<std::string>> workers;
    workers.push_back(std::async(std::launch::async, [&] { return cache.load(cancelled); }));
    entered.get_future().wait();
    for (int i = 0; i < 5; ++i)
        workers.push_back(std::async(std::launch::async, [&] { return cache.load(cancelled); }));
    release.set_value();
    for (auto& worker : workers) EXPECT_EQ(worker.get(), "shared");
    EXPECT_EQ(reads, 1);
    SummaryPromptCache other([&](const auto&) -> std::optional<std::string> { ++reads; return "other"; });
    EXPECT_EQ(other.load(cancelled), "other");
    EXPECT_EQ(reads, 2);
}

TEST(SummaryPromptCache, OnlyUncancelledFailuresLogASafeWarning) {
    const auto path = std::filesystem::temp_directory_path() / ("cha_summary_cache_"
        + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".log");
    initialize_diagnostic_logging(path, "warn");
    std::atomic_bool cancelled{false};
    SummaryPromptCache failed([](const auto&) -> std::optional<std::string> { return std::nullopt; });
    EXPECT_TRUE(failed.load(cancelled).empty());
    SummaryPromptCache partial([&](const auto&) -> std::optional<std::string> {
        cancelled = true; return std::nullopt;
    });
    EXPECT_TRUE(partial.load(cancelled).empty());
    shutdown_diagnostic_logging();
    std::ifstream file(path);
    const std::string log{std::istreambuf_iterator<char>(file), {}};
    constexpr std::string_view warning = "Saved summary catalogue refresh failed";
    const auto at = log.find(warning);
    ASSERT_NE(at, std::string::npos);
    EXPECT_EQ(log.find(warning, at + 1), std::string::npos);
    file.close();
    std::filesystem::remove(path);
}

TEST(SummaryPromptCache, WaiterRechecksCancellationAfterAcquiringMutex) {
    std::atomic_bool active{false}, cancelled{false};
    std::promise<void> entered, release;
    auto gate = release.get_future().share();
    SummaryPromptCache cache([&](const auto&) -> std::optional<std::string> {
        entered.set_value(); gate.wait(); return "snapshot";
    });
    auto owner = std::async(std::launch::async, [&] { return cache.load(active); });
    entered.get_future().wait();
    auto waiter = std::async(std::launch::async, [&] { return cache.load(cancelled); });
    cancelled = true;
    release.set_value();
    EXPECT_EQ(owner.get(), "snapshot");
    EXPECT_TRUE(waiter.get().empty());
    EXPECT_EQ(cache.load(active), "snapshot");
}

} // namespace
} // namespace cha
