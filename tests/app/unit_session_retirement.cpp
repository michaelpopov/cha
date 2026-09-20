#include "web/live_session_manager.h"

#include "session/not_found_error.h"
#include "support/test_live_session.h"
#include "workspace/builtins.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>

namespace cha {
namespace {

using namespace std::chrono_literals;

app::RuntimeSettings native_manager_settings(std::size_t session_limit) {
    app::RuntimeSettings settings;
    settings.session_limit = session_limit;
    settings.command_queue_capacity = 8;

    settings.monotonic_event_sequence = true;
    return settings;
}

class SessionFiles {
public:
    const std::filesystem::path& path_for(const FullSessionId& key) {
        std::lock_guard lock(mutex_);
        auto found = files_.find(key);
        if (found == files_.end()) {
            found = files_.emplace(
                key,
                std::make_unique<test::TemporarySessionFile>("retire", key)).first;
        }
        return found->second->path();
    }

private:
    std::mutex mutex_;
    std::map<FullSessionId, std::unique_ptr<test::TemporarySessionFile>> files_;
};

SessionOpener test_opener(SessionFiles& files) {
    return [&files](const FullSessionId& identity, std::shared_ptr<WakeNotifier> notifier) {
        return test::open_test_session(
            identity, files.path_for(identity), notifier);
    };
}

bool wait_finished(
    const LiveSessionHandle& session,
    std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (session->lifecycle() == LiveSessionState::finished) return true;
        std::this_thread::sleep_for(1ms);
    }
    return session->lifecycle() == LiveSessionState::finished;
}

TEST(SessionRetirement, VisitsMoreIdleSessionsThanTheActorLimit) {
    SessionFiles files;
    std::mutex mutex;
    std::condition_variable changed;
    bool b_entered{};
    bool release_b{};
    bool a_finished_when_c_opened{};
    LiveSessionHandle first;
    const FullSessionId a{"forum", "a"};
    const FullSessionId b{"forum", "b"};
    const FullSessionId c{"forum", "c"};
    LiveSessionManager manager(
        native_manager_settings(2),
        [&](const FullSessionId& identity, std::shared_ptr<WakeNotifier> notifier) {
            if (identity == b) {
                std::unique_lock lock(mutex);
                b_entered = true;
                changed.notify_all();
                changed.wait(lock, [&] { return release_b; });
            } else if (identity == c) {
                a_finished_when_c_opened = first
                    && first->lifecycle() == LiveSessionState::finished;
            }
            return test::open_test_session(
                identity, files.path_for(identity), notifier);
        });

    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.select(a, 2s)));
    first = manager.lookup(a);
    ASSERT_TRUE(first);
    auto selecting_b = std::async(std::launch::async, [&] {
        return manager.select(b, 2s);
    });
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, 2s, [&] { return b_entered; }));
    }
    auto selecting_c = std::async(std::launch::async, [&] {
        return manager.select(c, 2s);
    });
    EXPECT_EQ(selecting_c.wait_for(20ms), std::future_status::timeout);
    {
        std::lock_guard lock(mutex);
        release_b = true;
    }
    changed.notify_all();

    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(selecting_b.get()));
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(selecting_c.get()));
    EXPECT_TRUE(a_finished_when_c_opened);
    EXPECT_EQ(first->lifecycle(), LiveSessionState::finished);
    EXPECT_EQ(manager.selected(), c);
    EXPECT_FALSE(manager.lookup(a));
    EXPECT_EQ(manager.snapshot().live_session_count, 1U);
}

TEST(SessionRetirement, FailedOpenLeavesPreviousSelection) {
    SessionFiles files;
    auto opener = [&files](
                      const FullSessionId& identity,
                      std::shared_ptr<WakeNotifier> notifier) {
        if (identity.session_id == "missing") {
            throw SessionNotFoundError("missing");
        }
        return test::open_test_session(
            identity, files.path_for(identity), notifier);
    };
    LiveSessionManager manager(native_manager_settings(2), opener);
    const FullSessionId a{"forum", "a"};
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.select(a, 2s)));

    const auto missing = manager.select({"forum", "missing"}, 2s);
    ASSERT_TRUE(std::holds_alternative<LiveSessionOpenFailure>(missing));
    EXPECT_EQ(
        std::get<LiveSessionOpenFailure>(missing),
        LiveSessionOpenFailure::not_found);
    EXPECT_EQ(manager.selected(), a);
}

TEST(SessionRetirement, TimedOutOpenLeavesPreviousSelection) {
    SessionFiles files;
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{};
    bool release{};
    auto opener = [&](const FullSessionId& identity,
                      std::shared_ptr<WakeNotifier> notifier) {
        if (identity.session_id == "slow") {
            std::unique_lock lock(mutex);
            entered = true;
            changed.notify_all();
            changed.wait(lock, [&] { return release; });
        }
        return test::open_test_session(
            identity, files.path_for(identity), notifier);
    };
    LiveSessionManager manager(native_manager_settings(2), opener);
    const FullSessionId selected{"forum", "selected"};
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(
        manager.select(selected, 2s)));

    auto opening = std::async(std::launch::async, [&] {
        return manager.select({"forum", "slow"}, 20ms);
    });
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, 2s, [&] { return entered; }));
    }
    ASSERT_EQ(opening.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(
        std::get<LiveSessionOpenFailure>(opening.get()),
        LiveSessionOpenFailure::open_timeout);
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();
    EXPECT_EQ(manager.selected(), selected);
}

TEST(SessionRetirement, BusyDeselectedGenerationFinishesThenRetires) {
    SessionFiles files;
    auto controls = std::make_shared<test::BackendControls>();
    auto opener = [&files, controls](
                      const FullSessionId& identity,
                      std::shared_ptr<WakeNotifier> notifier) {
        return test::open_scripted_session(
            identity, files.path_for(identity), notifier, controls);
    };
    LiveSessionManager manager(native_manager_settings(2), opener);
    const FullSessionId a{"forum", "a"};
    const FullSessionId b{"forum", "b"};
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.select(a, 2s)));
    LiveSessionHandle first = manager.lookup(a);
    ASSERT_TRUE(first);
    ASSERT_TRUE(std::holds_alternative<CommandResult>(
        first->submit(RawCommand{"Question"}, 2s)));
    ASSERT_TRUE(controls->wait_until_running());
    EXPECT_FALSE(first->idle_for_retirement());

    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.select(b, 2s)));
    EXPECT_EQ(first->lifecycle(), LiveSessionState::running);
    controls->finish();
    EXPECT_TRUE(wait_finished(first));
    EXPECT_FALSE(manager.lookup(a));
}

TEST(SessionRetirement, BusyActorsAtLimitAreNotCancelled) {
    SessionFiles files;
    auto controls_a = std::make_shared<test::BackendControls>();
    auto controls_b = std::make_shared<test::BackendControls>();
    auto opener = [&files, controls_a, controls_b](
                      const FullSessionId& identity,
                      std::shared_ptr<WakeNotifier> notifier) {
        return test::open_scripted_session(
            identity,
            files.path_for(identity),
            notifier,
            identity.session_id == "a" ? controls_a : controls_b);
    };
    LiveSessionManager manager(native_manager_settings(2), opener);
    const FullSessionId a{"forum", "a"};
    const FullSessionId b{"forum", "b"};
    const FullSessionId c{"forum", "c"};
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.select(a, 2s)));
    LiveSessionHandle first = manager.lookup(a);
    ASSERT_TRUE(first);
    ASSERT_TRUE(std::holds_alternative<CommandResult>(
        first->submit(RawCommand{"Question"}, 2s)));
    ASSERT_TRUE(controls_a->wait_until_running());

    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.select(b, 2s)));
    LiveSessionHandle second = manager.lookup(b);
    ASSERT_TRUE(second);
    ASSERT_TRUE(std::holds_alternative<CommandResult>(
        second->submit(RawCommand{"Question"}, 2s)));
    ASSERT_TRUE(controls_b->wait_until_running());
    EXPECT_EQ(first->lifecycle(), LiveSessionState::running);

    const auto third = manager.select(c, 200ms);
    ASSERT_TRUE(std::holds_alternative<LiveSessionOpenFailure>(third));
    EXPECT_EQ(
        std::get<LiveSessionOpenFailure>(third),
        LiveSessionOpenFailure::limit_reached);
    EXPECT_EQ(first->lifecycle(), LiveSessionState::running);
    EXPECT_EQ(second->lifecycle(), LiveSessionState::running);
    EXPECT_EQ(manager.selected(), b);
    controls_a->finish();
    controls_b->finish();
    EXPECT_TRUE(wait_finished(first));
    EXPECT_FALSE(wait_finished(second, 200ms));
    EXPECT_EQ(second->lifecycle(), LiveSessionState::running);
    EXPECT_EQ(manager.selected(), b);
    EXPECT_TRUE(manager.lookup(b));
}

TEST(SessionRetirement, ReselectCancelsIdleRetirement) {
    SessionFiles files;
    LiveSessionManager manager(native_manager_settings(2), test_opener(files));
    const FullSessionId a{"forum", "a"};
    const FullSessionId b{"forum", "b"};
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.select(a, 2s)));
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.select(b, 2s)));
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.select(a, 2s)));
    EXPECT_EQ(manager.selected(), a);
    EXPECT_TRUE(manager.lookup(a));
}

} // namespace
} // namespace cha
