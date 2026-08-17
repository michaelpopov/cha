#include "web/live_session_manager.h"
#include "web/sse_mailbox.h"

#include "session/session_lease.h"
#include "session/session_repository.h"
#include "support/test_live_session.h"
#include "support/test_web_graph.h"
#include "support/test_workspace.h"
#include "util/path_name.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace cha::web {
namespace {

using namespace std::chrono_literals;

// One temporary database per identity. Every session in this file runs a real
// SessionController over its own storage and its own cross-process lease.
class SessionFiles {
public:
    const std::filesystem::path& path_for(const SessionIdentity& key) {
        std::lock_guard lock(mutex_);
        auto found = files_.find(key);
        if (found == files_.end()) {
            found = files_.emplace(
                key,
                std::make_unique<test::TemporarySessionFile>("stress", key)).first;
        }
        return found->second->path();
    }

private:
    std::mutex mutex_;
    std::map<SessionIdentity, std::unique_ptr<test::TemporarySessionFile>> files_;
};

class CommandGate {
public:
    void wait() {
        std::unique_lock lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return released_; });
    }

    bool wait_until_entered(std::chrono::milliseconds timeout = 5s) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [this] { return entered_; });
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

void execute_sql(const std::filesystem::path& path, const char* statement) {
    sqlite3* raw_database = nullptr;
    const int open_result = sqlite3_open_v2(
        utf8_path(path).c_str(), &raw_database, SQLITE_OPEN_READWRITE, nullptr);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> database(
        raw_database, &sqlite3_close_v2);
    if (open_result != SQLITE_OK) {
        throw std::runtime_error("Failed to open the stress fixture database");
    }
    char* raw_error = nullptr;
    const int execute_result =
        sqlite3_exec(database.get(), statement, nullptr, nullptr, &raw_error);
    sqlite3_free(raw_error);
    if (execute_result != SQLITE_OK) {
        throw std::runtime_error("Failed to inject a persistence failure");
    }
}

bool wait_for_live_count(
    LiveSessionManager& manager,
    std::size_t expected,
    std::chrono::milliseconds timeout = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (manager.snapshot().live_session_count == expected) return true;
        std::this_thread::sleep_for(1ms);
    }
    return manager.snapshot().live_session_count == expected;
}

TEST(WebSessionStress, ConcurrentSessionsKeepCommandsOnIndependentQueues) {
    SessionFiles files;
    WebSettings settings;
    settings.session_limit = 2;
    settings.command_queue_capacity = 64;
    LiveSessionManager manager(
        settings,
        [&files](const SessionIdentity& key, WakeNotifier& notifier) {
            return test::open_leased_session(key, files.path_for(key), notifier);
        });

    auto open = [&manager](std::string id) {
        return manager.open({"forum", std::move(id)}, 10s);
    };
    auto first_open = std::async(std::launch::async, open, "first");
    auto second_open = std::async(std::launch::async, open, "second");
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(first_open.get()));
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(second_open.get()));

    LiveSessionHandle first = manager.lookup({"forum", "first"});
    LiveSessionHandle second = manager.lookup({"forum", "second"});
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);

    constexpr unsigned int commands_per_session = 40;
    std::vector<std::future<CommandSubmitResult>> results;
    results.reserve(commands_per_session * 2);
    for (unsigned int index = 0; index != commands_per_session; ++index) {
        results.push_back(std::async(std::launch::async, [&first] {
            return first->submit(RawCommand{"/info"}, 10s);
        }));
        results.push_back(std::async(std::launch::async, [&second] {
            return second->submit(RawCommand{"/info"}, 10s);
        }));
    }
    for (auto& result : results) {
        EXPECT_TRUE(std::holds_alternative<CommandResult>(result.get()));
    }

    // Each actor owns its own controller and storage, so a prompt submitted to
    // one can never appear in the other's transcript.
    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        first->submit(RawCommand{"first-only"}, 10s)));
    const auto first_state = first->snapshot(10s);
    const auto second_state = second->snapshot(10s);
    ASSERT_TRUE(std::holds_alternative<SessionSnapshot>(first_state));
    ASSERT_TRUE(std::holds_alternative<SessionSnapshot>(second_state));
    const SessionSnapshot& first_snapshot = std::get<SessionSnapshot>(first_state);
    ASSERT_FALSE(first_snapshot.transcript.empty());
    EXPECT_EQ(first_snapshot.transcript.front().text, "first-only");
    EXPECT_TRUE(std::get<SessionSnapshot>(second_state).transcript.empty());

    manager.begin_shutdown();
    EXPECT_TRUE(manager.join_shutdown(10s));
}

TEST(WebSessionStress, BlockedOwnerDoesNotDelayAnotherSession) {
    SessionFiles files;
    CommandGate gate;
    auto blocked_controls = std::make_shared<test::BackendControls>();
    WebSettings settings;
    settings.session_limit = 2;
    settings.command_queue_capacity = 8;
    LiveSessionManager manager(
        settings,
        [&](const SessionIdentity& key, WakeNotifier& notifier) {
            if (key.session_id == "blocked") {
                return test::open_scripted_session(
                    key,
                    files.path_for(key),
                    notifier,
                    test::one_backend(test::scripted_backend(blocked_controls)),
                    [&gate](std::size_t) { gate.wait(); });
            }
            return test::open_leased_session(key, files.path_for(key), notifier);
        });
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(
        manager.open({"forum", "blocked"}, 10s)));
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(
        manager.open({"forum", "healthy"}, 10s)));
    LiveSessionHandle blocked = manager.lookup({"forum", "blocked"});
    LiveSessionHandle healthy = manager.lookup({"forum", "healthy"});
    ASSERT_TRUE(blocked);
    ASSERT_TRUE(healthy);

    auto blocked_command = std::async(std::launch::async, [&] {
        return blocked->submit(RawCommand{"wait"}, 50ms);
    });
    ASSERT_TRUE(gate.wait_until_entered());
    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        healthy->submit(RawCommand{"/info"}, 10s)));
    EXPECT_EQ(
        std::get<ErrorCode>(blocked_command.get()), ErrorCode::command_timeout);

    gate.release();
    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        healthy->submit(RawCommand{"/info"}, 10s)));
    ASSERT_TRUE(blocked_controls->wait_until_running());
    blocked_controls->finish();
    manager.begin_shutdown();
    EXPECT_TRUE(manager.join_shutdown(10s));
}

TEST(WebSessionStress, RepeatedOpenUnloadReopenAndSweepRacesPreserveLimit) {
    constexpr int rounds = 8;
    constexpr int limit = 4;
    SessionFiles files;
    std::atomic<int> starts{};
    WebSettings settings;
    settings.session_limit = limit;
    LiveSessionManager manager(
        settings,
        [&](const SessionIdentity& key, WakeNotifier& notifier) {
            ++starts;
            return test::open_leased_session(key, files.path_for(key), notifier);
        });

    for (int round = 0; round != rounds; ++round) {
        std::vector<SessionIdentity> keys;
        for (int index = 0; index != limit; ++index) {
            keys.push_back({
                "forum",
                "round-" + std::to_string(round) + "-" + std::to_string(index),
            });
        }
        std::promise<void> start_signal;
        const std::shared_future<void> start = start_signal.get_future().share();
        std::vector<std::future<LiveSessionOpenResult>> opens;
        for (const SessionIdentity& key : keys) {
            for (int duplicate = 0; duplicate != 2; ++duplicate) {
                opens.push_back(std::async(
                    std::launch::async, [&, key, start] {
                        start.wait();
                        return manager.open(key, 10s);
                    }));
            }
        }
        start_signal.set_value();
        for (auto& open : opens) {
            ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(open.get()));
        }
        const LiveSessionOpenResult overflow = manager.open(
            {"forum", "overflow-" + std::to_string(round)}, 10ms);
        ASSERT_TRUE(std::holds_alternative<LiveSessionOpenFailure>(overflow));
        EXPECT_EQ(
            std::get<LiveSessionOpenFailure>(overflow),
            LiveSessionOpenFailure::limit_reached);

        std::vector<LiveSessionHandle> old_handles;
        for (const SessionIdentity& key : keys) {
            old_handles.push_back(manager.lookup(key));
            ASSERT_TRUE(old_handles.back());
            old_handles.back()->request_shutdown();
        }
        old_handles.clear();

        std::atomic<bool> sweeping{true};
        auto sweeper = std::async(std::launch::async, [&] {
            while (sweeping) {
                manager.sweep();
                std::this_thread::yield();
            }
        });
        std::vector<std::future<LiveSessionHandle>> reopened;
        for (const SessionIdentity& key : keys) {
            reopened.push_back(std::async(std::launch::async, [&, key] {
                const auto deadline = std::chrono::steady_clock::now() + 20s;
                while (std::chrono::steady_clock::now() < deadline) {
                    const LiveSessionOpenResult result = manager.open(key, 500ms);
                    if (std::holds_alternative<LiveSessionReady>(result)) {
                        LiveSessionHandle session = manager.lookup(key);
                        if (session && std::holds_alternative<SessionSnapshot>(
                                session->snapshot(500ms))) {
                            return session;
                        }
                    }
                    std::this_thread::yield();
                }
                return LiveSessionHandle{};
            }));
        }
        std::vector<LiveSessionHandle> live_handles;
        bool all_reopened = true;
        for (auto& reopen : reopened) {
            live_handles.push_back(reopen.get());
            all_reopened = all_reopened && static_cast<bool>(live_handles.back());
        }
        sweeping = false;
        sweeper.get();
        ASSERT_TRUE(all_reopened);
        EXPECT_EQ(manager.snapshot().live_session_count, limit);

        for (LiveSessionHandle& session : live_handles) {
            session->request_shutdown();
        }
        live_handles.clear();
        ASSERT_TRUE(wait_for_live_count(manager, 0));
    }
    EXPECT_EQ(starts, rounds * limit * 2);
}

TEST(WebSessionStress, FatalSessionDoesNotInterruptItsPeer) {
    SessionFiles files;
    WebSettings settings;
    settings.session_limit = 2;
    settings.command_queue_capacity = 64;
    LiveSessionManager manager(
        settings,
        [&files](const SessionIdentity& key, WakeNotifier& notifier) {
            return test::open_leased_session(key, files.path_for(key), notifier);
        });
    const SessionIdentity failing{"forum", "failing"};
    const SessionIdentity healthy{"forum", "healthy"};
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open(failing, 10s)));
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open(healthy, 10s)));
    LiveSessionHandle failing_session = manager.lookup(failing);
    LiveSessionHandle healthy_session = manager.lookup(healthy);
    ASSERT_TRUE(failing_session);
    ASSERT_TRUE(healthy_session);

    // A corrupted journal makes the next real controller write fail inside the
    // failing actor's owner loop.
    execute_sql(files.path_for(failing), "DROP TABLE turns");
    (void)failing_session->submit(RawCommand{"Question"}, 500ms);

    std::vector<std::future<CommandSubmitResult>> commands;
    for (int index = 0; index != 40; ++index) {
        commands.push_back(std::async(std::launch::async, [&] {
            return healthy_session->submit(RawCommand{"/info"}, 10s);
        }));
    }
    for (auto& command : commands) {
        EXPECT_TRUE(std::holds_alternative<CommandResult>(command.get()));
    }

    failing_session = {};
    ASSERT_TRUE(wait_for_live_count(manager, 1));
    EXPECT_TRUE(manager.lookup(healthy));
    // The failed actor released its lease and its capacity; only its own
    // storage is now unusable.
    SessionLease reopened = SessionLease::acquire(files.path_for(failing));
    EXPECT_TRUE(reopened.active());
    reopened = SessionLease::inactive_for_testing();
    EXPECT_TRUE(std::holds_alternative<LiveSessionReady>(
        manager.open({"forum", "replacement"}, 10s)));

    manager.begin_shutdown();
    EXPECT_TRUE(manager.join_shutdown(10s));
}

TEST(WebSessionStress, ConcurrentWorkspaceLifecycleKeepsMailboxesAndLeasesIndependent) {
    test::TestWorkspace fixture;
    const test::WebGraph graph(fixture.root());
    constexpr std::size_t session_count = 10;
    std::atomic<bool> creating{true};
    std::atomic<std::size_t> list_calls{};
    auto listing = std::async(std::launch::async, [&] {
        do {
            (void)graph.sessions()->list("lobby");
            ++list_calls;
        } while (creating);
    });

    std::vector<std::future<StoredSession>> creations;
    creations.reserve(session_count);
    for (std::size_t index = 0; index != session_count; ++index) {
        creations.push_back(std::async(std::launch::async, [&, index] {
            return graph.sessions()->create(
                "lobby", "Load " + std::to_string(index));
        }));
    }
    std::vector<StoredSession> sessions;
    sessions.reserve(session_count);
    for (auto& creation : creations) sessions.push_back(creation.get());
    creating = false;
    listing.get();
    EXPECT_GT(list_calls, 0U);
    EXPECT_EQ(graph.sessions()->list("lobby").size(), session_count);

    WebSettings settings;
    settings.session_limit = session_count;
    settings.command_queue_capacity = 16;
    LiveSessionManager manager(settings, graph.opener());
    std::vector<std::future<LiveSessionOpenResult>> opens;
    opens.reserve(session_count);
    for (const StoredSession& session : sessions) {
        opens.push_back(std::async(std::launch::async, [&, key = session.identity] {
            return manager.open(key, 10s);
        }));
    }
    for (auto& open : opens) {
        ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(open.get()));
    }
    EXPECT_EQ(manager.snapshot().live_session_count, session_count);

    std::vector<LiveSessionHandle> handles;
    std::vector<SseConnectResult> streams;
    std::set<const SseMailbox*> mailboxes;
    handles.reserve(session_count);
    streams.reserve(session_count);
    for (const StoredSession& session : sessions) {
        handles.push_back(manager.lookup(session.identity));
        ASSERT_TRUE(handles.back());
        CommandSubmitResult connected = handles.back()->connect_sse(10s);
        ASSERT_TRUE(std::holds_alternative<SseConnectResult>(connected));
        streams.push_back(std::get<SseConnectResult>(std::move(connected)));
        mailboxes.insert(streams.back().mailbox.get());
        const SseMailbox::Next initial = streams.back().mailbox->next(
            streams.back().stream, 5s);
        ASSERT_TRUE(initial.payload);
        streams.back().mailbox->written(streams.back().stream);
    }
    EXPECT_EQ(mailboxes.size(), session_count);

    for (const StoredSession& session : sessions) {
        const std::filesystem::path database =
            fixture.root() / "forums" / "lobby" / "sessions"
            / (session.identity.session_id + ".sqlite3");
        EXPECT_THROW((void)SessionLease::acquire(database), SessionBusyError);
    }

    std::vector<std::future<CommandSubmitResult>> commands;
    commands.reserve(session_count);
    for (LiveSessionHandle& session : handles) {
        LiveSession* const target = session.get();
        commands.push_back(std::async(std::launch::async, [target] {
            return target->submit(RawCommand{"/clear"}, 10s);
        }));
    }
    for (auto& command : commands) {
        EXPECT_TRUE(std::holds_alternative<CommandResult>(command.get()));
    }

    for (std::size_t index = 0; index != handles.size(); ++index) {
        const std::size_t collapsed = streams[index].mailbox->end_stream(
            streams[index].stream);
        handles[index]->disconnect_sse(streams[index].connection_id, collapsed);
    }
    std::vector<std::future<void>> unloads;
    unloads.reserve(handles.size());
    for (LiveSessionHandle& session : handles) {
        LiveSession* const target = session.get();
        unloads.push_back(std::async(std::launch::async, [target] {
            target->request_shutdown();
        }));
    }
    for (auto& unload : unloads) unload.get();

    ASSERT_TRUE(wait_for_live_count(manager, 0));
    for (const StoredSession& session : sessions) {
        const std::filesystem::path database =
            fixture.root() / "forums" / "lobby" / "sessions"
            / (session.identity.session_id + ".sqlite3");
        SessionLease lease = SessionLease::acquire(database);
        EXPECT_TRUE(lease.active());
    }
}

} // namespace
} // namespace cha::web
