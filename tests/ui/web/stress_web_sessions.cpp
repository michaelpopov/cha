#include "ui/web/session_registry.h"
#include "ui/web/sse_mailbox.h"

#include "session/session_lease.h"
#include "session/session_repository.h"
#include "support/test_web_graph.h"
#include "support/test_workspace.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace cha::web {
namespace {

PortBackedSession fake_session(
    const SessionIdentity& identity,
    std::unique_ptr<WebSessionController> controller) {
    return {{identity, "Test forum " + identity.forum_id,
             "Test session " + identity.session_id}, std::move(controller)};
}

using namespace std::chrono_literals;

struct Counts {
    std::atomic<unsigned int> first{};
    std::atomic<unsigned int> second{};
};

class CountingController final : public WebSessionController {
public:
    explicit CountingController(std::atomic<unsigned int>& count) : count_(count) {}

    CommandResult handle_raw_input(std::string_view, std::string) override {
        ++count_;
        return {.clear_input = true};
    }
    SessionChange request_stop() override { return {}; }
    SessionChange set_default_agent_id(std::string_view) override { return {}; }
    SessionEventBatch receive(std::size_t) override { return {}; }
    [[nodiscard]] bool is_generating() const override { return false; }
    void shutdown() override {}

private:
    std::atomic<unsigned int>& count_;
};

class CommandGate {
public:
    void wait() {
        std::unique_lock lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return released_; });
    }

    bool wait_until_entered() {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 1s, [this] { return entered_; });
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

class BlockingController final : public WebSessionController {
public:
    explicit BlockingController(CommandGate& gate) : gate_(gate) {}
    CommandResult handle_raw_input(std::string_view, std::string) override {
        gate_.wait();
        return {.clear_input = true};
    }
    SessionChange request_stop() override { return {}; }
    SessionChange set_default_agent_id(std::string_view) override { return {}; }
    SessionEventBatch receive(std::size_t) override { return {}; }
    [[nodiscard]] bool is_generating() const override { return false; }
    void shutdown() override {}

private:
    CommandGate& gate_;
};

class FatalRaceController final : public WebSessionController {
public:
    explicit FatalRaceController(std::atomic<bool>& fail) : fail_(fail) {}
    CommandResult handle_raw_input(std::string_view, std::string) override { return {}; }
    SessionChange request_stop() override { return {}; }
    SessionChange set_default_agent_id(std::string_view) override { return {}; }
    SessionEventBatch receive(std::size_t) override {
        if (fail_) throw std::runtime_error("injected fatal race");
        return {};
    }
    [[nodiscard]] bool is_generating() const override { return false; }
    void shutdown() override {}

private:
    std::atomic<bool>& fail_;
};

class GeneratingCountingController final : public WebSessionController {
public:
    explicit GeneratingCountingController(std::atomic<unsigned int>& count)
        : count_(count) {}
    CommandResult handle_raw_input(std::string_view, std::string) override {
        ++count_;
        return {.clear_input = true};
    }
    SessionChange request_stop() override { return {}; }
    SessionChange set_default_agent_id(std::string_view) override { return {}; }
    SessionEventBatch receive(std::size_t) override { return {}; }
    [[nodiscard]] bool is_generating() const override { return true; }
    SessionState state() override {
        return {
            .generation = {
                .active = true,
                .request_id = 1,
                .phase = ResponsePhase::answering,
            },
        };
    }
    void shutdown() override {}

private:
    std::atomic<unsigned int>& count_;
};

TEST(WebSessionStress, ConcurrentSessionsKeepCommandsOnIndependentQueues) {
    auto counts = std::make_shared<Counts>();
    WebSettings settings{.session_limit = 2, .command_queue_capacity = 64};
    SessionRegistry registry(
        settings,
        [counts](const SessionIdentity& key, WakeNotifier&) {
            return fake_session(key, std::make_unique<CountingController>(
                key.session_id == "first" ? counts->first : counts->second));
        });

    auto open = [&registry](std::string id) {
        return registry.open({"forum", std::move(id)}, 1s);
    };
    auto first_open = std::async(std::launch::async, open, "first");
    auto second_open = std::async(std::launch::async, open, "second");
    ASSERT_TRUE(std::holds_alternative<RegistryReady>(first_open.get()));
    ASSERT_TRUE(std::holds_alternative<RegistryReady>(second_open.get()));

    SessionHandle first = registry.lookup({"forum", "first"});
    SessionHandle second = registry.lookup({"forum", "second"});
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);

    constexpr unsigned int commands_per_session = 40;
    std::vector<std::future<CommandSubmitResult>> results;
    results.reserve(commands_per_session * 2);
    for (unsigned int index = 0; index != commands_per_session; ++index) {
        results.push_back(std::async(std::launch::async, [&first] {
            return first.runtime().submit(RawCommand{"reader", "first"}, 1s);
        }));
        results.push_back(std::async(std::launch::async, [&second] {
            return second.runtime().submit(RawCommand{"reader", "second"}, 1s);
        }));
    }
    for (auto& result : results) {
        EXPECT_TRUE(std::holds_alternative<CommandResult>(result.get()));
    }
    EXPECT_EQ(counts->first, commands_per_session);
    EXPECT_EQ(counts->second, commands_per_session);

    registry.begin_shutdown();
    EXPECT_TRUE(registry.join_shutdown(1s));
}

TEST(WebSessionStress, BlockedOwnerDoesNotDelayAnotherSession) {
    CommandGate gate;
    std::atomic<unsigned int> healthy_commands{};
    SessionRegistry registry(
        {.session_limit = 2, .command_queue_capacity = 8},
        [&](const SessionIdentity& key, WakeNotifier&) {
            if (key.session_id == "blocked") {
                return fake_session(
                    key, std::make_unique<BlockingController>(gate));
            }
            return fake_session(
                key, std::make_unique<CountingController>(healthy_commands));
        });
    ASSERT_TRUE(std::holds_alternative<RegistryReady>(
        registry.open({"forum", "blocked"}, 1s)));
    ASSERT_TRUE(std::holds_alternative<RegistryReady>(
        registry.open({"forum", "healthy"}, 1s)));
    SessionHandle blocked = registry.lookup({"forum", "blocked"});
    SessionHandle healthy = registry.lookup({"forum", "healthy"});
    ASSERT_TRUE(blocked);
    ASSERT_TRUE(healthy);

    auto blocked_command = std::async(std::launch::async, [&] {
        return blocked.runtime().submit(RawCommand{"reader", "wait"}, 50ms);
    });
    ASSERT_TRUE(gate.wait_until_entered());
    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        healthy.runtime().submit(RawCommand{"reader", "independent"}, 1s)));
    EXPECT_EQ(healthy_commands, 1U);
    EXPECT_EQ(
        std::get<ErrorCode>(blocked_command.get()),
        ErrorCode::command_timeout);

    gate.release();
    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        healthy.runtime().submit(RawCommand{"reader", "still-independent"}, 1s)));
    EXPECT_EQ(healthy_commands, 2U);
    registry.begin_shutdown();
    EXPECT_TRUE(registry.join_shutdown(1s));
}

TEST(WebSessionStress, RepeatedOpenUnloadReopenAndSweepRacesPreserveLimit) {
    constexpr int rounds = 20;
    constexpr int limit = 4;
    std::atomic<int> starts{};
    SessionRegistry registry(
        {.session_limit = limit},
        [&](const SessionIdentity& key, WakeNotifier&) {
            ++starts;
            static std::atomic<unsigned int> ignored_commands{};
            return fake_session(key, std::make_unique<CountingController>(ignored_commands));
        });

    for (int round = 0; round != rounds; ++round) {
        std::vector<SessionIdentity> keys;
        for (int index = 0; index != limit; ++index) {
            keys.push_back({
                "forum",
                "round-" + std::to_string(round)
                    + "-" + std::to_string(index),
            });
        }
        std::promise<void> start_signal;
        const std::shared_future<void> start =
            start_signal.get_future().share();
        std::vector<std::future<RegistryOpenResult>> opens;
        for (const SessionIdentity& key : keys) {
            for (int duplicate = 0; duplicate != 2; ++duplicate) {
                opens.push_back(std::async(
                    std::launch::async, [&, key, start] {
                        start.wait();
                        return registry.open(key, 1s);
                    }));
            }
        }
        start_signal.set_value();
        for (auto& open : opens) {
            ASSERT_TRUE(std::holds_alternative<RegistryReady>(open.get()));
        }
        const RegistryOpenResult overflow = registry.open(
            {"forum", "overflow-" + std::to_string(round)}, 10ms);
        ASSERT_TRUE(std::holds_alternative<RegistryOpenFailure>(overflow));
        EXPECT_EQ(
            std::get<RegistryOpenFailure>(overflow),
            RegistryOpenFailure::limit_reached);

        std::vector<SessionHandle> old_handles;
        for (const SessionIdentity& key : keys) {
            old_handles.push_back(registry.lookup(key));
            ASSERT_TRUE(old_handles.back());
            old_handles.back().runtime().request_shutdown();
        }
        old_handles.clear();

        std::atomic<bool> sweeping{true};
        auto sweeper = std::async(std::launch::async, [&] {
            while (sweeping) {
                registry.sweep();
                std::this_thread::yield();
            }
        });
        std::vector<std::future<SessionHandle>> reopened;
        for (const SessionIdentity& key : keys) {
            reopened.push_back(std::async(std::launch::async, [&, key] {
                const auto deadline = std::chrono::steady_clock::now() + 2s;
                while (std::chrono::steady_clock::now() < deadline) {
                    const RegistryOpenResult result = registry.open(key, 100ms);
                    if (std::holds_alternative<RegistryReady>(result)) {
                        SessionHandle handle = registry.lookup(key);
                        if (handle && std::holds_alternative<SessionSnapshot>(
                                handle.runtime().snapshot(100ms))) {
                            return handle;
                        }
                    }
                    std::this_thread::yield();
                }
                return SessionHandle{};
            }));
        }
        std::vector<SessionHandle> live_handles;
        bool all_reopened = true;
        for (auto& reopen : reopened) {
            live_handles.push_back(reopen.get());
            all_reopened = all_reopened && static_cast<bool>(live_handles.back());
        }
        sweeping = false;
        sweeper.get();
        ASSERT_TRUE(all_reopened);
        EXPECT_EQ(registry.snapshot().live_entry_count, limit);

        for (SessionHandle& handle : live_handles) {
            handle.runtime().request_shutdown();
        }
        live_handles.clear();
        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (registry.snapshot().live_entry_count != 0
            && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        ASSERT_EQ(registry.snapshot().live_entry_count, 0U);
    }
    EXPECT_EQ(starts, rounds * limit * 2);
}

TEST(WebSessionStress, FatalSessionDoesNotInterruptGeneratingPeer) {
    std::atomic<bool> fail{};
    std::atomic<unsigned int> healthy_commands{};
    SessionRegistry registry(
        {.session_limit = 2, .command_queue_capacity = 64},
        [&](const SessionIdentity& key, WakeNotifier&) {
            if (key.session_id == "failing") {
                return fake_session(
                    key, std::make_unique<FatalRaceController>(fail));
            }
            return fake_session(
                key, std::make_unique<GeneratingCountingController>(healthy_commands));
        });
    const SessionIdentity failing{"forum", "failing"};
    const SessionIdentity healthy{"forum", "healthy"};
    ASSERT_TRUE(std::holds_alternative<RegistryReady>(
        registry.open(failing, 1s)));
    ASSERT_TRUE(std::holds_alternative<RegistryReady>(
        registry.open(healthy, 1s)));
    SessionHandle failing_handle = registry.lookup(failing);
    SessionHandle healthy_handle = registry.lookup(healthy);
    ASSERT_TRUE(failing_handle);
    ASSERT_TRUE(healthy_handle);

    fail = true;
    failing_handle.runtime().notifier_for_owner().wake();
    std::vector<std::future<CommandSubmitResult>> commands;
    for (int index = 0; index != 40; ++index) {
        commands.push_back(std::async(std::launch::async, [&] {
            return healthy_handle.runtime().submit(RawCommand{"reader", "work"}, 1s);
        }));
    }
    for (auto& command : commands) {
        EXPECT_TRUE(std::holds_alternative<CommandResult>(command.get()));
    }
    EXPECT_EQ(healthy_commands, 40U);

    failing_handle = {};
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (registry.snapshot().live_entry_count != 1
        && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    EXPECT_EQ(registry.snapshot().live_entry_count, 1U);
    EXPECT_TRUE(registry.lookup(healthy));
    fail = false;
    EXPECT_TRUE(std::holds_alternative<RegistryReady>(
        registry.open(failing, 1s)));

    registry.begin_shutdown();
    EXPECT_TRUE(registry.join_shutdown(1s));
}

TEST(WebSessionStress, ConcurrentWorkspaceLifecycleKeepsMailboxesAndLeasesIndependent) {
    test::TestWorkspace fixture;
    const test::WebGraph graph(fixture.root());
    constexpr std::size_t session_count = 10;
    std::atomic<bool> creating{true};
    std::atomic<std::size_t> list_calls{};
    auto listing = std::async(std::launch::async, [&] {
        do {
            (void)graph.sessions->list("lobby");
            ++list_calls;
        } while (creating);
    });

    std::vector<std::future<SessionEntry>> creations;
    creations.reserve(session_count);
    for (std::size_t index = 0; index != session_count; ++index) {
        creations.push_back(std::async(std::launch::async, [&, index] {
            return graph.sessions->create(
                "lobby", "Load " + std::to_string(index));
        }));
    }
    std::vector<SessionEntry> sessions;
    sessions.reserve(session_count);
    for (auto& creation : creations) sessions.push_back(creation.get());
    creating = false;
    listing.get();
    EXPECT_GT(list_calls, 0U);
    EXPECT_EQ(graph.sessions->list("lobby").size(), session_count);

    WebSettings settings;
    settings.session_limit = session_count;
    settings.command_queue_capacity = 16;
    SessionRegistry registry(settings, graph.factory());
    std::vector<std::future<RegistryOpenResult>> opens;
    opens.reserve(session_count);
    for (const SessionEntry& session : sessions) {
        opens.push_back(std::async(std::launch::async, [&, key = session.identity] {
            return registry.open(key, 2s);
        }));
    }
    for (auto& open : opens) {
        ASSERT_TRUE(std::holds_alternative<RegistryReady>(open.get()));
    }
    EXPECT_EQ(registry.snapshot().live_entry_count, session_count);

    std::vector<SessionHandle> handles;
    std::vector<SseConnectResult> streams;
    std::set<const SseMailbox*> mailboxes;
    handles.reserve(session_count);
    streams.reserve(session_count);
    for (const SessionEntry& session : sessions) {
        handles.push_back(registry.lookup(session.identity));
        ASSERT_TRUE(handles.back());
        CommandSubmitResult connected = handles.back().runtime().connect_sse(1s);
        ASSERT_TRUE(std::holds_alternative<SseConnectResult>(connected));
        streams.push_back(std::get<SseConnectResult>(std::move(connected)));
        mailboxes.insert(streams.back().mailbox.get());
        const SseMailbox::Next initial = streams.back().mailbox->next(
            streams.back().stream, 1s);
        ASSERT_TRUE(initial.payload);
        streams.back().mailbox->written(streams.back().stream);
    }
    EXPECT_EQ(mailboxes.size(), session_count);

    for (const SessionEntry& session : sessions) {
        const std::filesystem::path database =
            fixture.root() / "forums" / "lobby" / "sessions"
            / (session.identity.session_id + ".sqlite3");
        EXPECT_THROW(
            (void)SessionLease::acquire(database),
            SessionBusyError);
    }

    std::vector<std::future<CommandSubmitResult>> commands;
    commands.reserve(session_count);
    for (SessionHandle& handle : handles) {
        SessionHandle* const target = &handle;
        commands.push_back(std::async(std::launch::async, [target] {
            return target->runtime().submit(RawCommand{"reader", "/clear"}, 2s);
        }));
    }
    for (auto& command : commands) {
        EXPECT_TRUE(std::holds_alternative<CommandResult>(command.get()));
    }

    for (std::size_t index = 0; index != handles.size(); ++index) {
        const std::size_t collapsed = streams[index].mailbox->end_stream(
            streams[index].stream);
        handles[index].runtime().disconnect_sse(
            streams[index].connection_id, collapsed);
    }
    std::vector<std::future<void>> unloads;
    unloads.reserve(handles.size());
    for (SessionHandle& handle : handles) {
        SessionHandle* const target = &handle;
        unloads.push_back(std::async(std::launch::async, [target] {
            target->runtime().request_shutdown();
        }));
    }
    for (auto& unload : unloads) unload.get();

    const auto unload_deadline = std::chrono::steady_clock::now() + 3s;
    while (registry.snapshot().live_entry_count != 0
        && std::chrono::steady_clock::now() < unload_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(registry.snapshot().live_entry_count, 0U);
    for (const SessionEntry& session : sessions) {
        const std::filesystem::path database =
            fixture.root() / "forums" / "lobby" / "sessions"
            / (session.identity.session_id + ".sqlite3");
        SessionLease lease = SessionLease::acquire(database);
        EXPECT_TRUE(lease.active());
    }
}

} // namespace
} // namespace cha::web
