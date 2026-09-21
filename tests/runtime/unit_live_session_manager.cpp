#include "runtime/live_session_manager.h"

#include "workspace/builtins.h"
#include "storage/session_repository.h"
#include "support/test_live_session.h"
#include "support/test_web_graph.h"
#include "support/test_workspace.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace cha {
namespace {

using namespace std::chrono_literals;

RuntimeSettings manager_settings(std::size_t session_limit) {
    RuntimeSettings settings;
    settings.session_limit = session_limit;
    settings.command_queue_capacity = 8;
    return settings;
}

LiveSessionOpenFailure failure_of(const LiveSessionOpenResult& value) {
    return std::get<LiveSessionOpenFailure>(value);
}

// One temporary database per identity, created on first use so the same key
// reopens from the same storage across unload cycles.
class SessionFiles {
public:
    const std::filesystem::path& path_for(const FullSessionId& key) {
        std::lock_guard lock(mutex_);
        auto found = files_.find(key);
        if (found == files_.end()) {
            found = files_.emplace(
                key,
                std::make_unique<test::TemporarySessionFile>("manager", key)).first;
        }
        return found->second->path();
    }

private:
    std::mutex mutex_;
    std::map<FullSessionId, std::unique_ptr<test::TemporarySessionFile>> files_;
};

// Every mapped actor here runs a real SessionController over real storage;
// nothing about the controller, its output, or its lifecycle is faked.
SessionOpener test_opener(SessionFiles& files) {
    return [&files](const FullSessionId& identity, std::shared_ptr<WakeNotifier> notifier) {
        return test::open_test_session(
            identity, files.path_for(identity), notifier);
    };
}

bool wait_for_finished(
    const LiveSessionHandle& session,
    std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (session->lifecycle() == LiveSessionState::finished) return true;
        std::this_thread::sleep_for(1ms);
    }
    return session->lifecycle() == LiveSessionState::finished;
}

std::optional<MaintenanceFailure> reserve_and_release(
    LiveSessionManager& manager,
    const FullSessionId& key,
    bool global,
    std::chrono::milliseconds deadline) {
    if (global) {
        auto result = manager.reserve_global_maintenance(deadline);
        if (const auto* failure = std::get_if<MaintenanceFailure>(&result)) {
            return *failure;
        }
    } else {
        auto result = manager.reserve_for_deletion(key, deadline);
        if (const auto* failure = std::get_if<MaintenanceFailure>(&result)) {
            return *failure;
        }
    }
    return std::nullopt;
}

// Holds one actor's owner thread inside SessionController::shutdown() by
// keeping a generation in flight that deliberately ignores cancellation. This
// is the exact lifecycle point a wedged owner occupies; nothing else about the
// session is replaced.
class WedgedOwners {
public:
    explicit WedgedOwners(SessionFiles& files) : files_(files) {}

    SessionOpener opener() {
        return [this](const FullSessionId& identity, std::shared_ptr<WakeNotifier> notifier) {
            return test::open_scripted_session(
                identity,
                files_.path_for(identity),
                notifier,
                controls_for(identity));
        };
    }

    // Starts a generation that ignores cancellation, so this actor's owner
    // blocks inside SessionController::shutdown() until release().
    [[nodiscard]] bool wedge(LiveSession& session) {
        const std::shared_ptr<test::BackendControls> controls =
            controls_for(session.identity());
        controls->ignore_cancellation();
        if (!std::holds_alternative<CommandResult>(
                session.submit(RawCommand{"Question"}, 5s))) {
            return false;
        }
        return controls->wait_until_running();
    }

    void release() {
        std::lock_guard lock(mutex_);
        for (const auto& [key, controls] : controls_) {
            (void)key;
            controls->finish();
        }
    }

private:
    std::shared_ptr<test::BackendControls> controls_for(
        const FullSessionId& key) {
        std::lock_guard lock(mutex_);
        std::shared_ptr<test::BackendControls>& slot = controls_[key];
        if (!slot) slot = std::make_shared<test::BackendControls>();
        return slot;
    }

    SessionFiles& files_;
    std::mutex mutex_;
    std::map<FullSessionId, std::shared_ptr<test::BackendControls>> controls_;
};

TEST(LiveSessionManager, ReusesRunningSessionAndReturnsTheActor) {
    SessionFiles files;
    std::atomic<int> starts{};
    LiveSessionManager manager(
        manager_settings(2),
        [&](const FullSessionId& identity, std::shared_ptr<WakeNotifier> notifier) {
            ++starts;
            return test::open_test_session(
                identity, files.path_for(identity), notifier);
        });
    const FullSessionId key{"forum", "session"};
    EXPECT_FALSE(manager.try_reattach(key));

    const auto first = manager.open(key, 5s);
    const auto second = manager.open(key, 5s);
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(first));
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(second));
    EXPECT_EQ(starts, 1);
    EXPECT_TRUE(manager.lookup(key));
    const auto reattached = manager.try_reattach(key);
    ASSERT_TRUE(reattached);
    EXPECT_TRUE(std::holds_alternative<LiveSessionReady>(*reattached));

    manager.begin_shutdown();
    const auto stopping = manager.try_reattach(key);
    ASSERT_TRUE(stopping);
    EXPECT_EQ(failure_of(*stopping), LiveSessionOpenFailure::manager_stopping);
    EXPECT_TRUE(manager.join_shutdown(5s));
}

TEST(LiveSessionManager, OpensWelcomeAndAttributesGuestInBuiltInAndWorkspaceSessions) {
    test::TestWorkspace fixture;
    const test::WebGraph graph(fixture.root());
    LiveSessionManager manager(manager_settings(2), graph.opener());

    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(
        manager.open({std::string(entrance_id), std::string(welcome_id)}, 5s)));
    LiveSessionHandle welcome =
        manager.lookup({std::string(entrance_id), std::string(welcome_id)});
    ASSERT_TRUE(welcome);
    const CommandSubmitResult welcome_state = welcome->snapshot(5s);
    const auto* welcome_snapshot = std::get_if<SessionSnapshot>(&welcome_state);
    ASSERT_NE(welcome_snapshot, nullptr);
    EXPECT_EQ(welcome_snapshot->forum.id, entrance_id);
    EXPECT_EQ(welcome_snapshot->forum.display_name, entrance_name);
    EXPECT_EQ(welcome_snapshot->session_id, welcome_id);
    EXPECT_EQ(welcome_snapshot->session_label, welcome_name);
    ASSERT_EQ(welcome_snapshot->characters.size(), 1U);
    EXPECT_EQ(welcome_snapshot->characters.front().id, assistant_id);

    ASSERT_TRUE(std::holds_alternative<CommandResult>(
        welcome->submit(RawCommand{"Welcome"}, 5s)));
    const CommandSubmitResult attributed_welcome_state = welcome->snapshot(5s);
    const auto* attributed_welcome =
        std::get_if<SessionSnapshot>(&attributed_welcome_state);
    ASSERT_NE(attributed_welcome, nullptr);
    ASSERT_FALSE(attributed_welcome->transcript.empty());
    EXPECT_EQ(attributed_welcome->transcript.front().participant_id, guest_id);
    EXPECT_EQ(attributed_welcome->transcript.front().display_name, guest_name);

    const StoredSession stored = graph.sessions()->create("lobby", "Guest session");
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(
        manager.open(stored.identity, 5s)));
    LiveSessionHandle ordinary = manager.lookup(stored.identity);
    ASSERT_TRUE(ordinary);
    ASSERT_TRUE(std::holds_alternative<CommandResult>(
        ordinary->submit(RawCommand{"Ordinary"}, 5s)));
    const CommandSubmitResult attributed_ordinary_state = ordinary->snapshot(5s);
    const auto* attributed_ordinary =
        std::get_if<SessionSnapshot>(&attributed_ordinary_state);
    ASSERT_NE(attributed_ordinary, nullptr);
    ASSERT_FALSE(attributed_ordinary->transcript.empty());
    EXPECT_EQ(attributed_ordinary->transcript.front().participant_id, guest_id);
    EXPECT_EQ(attributed_ordinary->transcript.front().display_name, guest_name);

    manager.begin_shutdown();
}

TEST(LiveSessionManager, WelcomeReopensOnlyFromTheSameStorageWithItsForumPersona) {
    test::TestWorkspace fixture;
    const FullSessionId welcome_key = test::WebGraph::welcome();
    {
        const test::WebGraph graph(fixture.root());
        {
            LiveSessionManager manager(manager_settings(1), graph.opener());
            ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(
                manager.open(welcome_key, 5s)));
            LiveSessionHandle welcome = manager.lookup(welcome_key);
            ASSERT_TRUE(welcome);
            ASSERT_TRUE(std::holds_alternative<CommandResult>(
                welcome->submit(RawCommand{"Remember this"}, 5s)));

            std::optional<SessionSnapshot> settled;
            for (int attempt = 0; attempt < 200 && !settled; ++attempt) {
                CommandSubmitResult state = welcome->snapshot(5s);
                if (const auto* snapshot = std::get_if<SessionSnapshot>(&state);
                    snapshot != nullptr && !snapshot->generation.active) {
                    settled = *snapshot;
                    break;
                }
                std::this_thread::sleep_for(5ms);
            }
            ASSERT_TRUE(settled);
            ASSERT_EQ(settled->transcript.size(), 2U);
            EXPECT_EQ(settled->transcript.front().participant_id, guest_id);
            EXPECT_EQ(settled->transcript.front().display_name, guest_name);
            manager.begin_shutdown();
        }

        {
            LiveSessionManager manager(manager_settings(1), graph.opener());
            ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(
                manager.open(welcome_key, 5s)));
            LiveSessionHandle welcome = manager.lookup(welcome_key);
            ASSERT_TRUE(welcome);
            const CommandSubmitResult state = welcome->snapshot(5s);
            const auto* snapshot = std::get_if<SessionSnapshot>(&state);
            ASSERT_NE(snapshot, nullptr);
            EXPECT_EQ(snapshot->transcript.size(), 2U);
            manager.begin_shutdown();
        }
    }

    // A later repository owns a different temporary database and starts empty.
    const test::WebGraph other_graph(fixture.root());
    LiveSessionManager other(manager_settings(1), other_graph.opener());
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(
        other.open(welcome_key, 5s)));
    LiveSessionHandle fresh = other.lookup(welcome_key);
    ASSERT_TRUE(fresh);
    const CommandSubmitResult state = fresh->snapshot(5s);
    const auto* snapshot = std::get_if<SessionSnapshot>(&state);
    ASSERT_NE(snapshot, nullptr);
    EXPECT_TRUE(snapshot->transcript.empty());
    other.begin_shutdown();
}

TEST(LiveSessionManager, LimitHasStableError) {
    SessionFiles limit_files;
    LiveSessionManager limit(manager_settings(1), test_opener(limit_files));
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(
        limit.open({"f", "one"}, 5s)));
    EXPECT_EQ(
        failure_of(limit.open({"f", "two"}, 10ms)),
        LiveSessionOpenFailure::limit_reached);
    limit.begin_shutdown();
}

TEST(LiveSessionManager, SimultaneousDistinctOpensAreSerializedAndRespectLimit) {
    constexpr int request_count = 32;
    constexpr int session_limit = 4;
    SessionFiles files;
    std::mutex mutex;
    int active{};
    int peak{};
    int started{};
    LiveSessionManager manager(
        manager_settings(session_limit),
        [&](const FullSessionId& identity, std::shared_ptr<WakeNotifier> notifier) {
            {
                std::unique_lock lock(mutex);
                ++active;
                ++started;
                peak = std::max(peak, active);
            }
            std::this_thread::sleep_for(1ms);
            {
                std::lock_guard lock(mutex);
                --active;
            }
            return test::open_test_session(
                identity, files.path_for(identity), notifier);
        });

    std::promise<void> start_all;
    const std::shared_future<void> start = start_all.get_future().share();
    std::vector<std::future<LiveSessionOpenResult>> opens;
    opens.reserve(request_count);
    for (int index = 0; index < request_count; ++index) {
        opens.push_back(std::async(std::launch::async, [&, index] {
            start.wait();
            return manager.open({"f", "session-" + std::to_string(index)}, 10s);
        }));
    }
    start_all.set_value();

    int successes{};
    int rejected{};
    for (auto& open : opens) {
        LiveSessionOpenResult result = open.get();
        if (std::holds_alternative<LiveSessionReady>(result)) {
            ++successes;
        } else {
            const auto failure = failure_of(result);
            EXPECT_TRUE(failure == LiveSessionOpenFailure::limit_reached
                || failure == LiveSessionOpenFailure::open_timeout);
            ++rejected;
        }
    }
    EXPECT_EQ(successes, session_limit);
    EXPECT_EQ(rejected, request_count - session_limit);
    EXPECT_EQ(started, session_limit);
    EXPECT_EQ(peak, 1);
    manager.begin_shutdown();
}

TEST(LiveSessionManager, ConcurrentSameKeyOpensShareOneOwnerAndOutcome) {
    SessionFiles files;
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{};
    bool release{};
    std::atomic<int> starts{};
    LiveSessionManager manager(
        manager_settings(2),
        [&](const FullSessionId& identity, std::shared_ptr<WakeNotifier> notifier) {
            ++starts;
            {
                std::unique_lock lock(mutex);
                entered = true;
                changed.notify_all();
                changed.wait(lock, [&] { return release; });
            }
            return test::open_test_session(
                identity, files.path_for(identity), notifier);
        });
    const FullSessionId key{"f", "same"};
    auto first = std::async(std::launch::async, [&] { return manager.open(key, 10s); });
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, 2s, [&] { return entered; }));
    }
    auto second = std::async(std::launch::async, [&] { return manager.open(key, 10s); });
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();
    EXPECT_TRUE(std::holds_alternative<LiveSessionReady>(first.get()));
    EXPECT_TRUE(std::holds_alternative<LiveSessionReady>(second.get()));
    EXPECT_EQ(starts, 1);
    manager.begin_shutdown();
}

TEST(LiveSessionManager, ConcurrentDifferentKeyOpensAreSerialized) {
    SessionFiles files;
    std::mutex mutex;
    std::condition_variable changed;
    int entered{};
    bool release_first{};
    LiveSessionManager manager(
        manager_settings(2),
        [&](const FullSessionId& identity, std::shared_ptr<WakeNotifier> notifier) {
            {
                std::unique_lock lock(mutex);
                ++entered;
                changed.notify_all();
                if (identity.session_id == "one") {
                    changed.wait(lock, [&] { return release_first; });
                }
            }
            return test::open_test_session(
                identity, files.path_for(identity), notifier);
    });
    auto first = std::async(std::launch::async, [&] { return manager.open({"f", "one"}, 10s); });
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, 2s, [&] { return entered == 1; }));
    }
    auto second = std::async(std::launch::async, [&] { return manager.open({"f", "two"}, 10s); });
    EXPECT_EQ(second.wait_for(20ms), std::future_status::timeout);
    {
        std::lock_guard lock(mutex);
        EXPECT_EQ(entered, 1);
        release_first = true;
    }
    changed.notify_all();
    EXPECT_TRUE(std::holds_alternative<LiveSessionReady>(first.get()));
    EXPECT_TRUE(std::holds_alternative<LiveSessionReady>(second.get()));
    EXPECT_EQ(entered, 2);
    manager.begin_shutdown();
}

TEST(LiveSessionManager, FailedOpenIsSweptAndCanBeRetried) {
    SessionFiles files;
    std::atomic<int> attempts{};
    LiveSessionManager manager(
        manager_settings(1),
        [&](const FullSessionId& identity, std::shared_ptr<WakeNotifier> notifier) {
            if (++attempts == 1) throw std::runtime_error("open failed");
            return test::open_test_session(
                identity, files.path_for(identity), notifier);
        });
    const FullSessionId key{"f", "retry"};
    EXPECT_EQ(
        failure_of(manager.open(key, 2s)), LiveSessionOpenFailure::internal_error);
    EXPECT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open(key, 5s)));
    EXPECT_EQ(attempts, 2);
    manager.begin_shutdown();
}

TEST(LiveSessionManager, TimeoutDoesNotCancelTheOpen) {
    SessionFiles files;
    std::mutex mutex;
    std::condition_variable changed;
    bool release{};
    std::atomic<int> starts{};
    LiveSessionManager manager(
        manager_settings(1),
        [&](const FullSessionId& identity, std::shared_ptr<WakeNotifier> notifier) {
            ++starts;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] { return release; });
            }
            return test::open_test_session(
                identity, files.path_for(identity), notifier);
        });
    const FullSessionId key{"f", "slow"};
    EXPECT_EQ(
        failure_of(manager.open(key, 10ms)), LiveSessionOpenFailure::open_timeout);
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();
    EXPECT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open(key, 5s)));
    EXPECT_EQ(starts, 1);
    manager.begin_shutdown();
}

TEST(LiveSessionManager, WaitersHaveIndependentDeadlines) {
    SessionFiles files;
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{};
    bool release{};
    LiveSessionManager manager(
        manager_settings(1),
        [&](const FullSessionId& identity, std::shared_ptr<WakeNotifier> notifier) {
            {
                std::unique_lock lock(mutex);
                entered = true;
                changed.notify_all();
                changed.wait(lock, [&] { return release; });
            }
            return test::open_test_session(
                identity, files.path_for(identity), notifier);
        });
    const FullSessionId key{"f", "deadlines"};
    auto short_waiter = std::async(std::launch::async, [&] { return manager.open(key, 10ms); });
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, 2s, [&] { return entered; }));
    }
    auto long_waiter = std::async(std::launch::async, [&] { return manager.open(key, 10s); });
    ASSERT_EQ(short_waiter.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(failure_of(short_waiter.get()), LiveSessionOpenFailure::open_timeout);
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();
    EXPECT_TRUE(std::holds_alternative<LiveSessionReady>(long_waiter.get()));
    manager.begin_shutdown();
}

TEST(LiveSessionManager, StoppingActorRejectsOpenConsumesCapacityAndLateHandleStops) {
    SessionFiles files;
    WedgedOwners wedged(files);
    LiveSessionManager manager(manager_settings(1), wedged.opener());
    const FullSessionId key{"f", "stopping"};
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open(key, 5s)));
    LiveSessionHandle session = manager.lookup(key);
    ASSERT_TRUE(session);
    ASSERT_TRUE(wedged.wedge(*session));

    session->request_shutdown();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (session->lifecycle() != LiveSessionState::stopping
        && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_EQ(session->lifecycle(), LiveSessionState::stopping);

    EXPECT_EQ(failure_of(manager.open(key, 10ms)), LiveSessionOpenFailure::stopping);
    EXPECT_EQ(
        failure_of(manager.open({"f", "other"}, 10ms)),
        LiveSessionOpenFailure::open_timeout);

    wedged.release();
    ASSERT_TRUE(wait_for_finished(session));
    manager.sweep();
    EXPECT_FALSE(manager.lookup(key));
    // The map no longer owns the actor, but this in-flight request handle keeps
    // it alive and sees the already-stopped session.
    EXPECT_EQ(
        std::get<ErrorCode>(session->submit(RawCommand{"ignored"}, 10ms)),
        ErrorCode::session_not_live);
}

TEST(LiveSessionManager, ShutdownExposesUnfinishedOwnersWithoutCompletingStartup) {
    SessionFiles files;
    std::mutex mutex;
    std::condition_variable changed;
    bool entered{};
    bool release{};
    LiveSessionManager manager(
        manager_settings(1),
        [&](const FullSessionId& identity, std::shared_ptr<WakeNotifier> notifier) {
            {
                std::unique_lock lock(mutex);
                entered = true;
                changed.notify_all();
                changed.wait(lock, [&] { return release; });
            }
            return test::open_test_session(
                identity, files.path_for(identity), notifier);
        });
    const FullSessionId key{"f", "unfinished"};
    auto waiter = std::async(std::launch::async, [&] { return manager.open(key, 30s); });
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(changed.wait_for(lock, 2s, [&] { return entered; }));
    }
    auto snapshot = std::async(std::launch::async, [&] { return manager.snapshot(); });
    ASSERT_EQ(snapshot.wait_for(20ms), std::future_status::timeout);
    manager.begin_shutdown();
    EXPECT_EQ(manager.unfinished_owners(), std::vector<FullSessionId>{key});
    ASSERT_EQ(waiter.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(failure_of(waiter.get()), LiveSessionOpenFailure::manager_stopping);
    ASSERT_EQ(snapshot.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(snapshot.get().live_session_count, 0U);
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();
    EXPECT_TRUE(manager.join_shutdown(10s));
}

TEST(LiveSessionManager, ShutdownAtCommitNeverPublishesAndTearsDownTheNewController) {
    test::TemporarySessionFile file("manager_commit_race");
    std::atomic<int> opens{};
    LiveSessionManager* manager_pointer = nullptr;
    LiveSessionManager manager(
        manager_settings(1),
        [&](const FullSessionId& identity, std::shared_ptr<WakeNotifier> notifier) {
            ++opens;
            // Shutdown wins the commit race while this open is still running.
            manager_pointer->begin_shutdown();
            return test::open_test_session(identity, file.path(), notifier);
        });
    manager_pointer = &manager;
    const FullSessionId key{"f", "commit-race"};

    EXPECT_EQ(
        failure_of(manager.open(key, 5s)),
        LiveSessionOpenFailure::manager_stopping);
    EXPECT_TRUE(manager.join_shutdown(10s));
    EXPECT_EQ(opens, 1);
    EXPECT_FALSE(manager.lookup(key));
    // The controller that opening produced was still shut down.
}

TEST(LiveSessionManager, ShutdownGraceIsNotMultipliedByOwnerCount) {
    SessionFiles files;
    WedgedOwners wedged(files);
    LiveSessionManager manager(manager_settings(4), wedged.opener());
    std::vector<LiveSessionHandle> sessions;
    std::vector<FullSessionId> expected_unfinished;
    for (int index = 0; index != 4; ++index) {
        const FullSessionId key{"f", "blocked-" + std::to_string(index)};
        ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open(key, 5s)));
        LiveSessionHandle session = manager.lookup(key);
        ASSERT_TRUE(session);
        sessions.push_back(session);
        expected_unfinished.push_back(key);
    }
    // All four share one wedged backend control, so all four owners block.
    for (const LiveSessionHandle& session : sessions) {
        ASSERT_TRUE(wedged.wedge(*session));
    }

    manager.begin_shutdown();
    const auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(manager.join_shutdown(200ms));
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_GE(elapsed, 200ms);
    EXPECT_LT(elapsed, 800ms);
    const auto unfinished = manager.unfinished_owners();
    ASSERT_EQ(unfinished.size(), expected_unfinished.size());
    for (const FullSessionId& key : expected_unfinished) {
        EXPECT_NE(std::find(unfinished.begin(), unfinished.end(), key), unfinished.end());
    }

    wedged.release();
    EXPECT_TRUE(manager.join_shutdown(10s));
    EXPECT_TRUE(manager.unfinished_owners().empty());
}

TEST(LiveSessionManager, ReopensSameKeyAfterTheOldOwnerHasBeenJoined) {
    SessionFiles files;
    std::atomic<int> starts{};
    LiveSessionManager manager(
        manager_settings(2),
        [&](const FullSessionId& identity, std::shared_ptr<WakeNotifier> notifier) {
            ++starts;
            return test::open_test_session(
                identity, files.path_for(identity), notifier);
        });
    const FullSessionId key{"f", "reopen"};
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open(key, 5s)));
    LiveSessionHandle old_handle = manager.lookup(key);
    ASSERT_TRUE(old_handle);
    old_handle->request_shutdown();
    ASSERT_TRUE(wait_for_finished(old_handle));

    // The old actor released its controller before publishing finished, so the
    // same identity opens again while the old handle is retained by this test.
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open(key, 5s)));
    LiveSessionHandle fresh = manager.lookup(key);
    ASSERT_TRUE(fresh);
    EXPECT_NE(fresh.get(), old_handle.get());
    EXPECT_EQ(starts, 2);
    EXPECT_EQ(
        std::get<ErrorCode>(old_handle->submit(StopCommand{}, 10ms)),
        ErrorCode::session_not_live);
    EXPECT_TRUE(std::holds_alternative<SessionSnapshot>(fresh->snapshot(5s)));
    manager.begin_shutdown();
}

TEST(LiveSessionManager, DeletionReservationStopsTheActorAndBlocksOpenAndReattach) {
    SessionFiles files;
    RuntimeSettings settings = manager_settings(2);
    settings.delete_deadline = 1s;
    LiveSessionManager manager(settings, test_opener(files));
    const FullSessionId key{"forum", "reserved"};
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open(key, 5s)));

    {
        MaintenanceReservationResult reservation =
            manager.reserve_for_deletion(key, 1s);
        ASSERT_TRUE(std::holds_alternative<LiveSessionMaintenanceReservation>(reservation));
        EXPECT_EQ(failure_of(manager.open(key, 10ms)), LiveSessionOpenFailure::stopping);
        const auto reattach = manager.try_reattach(key);
        ASSERT_TRUE(reattach);
        EXPECT_EQ(failure_of(*reattach), LiveSessionOpenFailure::stopping);
        EXPECT_FALSE(manager.lookup(key));
    }

    EXPECT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open(key, 5s)));
    manager.begin_shutdown();
}

TEST(LiveSessionManager, RepeatedOpenUnloadCyclesReapOwnersAndReleaseCapacity) {
    constexpr int cycles = 20;
    SessionFiles files;
    std::atomic<int> starts{};
    LiveSessionManager manager(
        manager_settings(1),
        [&](const FullSessionId& identity, std::shared_ptr<WakeNotifier> notifier) {
            ++starts;
            return test::open_test_session(
                identity, files.path_for(identity), notifier);
        });
    const FullSessionId key{"forum", "cycled"};

    for (int cycle = 0; cycle != cycles; ++cycle) {
        ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open(key, 5s)))
            << "cycle " << cycle;
        LiveSessionHandle session = manager.lookup(key);
        ASSERT_TRUE(session);
        session->request_shutdown();
        session = {};

        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (manager.snapshot().live_session_count != 0
            && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        EXPECT_EQ(manager.snapshot().live_session_count, 0U);
    }

    EXPECT_EQ(starts, cycles);
}

TEST(LiveSessionManager, GlobalMaintenanceReleasesActorsAndResumesAdmission) {
    SessionFiles files;
    LiveSessionManager manager(
        manager_settings(2),
        [&](const FullSessionId& identity, std::shared_ptr<WakeNotifier> notifier) {
            return test::open_test_session(
                identity, files.path_for(identity), notifier);
        });
    const FullSessionId key{"forum", "maintenance"};
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open(key, 5s)));

    {
        GlobalMaintenanceResult reserved =
            manager.reserve_global_maintenance(5s);
        ASSERT_TRUE(std::holds_alternative<LiveSessionGlobalMaintenance>(reserved));
        EXPECT_FALSE(manager.lookup(key));
        EXPECT_EQ(
            failure_of(manager.open(key, 10ms)),
            LiveSessionOpenFailure::manager_stopping);
    }

    EXPECT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open(key, 5s)));
    manager.begin_shutdown();
}

TEST(LiveSessionManager, MaintenanceDeadlinesIncludeAdmissionAndQueueWait) {
    for (const bool global : {false, true}) {
        for (const bool full_queue : {false, true}) {
            SCOPED_TRACE(global ? "global" : "deletion");
            SCOPED_TRACE(full_queue ? "full queue" : "queued admission");
            SessionFiles files;
            std::promise<void> entered;
            std::promise<void> release;
            auto entered_future = entered.get_future();
            auto released = release.get_future().share();
            auto settings = manager_settings(2);
            settings.command_queue_capacity = 1;
            const FullSessionId selected{"forum", "selected"};
            const FullSessionId blocked{"forum", "blocked"};
            LiveSessionManager manager(settings, [&](
                                           const FullSessionId& identity,
                                           std::shared_ptr<WakeNotifier> notifier) {
                if (identity == blocked) {
                    entered.set_value();
                    released.wait();
                }
                return test::open_test_session(identity, files.path_for(identity), notifier);
            });
            ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.select(selected, 2s)));
            const auto session = manager.lookup(selected);
            ASSERT_TRUE(session);
            auto opening = std::async(std::launch::async, [&] {
                return manager.open(blocked, 2s);
            });
            const auto entered_status = entered_future.wait_for(2s);
            if (entered_status != std::future_status::ready) release.set_value();
            ASSERT_EQ(entered_status, std::future_status::ready);
            if (full_queue) {
                EXPECT_TRUE(std::holds_alternative<std::shared_ptr<CommandReply>>(
                    session->enqueue(SnapshotCommand{})));
            }
            auto reserving = std::async(std::launch::async, [&] {
                return reserve_and_release(manager, selected, global, 20ms);
            });
            const auto status = reserving.wait_for(500ms);
            release.set_value();
            EXPECT_EQ(status, std::future_status::ready);
            EXPECT_EQ(reserving.get(), MaintenanceFailure::stopping);
            ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(opening.get()));

            // The timed-out command must not stop either session later or
            // leave a reservation that prevents another maintenance attempt.
            EXPECT_EQ(manager.lookup(selected), session);
            EXPECT_EQ(manager.selected(), selected);
            EXPECT_TRUE(manager.lookup(blocked));
            EXPECT_FALSE(reserve_and_release(manager, selected, global, 2s));
            EXPECT_TRUE(std::holds_alternative<LiveSessionReady>(manager.select(selected, 2s)));
        }
    }
}

TEST(LiveSessionManager, MaintenanceTimeoutCleanupDoesNotWaitForQueueCapacity) {
    for (const bool global : {false, true}) {
        SCOPED_TRACE(global ? "global" : "deletion");
        SessionFiles files;
        std::promise<void> entered;
        std::promise<void> release;
        auto entered_future = entered.get_future();
        auto released = release.get_future().share();
        std::atomic<bool> block_snapshot{};
        auto settings = manager_settings(2);
        settings.command_queue_capacity = 1;
        LiveSessionManager manager(settings, [&](
                                       const FullSessionId& identity,
                                       std::shared_ptr<WakeNotifier> notifier) {
            auto opened = test::open_test_session(identity, files.path_for(identity), notifier);
            opened.cached_audio_entries = [&] {
                if (block_snapshot.exchange(false)) {
                    entered.set_value();
                    released.wait();
                }
                return std::set<EntryId>{};
            };
            return opened;
        });
        const FullSessionId key{"forum", "draining"};
        ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.select(key, 2s)));
        const auto session = manager.lookup(key);
        ASSERT_TRUE(session);
        ASSERT_TRUE(std::holds_alternative<SubscribeResult>(
            session->subscribe({"view", 1, "sub"}, 2s)));
        block_snapshot.store(true);
        auto reserving = std::async(std::launch::async, [&] {
            return reserve_and_release(manager, key, global, 100ms);
        });
        const auto entered_status = entered_future.wait_for(2s);
        if (entered_status == std::future_status::ready) {
            // This accepted open fills the only queue slot while terminal
            // snapshot construction holds the runtime inside retirement.
            EXPECT_EQ(failure_of(manager.open({"forum", "queued"}, 0ms)),
                LiveSessionOpenFailure::open_timeout);
        }
        const auto status = reserving.wait_for(500ms);
        release.set_value();
        EXPECT_EQ(entered_status, std::future_status::ready);
        EXPECT_EQ(status, std::future_status::ready);
        EXPECT_EQ(reserving.get(), MaintenanceFailure::stopping);
        EXPECT_TRUE(std::holds_alternative<LiveSessionReady>(manager.select(key, 2s)));
        EXPECT_FALSE(reserve_and_release(manager, key, global, 2s));
    }
}

TEST(LiveSessionManager, ContextPublicationDoesNotWaitAndRejectsOldQueuedWork) {
    SessionFiles files;
    std::promise<void> entered;
    std::promise<void> release;
    auto entered_future = entered.get_future();
    auto released = release.get_future().share();
    const FullSessionId selected{"forum", "selected"};
    const FullSessionId blocked{"forum", "blocked"};
    LiveSessionManager manager(manager_settings(3), [&](
                                   const FullSessionId& identity,
                                   std::shared_ptr<WakeNotifier> notifier) {
        if (identity == blocked) {
            entered.set_value();
            released.wait();
        }
        return test::open_test_session(identity, files.path_for(identity), notifier);
    });
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.select(selected, 2s)));
    const auto session = manager.lookup(selected);
    ASSERT_TRUE(session);
    const auto epoch = manager.context_epoch();
    auto opening = std::async(std::launch::async, [&] {
        return manager.select(blocked, 2s);
    });
    const auto entered_status = entered_future.wait_for(2s);
    if (entered_status != std::future_status::ready) release.set_value();
    ASSERT_EQ(entered_status, std::future_status::ready);
    const auto queued = session->enqueue(RenameSessionCommand{"Old context"});
    auto selecting = std::async(std::launch::async, [&] {
        return manager.select({"forum", "queued"}, 2s);
    });
    EXPECT_EQ(selecting.wait_for(20ms), std::future_status::timeout);
    auto publishing = std::async(std::launch::async, [&] {
        return manager.bump_context_epoch();
    });
    const auto status = publishing.wait_for(500ms);
    release.set_value();
    EXPECT_EQ(status, std::future_status::ready);
    EXPECT_EQ(publishing.get(), epoch + 1);
    EXPECT_EQ(failure_of(opening.get()), LiveSessionOpenFailure::manager_stopping);
    EXPECT_EQ(failure_of(selecting.get()), LiveSessionOpenFailure::manager_stopping);
    ASSERT_TRUE(std::holds_alternative<std::shared_ptr<CommandReply>>(queued));
    const auto reply = std::get<std::shared_ptr<CommandReply>>(queued)->wait_for(2s);
    ASSERT_TRUE(reply);
    ASSERT_TRUE(std::holds_alternative<ErrorCode>(*reply));
    EXPECT_EQ(std::get<ErrorCode>(*reply), ErrorCode::vault_changed);
    EXPECT_EQ(manager.selected(), selected);
    EXPECT_FALSE(manager.lookup(selected, epoch));
    EXPECT_EQ(manager.lookup(selected, epoch + 1), session);
    EXPECT_EQ(read_session_database_metadata(files.path_for(selected)).label, "Test session");
    EXPECT_TRUE(std::holds_alternative<SessionLabelResult>(
        session->submit(RenameSessionCommand{"New context"}, 2s)));
}

} // namespace
} // namespace cha
