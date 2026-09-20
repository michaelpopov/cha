#include "web/live_session_manager.h"

#include "session/not_found_error.h"
#include "support/test_live_session.h"
#include "workspace/builtins.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>

namespace cha::web {
namespace {

using namespace std::chrono_literals;

cha::app::RuntimeSettings native_manager_settings(std::size_t session_limit) {
    cha::app::RuntimeSettings settings;
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
    LiveSessionManager manager(native_manager_settings(2), test_opener(files));
    const FullSessionId a{"forum", "a"};
    const FullSessionId b{"forum", "b"};
    const FullSessionId c{"forum", "c"};

    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.select(a, 2s)));
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.select(b, 2s)));
    EXPECT_EQ(manager.selected(), b);

    const LiveSessionHandle first = manager.lookup(a);
    ASSERT_TRUE(first);
    EXPECT_TRUE(wait_finished(first));

    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.select(c, 2s)));
    EXPECT_EQ(manager.selected(), c);
    EXPECT_FALSE(manager.lookup(a));
    EXPECT_EQ(manager.snapshot().live_session_count, 2U);
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
} // namespace cha::web
