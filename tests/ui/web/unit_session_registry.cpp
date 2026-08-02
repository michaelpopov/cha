#include "ui/web/session_registry.h"
#include "session/session_lease.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace cha::web {
namespace {

using namespace std::chrono_literals;

class IdleController final : public WebSessionController {
public:
    SessionUpdate handle_raw_input(std::string_view, std::string) override { return {}; }
    SessionUpdate request_stop() override { return {}; }
    SessionUpdate set_default_agent_id(std::string_view) override { return {}; }
    SessionEventBatch receive(std::size_t) override { return {}; }
    [[nodiscard]] bool is_generating() const override { return false; }
    void shutdown() override { ++shutdowns; }
    static inline std::atomic<int> shutdowns{};
};

ErrorCode code_of(const RegistryOpenResult& value) {
    return std::get<Error>(value).code;
}

class ShutdownGate {
public:
    void wait() {
        std::unique_lock lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return released_; });
    }

    bool wait_until_entered() {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 500ms, [this] { return entered_; });
    }

    void release() {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_{};
    bool released_{};
};

class ReleaseGateOnExit {
public:
    explicit ReleaseGateOnExit(ShutdownGate& gate) : gate_(gate) {}
    ~ReleaseGateOnExit() { gate_.release(); }

private:
    ShutdownGate& gate_;
};

class ThreadExitBlocker {
public:
    ~ThreadExitBlocker() {
        if (gate_) gate_->wait();
    }

    void block_on_exit(ShutdownGate& gate) { gate_ = &gate; }

private:
    ShutdownGate* gate_{};
};

thread_local ThreadExitBlocker thread_exit_blocker;

class GatedShutdownController final : public WebSessionController {
public:
    explicit GatedShutdownController(ShutdownGate& gate) : gate_(gate) {}
    SessionUpdate handle_raw_input(std::string_view, std::string) override { return {}; }
    SessionUpdate request_stop() override { return {}; }
    SessionUpdate set_default_agent_id(std::string_view) override { return {}; }
    SessionEventBatch receive(std::size_t) override { return {}; }
    [[nodiscard]] bool is_generating() const override { return false; }
    void shutdown() override { gate_.wait(); }

private:
    ShutdownGate& gate_;
};

class LeaseTrackingController final : public WebSessionController {
public:
    explicit LeaseTrackingController(std::atomic<bool>& lease_held)
        : lease_held_(lease_held) {}
    ~LeaseTrackingController() override { lease_held_ = false; }

    SessionUpdate handle_raw_input(std::string_view, std::string) override { return {}; }
    SessionUpdate request_stop() override { return {}; }
    SessionUpdate set_default_agent_id(std::string_view) override { return {}; }
    SessionEventBatch receive(std::size_t) override { return {}; }
    [[nodiscard]] bool is_generating() const override { return false; }
    void shutdown() override {}

private:
    std::atomic<bool>& lease_held_;
};

class TemporaryLeasePath {
public:
    TemporaryLeasePath()
        : path(std::filesystem::temp_directory_path()
               / ("cha_registry_failed_open_"
                  + std::to_string(std::chrono::steady_clock::now()
                                       .time_since_epoch()
                                       .count())
                  + ".sqlite3")) {}
    ~TemporaryLeasePath() {
        std::error_code ignored;
        std::filesystem::remove(SessionLease::companion_path(path), ignored);
    }

    std::filesystem::path path;
};

TEST(SessionRegistry, ReusesRunningSessionAndReturnsHandle) {
    std::atomic<int> starts{};
    SessionRegistry registry({.session_limit = 2}, [&starts](const SessionKey&, WakeNotifier&) {
        ++starts;
        return std::make_unique<IdleController>();
    });
    const SessionKey key{"forum", "session"};
    EXPECT_FALSE(registry.try_reattach(key));

    const auto first = registry.open(key, 500ms);
    const auto second = registry.open(key, 500ms);
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(first));
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(second));
    EXPECT_EQ(std::get<OpenSessionSuccess>(first).path, "/s/forum/session/");
    EXPECT_EQ(starts, 1);
    EXPECT_TRUE(registry.lookup(key));
    const auto reattached = registry.try_reattach(key);
    ASSERT_TRUE(reattached);
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(*reattached));
    EXPECT_EQ(
        std::get<OpenSessionSuccess>(*reattached).path,
        "/s/forum/session/");

    registry.begin_shutdown();
    const auto stopping = registry.try_reattach(key);
    ASSERT_TRUE(stopping);
    EXPECT_EQ(code_of(*stopping), ErrorCode::server_stopping);
    registry.sweep();
}

TEST(SessionRegistry, RejectsUrlUnsafeKeysBeforeStartingAnOwner) {
    std::atomic<int> starts{};
    SessionRegistry registry({.session_limit = 2}, [&starts](const SessionKey&, WakeNotifier&) {
        ++starts;
        return std::make_unique<IdleController>();
    });

    EXPECT_EQ(
        code_of(registry.open({"bad#forum", "session"}, 500ms)),
        ErrorCode::not_found);
    EXPECT_EQ(
        code_of(registry.open({"forum", "bad?session"}, 500ms)),
        ErrorCode::not_found);
    const auto reattached = registry.try_reattach({"forum", "bad%session"});
    ASSERT_TRUE(reattached);
    EXPECT_EQ(code_of(*reattached), ErrorCode::not_found);
    EXPECT_FALSE(registry.lookup({"forum", "bad session"}));
    EXPECT_EQ(starts, 0);
}

TEST(SessionRegistry, BusyAndLimitHaveStableErrors) {
    std::atomic<int> busy_attempts{};
    SessionRegistry busy({.session_limit = 1}, [&](const SessionKey&, WakeNotifier&) {
        if (++busy_attempts == 1) throw SessionBusyError("busy");
        return std::make_unique<IdleController>();
    });
    EXPECT_EQ(code_of(busy.open({"f", "s"}, 500ms)), ErrorCode::session_busy);
    busy.sweep();
    EXPECT_TRUE(std::holds_alternative<OpenSessionSuccess>(
        busy.open({"f", "s"}, 500ms)));
    EXPECT_EQ(busy_attempts, 2);
    busy.begin_shutdown();

    std::mutex mutex;
    std::condition_variable entered;
    bool release{};
    bool started{};
    SessionRegistry limit({.session_limit = 1}, [&](const SessionKey&, WakeNotifier&) {
        std::unique_lock lock(mutex);
        started = true;
        entered.notify_all();
        entered.wait(lock, [&] { return release; });
        return std::make_unique<IdleController>();
    });
    std::thread opener([&] { (void)limit.open({"f", "one"}, 1s); });
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(entered.wait_for(lock, 500ms, [&] { return started; }));
    }
    EXPECT_EQ(code_of(limit.open({"f", "two"}, 10ms)), ErrorCode::session_limit_reached);
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    entered.notify_all();
    opener.join();
    limit.begin_shutdown();
}

TEST(SessionRegistry, SimultaneousDistinctOpensNeverExceedLimit) {
    constexpr int request_count = 32;
    constexpr int session_limit = 4;
    std::mutex mutex;
    std::condition_variable changed;
    int active{};
    int peak{};
    int started{};
    bool release{};
    SessionRegistry registry({.session_limit = session_limit},
        [&](const SessionKey&, WakeNotifier&) {
            std::unique_lock lock(mutex);
            ++active;
            ++started;
            peak = std::max(peak, active);
            changed.notify_all();
            changed.wait(lock, [&] { return release; });
            --active;
            return std::make_unique<IdleController>();
        });

    std::promise<void> start_all;
    const std::shared_future<void> start = start_all.get_future().share();
    std::vector<std::future<RegistryOpenResult>> opens;
    opens.reserve(request_count);
    for (int index = 0; index < request_count; ++index) {
        opens.push_back(std::async(std::launch::async, [&, index] {
            start.wait();
            return registry.open(
                {"f", "session-" + std::to_string(index)}, 1s);
        }));
    }
    start_all.set_value();

    bool filled_limit = false;
    {
        std::unique_lock lock(mutex);
        filled_limit = changed.wait_for(lock, 1s, [&] {
            return started == session_limit;
        });
        release = true;
    }
    changed.notify_all();
    EXPECT_TRUE(filled_limit);

    int successes{};
    int rejected{};
    for (auto& open : opens) {
        RegistryOpenResult result = open.get();
        if (std::holds_alternative<OpenSessionSuccess>(result)) {
            ++successes;
        } else {
            EXPECT_EQ(code_of(result), ErrorCode::session_limit_reached);
            ++rejected;
        }
    }
    EXPECT_EQ(successes, session_limit);
    EXPECT_EQ(rejected, request_count - session_limit);
    EXPECT_EQ(started, session_limit);
    EXPECT_EQ(peak, session_limit);
    registry.begin_shutdown();
}

TEST(SessionRegistry, ConcurrentSameKeyOpensShareOneOwnerAndOutcome) {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{};
    bool release{};
    std::atomic<int> starts{};
    SessionRegistry registry({.session_limit = 2}, [&](const SessionKey&, WakeNotifier&) {
        ++starts;
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return release; });
        return std::make_unique<IdleController>();
    });
    const SessionKey key{"f", "same"};
    auto first = std::async(std::launch::async, [&] { return registry.open(key, 1s); });
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, 500ms, [&] { return entered; }));
    }
    EXPECT_FALSE(registry.lookup(key));
    auto second = std::async(std::launch::async, [&] { return registry.open(key, 1s); });
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();
    EXPECT_TRUE(std::holds_alternative<OpenSessionSuccess>(first.get()));
    EXPECT_TRUE(std::holds_alternative<OpenSessionSuccess>(second.get()));
    EXPECT_EQ(starts, 1);
    registry.begin_shutdown();
}

TEST(SessionRegistry, ConcurrentDifferentKeyOpensProceedIndependently) {
    std::mutex mutex;
    std::condition_variable changed;
    int entered{};
    SessionRegistry registry({.session_limit = 2}, [&](const SessionKey&, WakeNotifier&) {
        std::unique_lock lock(mutex);
        ++entered;
        changed.notify_all();
        changed.wait(lock, [&] { return entered == 2; });
        changed.notify_all();
        return std::make_unique<IdleController>();
    });
    auto first = std::async(std::launch::async, [&] { return registry.open({"f", "one"}, 1s); });
    auto second = std::async(std::launch::async, [&] { return registry.open({"f", "two"}, 1s); });
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, 500ms, [&] { return entered == 2; }));
        changed.notify_all();
    }
    EXPECT_TRUE(std::holds_alternative<OpenSessionSuccess>(first.get()));
    EXPECT_TRUE(std::holds_alternative<OpenSessionSuccess>(second.get()));
    registry.begin_shutdown();
}

TEST(SessionRegistry, FailedOpenIsSweptAndCanBeRetried) {
    std::atomic<int> attempts{};
    SessionRegistry registry({.session_limit = 1}, [&](const SessionKey&, WakeNotifier&) {
        if (++attempts == 1) throw std::runtime_error("open failed");
        return std::make_unique<IdleController>();
    });
    const SessionKey key{"f", "retry"};
    EXPECT_EQ(code_of(registry.open(key, 500ms)), ErrorCode::internal_error);
    EXPECT_TRUE(std::holds_alternative<OpenSessionSuccess>(registry.open(key, 500ms)));
    EXPECT_EQ(attempts, 2);
    registry.begin_shutdown();
}

TEST(SessionRegistry, FactoryFailureReleasesOpenHandoffLease) {
    TemporaryLeasePath temporary;
    SessionRegistry registry(
        {.session_limit = 1},
        [path = temporary.path](const SessionKey&, WakeNotifier&)
            -> std::unique_ptr<WebSessionController> {
            SessionLease lease = SessionLease::acquire(path);
            if (!lease.active()) {
                throw std::logic_error("factory did not acquire its lease");
            }
            throw std::runtime_error("injected factory failure");
        });

    EXPECT_EQ(
        code_of(registry.open({"f", "failed-lease"}, 500ms)),
        ErrorCode::internal_error);
    SessionLease reopened = SessionLease::acquire(temporary.path);
    EXPECT_TRUE(reopened.active());
    registry.begin_shutdown();
}

TEST(SessionRegistry, TimeoutDoesNotCancelTheOpen) {
    std::mutex mutex;
    std::condition_variable changed;
    bool release{};
    std::atomic<int> starts{};
    SessionRegistry registry({.session_limit = 1}, [&](const SessionKey&, WakeNotifier&) {
        ++starts;
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return release; });
        return std::make_unique<IdleController>();
    });
    const SessionKey key{"f", "slow"};
    EXPECT_EQ(code_of(registry.open(key, 10ms)), ErrorCode::session_open_timeout);
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();
    EXPECT_TRUE(std::holds_alternative<OpenSessionSuccess>(registry.open(key, 500ms)));
    EXPECT_EQ(starts, 1);
    registry.begin_shutdown();
}

TEST(SessionRegistry, WaitersHaveIndependentDeadlines) {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{};
    bool release{};
    SessionRegistry registry({.session_limit = 1}, [&](const SessionKey&, WakeNotifier&) {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return release; });
        return std::make_unique<IdleController>();
    });
    const SessionKey key{"f", "deadlines"};
    auto short_waiter = std::async(std::launch::async, [&] { return registry.open(key, 10ms); });
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, 500ms, [&] { return entered; }));
    }
    auto long_waiter = std::async(std::launch::async, [&] { return registry.open(key, 500ms); });
    ASSERT_EQ(short_waiter.wait_for(500ms), std::future_status::ready);
    EXPECT_EQ(code_of(short_waiter.get()), ErrorCode::session_open_timeout);
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();
    EXPECT_TRUE(std::holds_alternative<OpenSessionSuccess>(long_waiter.get()));
    registry.begin_shutdown();
}

TEST(SessionRegistry, StoppingEntryRejectsOpenConsumesCapacityAndLateHandleStops) {
    ShutdownGate gate;
    ReleaseGateOnExit release_gate(gate);
    std::atomic<int> starts{};
    SessionRegistry registry({.session_limit = 1}, [&](const SessionKey&, WakeNotifier&) {
        ++starts;
        return std::make_unique<GatedShutdownController>(gate);
    });
    const SessionKey key{"f", "stopping"};
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(registry.open(key, 500ms)));
    SessionHandle handle = registry.lookup(key);
    ASSERT_TRUE(handle);
    handle.runtime().request_shutdown();
    ASSERT_TRUE(gate.wait_until_entered());
    EXPECT_EQ(code_of(registry.open(key, 10ms)), ErrorCode::session_stopping);
    EXPECT_EQ(code_of(registry.open({"f", "other"}, 10ms)), ErrorCode::session_limit_reached);
    gate.release();
    registry.sweep();
    EXPECT_FALSE(registry.lookup(key));
    // The map no longer owns the runtime, but this in-flight request handle
    // keeps it alive and sees the already-stopping session.
    const auto stopped = handle.runtime().submit(RawCommand{"reader", "ignored"}, 10ms);
    EXPECT_EQ(std::get<ErrorCode>(stopped), ErrorCode::session_not_live);
    EXPECT_EQ(starts, 1);
}

TEST(SessionRegistry, ShutdownExposesUnfinishedOwnersWithoutCompletingStartup) {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{};
    bool release{};
    SessionRegistry registry({.session_limit = 1}, [&](const SessionKey&, WakeNotifier&) {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return release; });
        return std::make_unique<IdleController>();
    });
    const SessionKey key{"f", "unfinished"};
    auto waiter = std::async(std::launch::async, [&] { return registry.open(key, 5s); });
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, 500ms, [&] { return entered; }));
    }
    registry.begin_shutdown();
    EXPECT_EQ(registry.unfinished_owners(), std::vector<SessionKey>{key});
    ASSERT_EQ(waiter.wait_for(500ms), std::future_status::ready);
    EXPECT_EQ(code_of(waiter.get()), ErrorCode::server_stopping);
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();
}

TEST(SessionRegistry, ShutdownWakesStartingWaitersWithoutWritingTheirResult) {
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{};
    bool release{};
    SessionRegistry registry({.session_limit = 1}, [&](const SessionKey&, WakeNotifier&) {
        std::unique_lock lock(mutex);
        entered = true;
        changed.notify_all();
        changed.wait(lock, [&] { return release; });
        return std::make_unique<IdleController>();
    });
    auto waiter = std::async(std::launch::async, [&] {
        return registry.open({"f", "stopping"}, 5s);
    });
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, 500ms, [&] { return entered; }));
    }
    registry.begin_shutdown();
    ASSERT_EQ(waiter.wait_for(500ms), std::future_status::ready);
    EXPECT_EQ(code_of(waiter.get()), ErrorCode::server_stopping);
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();
}

TEST(SessionRegistry, ShutdownJoinUsesOneBoundedGracePeriod) {
    ShutdownGate gate;
    ReleaseGateOnExit release_gate(gate);
    SessionRegistry registry({.session_limit = 1}, [&](const SessionKey&, WakeNotifier&) {
        return std::make_unique<GatedShutdownController>(gate);
    });
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
        registry.open({"f", "blocked"}, 500ms)));

    registry.begin_shutdown();
    ASSERT_TRUE(gate.wait_until_entered());
    EXPECT_FALSE(registry.join_shutdown(10ms));
    const std::vector<SessionKey> expected_unfinished{{"f", "blocked"}};
    EXPECT_EQ(registry.unfinished_owners(), expected_unfinished);

    gate.release();
    EXPECT_TRUE(registry.join_shutdown(500ms));
    EXPECT_TRUE(registry.unfinished_owners().empty());
}

TEST(SessionRegistry, ShutdownAtCommitNeverPublishesAndTearsDownNewController) {
    std::atomic<int> factory_calls{};
    std::atomic<int> shutdowns{};
    std::promise<void> controller_shutdown;
    auto shutdown_complete = controller_shutdown.get_future();
    SessionRegistry* registry_pointer = nullptr;
    SessionRegistry registry({.session_limit = 1}, [&](const SessionKey&, WakeNotifier&) {
        ++factory_calls;
        registry_pointer->begin_shutdown();
        class ShutdownReportingController final : public WebSessionController {
        public:
            ShutdownReportingController(
                std::atomic<int>& shutdowns,
                std::promise<void>& shutdown_complete)
                : shutdowns_(shutdowns), shutdown_complete_(shutdown_complete) {}

            SessionUpdate handle_raw_input(std::string_view, std::string) override { return {}; }
            SessionUpdate request_stop() override { return {}; }
            SessionUpdate set_default_agent_id(std::string_view) override { return {}; }
            SessionEventBatch receive(std::size_t) override { return {}; }
            [[nodiscard]] bool is_generating() const override { return false; }
            void shutdown() override {
                ++shutdowns_;
                shutdown_complete_.set_value();
            }

        private:
            std::atomic<int>& shutdowns_;
            std::promise<void>& shutdown_complete_;
        };
        return std::make_unique<ShutdownReportingController>(
            shutdowns, controller_shutdown);
    });
    registry_pointer = &registry;
    const SessionKey key{"f", "commit-race"};

    EXPECT_EQ(code_of(registry.open(key, 500ms)), ErrorCode::server_stopping);
    ASSERT_EQ(shutdown_complete.wait_for(500ms), std::future_status::ready);
    EXPECT_EQ(factory_calls, 1);
    EXPECT_EQ(shutdowns, 1);
    EXPECT_FALSE(registry.lookup(key));
}

TEST(SessionRegistry, ReopensSameKeyWhileFinishedOwnerIsBeingJoined) {
    ShutdownGate old_thread_exit;
    std::atomic<bool> lease_held{};
    std::atomic<int> original_starts{};
    std::atomic<int> lease_conflicts{};
    std::promise<void> auxiliary_started;
    auto auxiliary_started_future = auxiliary_started.get_future();
    const SessionKey original{"f", "reopen"};
    const SessionKey auxiliary{"f", "sweeper"};
    SessionRegistry registry({.session_limit = 2}, [&](const SessionKey& key, WakeNotifier&) {
        if (key == auxiliary) {
            auxiliary_started.set_value();
            return std::unique_ptr<WebSessionController>(
                std::make_unique<IdleController>());
        }

        const int attempt = original_starts++;
        if (lease_held.exchange(true)) {
            ++lease_conflicts;
            throw SessionBusyError("test lease is still held");
        }
        if (attempt == 0) thread_exit_blocker.block_on_exit(old_thread_exit);
        return std::unique_ptr<WebSessionController>(
            std::make_unique<LeaseTrackingController>(lease_held));
    });
    ReleaseGateOnExit release_old_thread(old_thread_exit);

    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
        registry.open(original, 500ms)));
    SessionHandle old_handle = registry.lookup(original);
    ASSERT_TRUE(old_handle);
    old_handle.runtime().request_shutdown();
    ASSERT_TRUE(old_thread_exit.wait_until_entered());

    // This open removes the finished original entry under the mutex, starts an
    // unrelated owner, and then blocks outside the mutex joining the original
    // owner at its thread-exit gate.
    auto joining_open = std::async(std::launch::async, [&] {
        return registry.open(auxiliary, 500ms);
    });
    ReleaseGateOnExit release_before_future_join(old_thread_exit);
    ASSERT_EQ(
        auxiliary_started_future.wait_for(500ms),
        std::future_status::ready);
    EXPECT_EQ(joining_open.wait_for(20ms), std::future_status::timeout);

    // The old controller released its simulated lease before marking itself
    // finished. The old map entry is already gone, so the same key can be
    // admitted and opened while the other request remains blocked in join().
    EXPECT_TRUE(std::holds_alternative<OpenSessionSuccess>(
        registry.open(original, 500ms)));
    EXPECT_EQ(original_starts, 2);
    EXPECT_EQ(lease_conflicts, 0);

    old_thread_exit.release();
    EXPECT_TRUE(std::holds_alternative<OpenSessionSuccess>(joining_open.get()));
    registry.begin_shutdown();
}

TEST(SessionRegistry, RepeatedOpenUnloadCyclesReapOwnersAndReleaseCapacity) {
    constexpr int cycles = 40;
    std::atomic<bool> lease_held{};
    std::atomic<int> starts{};
    std::atomic<int> lease_conflicts{};
    SessionRegistry registry(
        {.session_limit = 1},
        [&](const SessionKey&, WakeNotifier&) {
            ++starts;
            if (lease_held.exchange(true)) {
                ++lease_conflicts;
                throw SessionBusyError("simulated leaked lease");
            }
            return std::make_unique<LeaseTrackingController>(lease_held);
        });
    const SessionKey key{"forum", "cycled"};

    for (int cycle = 0; cycle != cycles; ++cycle) {
        ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
            registry.open(key, 500ms)));
        SessionHandle handle = registry.lookup(key);
        ASSERT_TRUE(handle);
        handle.runtime().request_shutdown();
        handle = {};

        const auto deadline = std::chrono::steady_clock::now() + 500ms;
        while (registry.snapshot().live_entry_count != 0
            && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        EXPECT_EQ(registry.snapshot().live_entry_count, 0U);
        EXPECT_FALSE(lease_held);
    }

    EXPECT_EQ(starts, cycles);
    EXPECT_EQ(lease_conflicts, 0);
}

} // namespace
} // namespace cha::web
