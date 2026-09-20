#include "web/live_session.h"

#include "web/live_session_manager.h"

#include "support/test_backends.h"
#include "support/test_live_session.h"
#include "util/path_name.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
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

app::RuntimeSettings test_settings(
    std::size_t queue_capacity = 8,
    std::size_t command_batch_size = 8,
    std::size_t event_batch_size = 8) {
    app::RuntimeSettings settings;
    settings.session_limit = 4;
    settings.command_queue_capacity = queue_capacity;
    settings.command_batch_size = command_batch_size;
    settings.event_batch_size = event_batch_size;
    return settings;
}

// A rendezvous the controller's activation hook enters on the owner thread.
// Blocking there is the deterministic way to hold the owner inside one command
// while a test exercises queue bounds and stopping behavior.
class OwnerGate {
public:
    void wait() {
        std::unique_lock lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return released_; });
    }
    bool wait_until_entered(std::chrono::milliseconds timeout = 2s) {
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

// Actor tests reach a LiveSession exactly the way production does: through the
// manager that constructs, publishes, and later joins it.
class LiveSessionHost {
public:
    LiveSessionHost(
        app::RuntimeSettings settings,
        SessionOpener opener,
        LiveSessionClock clock = {},
        FullSessionId key = {"forum", "session"})
        : key_(std::move(key)),
          manager_(std::move(settings), std::move(opener), std::move(clock)) {
        if (!std::holds_alternative<LiveSessionReady>(manager_.open(key_, 5s))) {
            throw std::runtime_error("Test live session did not open");
        }
        session_ = manager_.lookup(key_);
        if (!session_) throw std::runtime_error("Test live session is not live");
    }

    LiveSession* operator->() const noexcept { return session_.get(); }
    LiveSession& operator*() const noexcept { return *session_; }
    [[nodiscard]] const LiveSessionHandle& handle() const noexcept { return session_; }
    [[nodiscard]] LiveSessionManager& manager() noexcept { return manager_; }
    [[nodiscard]] const FullSessionId& key() const noexcept { return key_; }

private:
    FullSessionId key_;
    LiveSessionManager manager_;
    LiveSessionHandle session_;
};

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

SessionOpener scripted_opener(
    const std::filesystem::path& path,
    std::shared_ptr<test::BackendControls> controls,
    SessionController::ActivationHook before_activation = {}) {
    return [path, controls, before_activation](
               const FullSessionId& identity, std::shared_ptr<WakeNotifier> notifier) {
        return test::open_scripted_session(
            identity, path, notifier, controls, before_activation);
    };
}

std::shared_ptr<const app::SessionOutputItem> next_output(
    LiveSession& session,
    std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto item = session.take_output()) return item;
        std::this_thread::sleep_for(1ms);
    }
    return session.take_output();
}

void subscribe(LiveSession& session, std::string_view subscription_id = "sub-1") {
    CommandSubmitResult connected = session.subscribe(
        SubscribeCommand{
            .connection_id = "view-1",
            .context_epoch = 1,
            .subscription_id = std::string(subscription_id),
        },
        2s);
    if (!std::get_if<SubscribeResult>(&connected)) {
        throw std::runtime_error("subscribe was rejected");
    }
}

void execute_sql(const std::filesystem::path& path, const char* statement) {
    sqlite3* raw_database = nullptr;
    const int open_result = sqlite3_open_v2(
        utf8_path(path).c_str(), &raw_database, SQLITE_OPEN_READWRITE, nullptr);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> database(
        raw_database, &sqlite3_close_v2);
    if (open_result != SQLITE_OK) {
        throw std::runtime_error("Failed to open persistence-failure fixture");
    }
    char* raw_error = nullptr;
    const int execute_result =
        sqlite3_exec(database.get(), statement, nullptr, nullptr, &raw_error);
    const std::string error = raw_error ? raw_error : "unknown SQLite error";
    sqlite3_free(raw_error);
    if (execute_result != SQLITE_OK) {
        throw std::runtime_error("Failed to inject persistence failure: " + error);
    }
}

TEST(OwnerWakeSignal, CoalescesMultipleWakes) {
    OwnerWakeSignal notifier;
    EXPECT_FALSE(notifier.wait_until(std::chrono::steady_clock::now() + 5ms));
    notifier.wake();
    EXPECT_TRUE(notifier.wait_until(std::chrono::steady_clock::now() + 50ms));
    notifier.wake();
    notifier.wake();

    EXPECT_TRUE(notifier.wait_until(std::chrono::steady_clock::now() + 50ms));
    EXPECT_FALSE(notifier.wait_until(std::chrono::steady_clock::now() + 5ms));
}

TEST(CommandReply, FirstReplyWins) {
    CommandReply reply;
    EXPECT_TRUE(reply.complete(CommandResult{
        .session = {.notice = "first"}}));
    EXPECT_FALSE(reply.complete(ErrorCode::internal_error));

    const auto result = reply.wait_for(0ms);
    ASSERT_TRUE(result);
    EXPECT_EQ(std::get<CommandResult>(*result).session.notice, "first");
}

TEST(CommandReply, TimeoutAtomicallyAbandonsLateReply) {
    CommandReply reply;

    EXPECT_FALSE(reply.wait_for(0ms));
    EXPECT_FALSE(reply.complete(CommandResult{}));
}

TEST(LiveSession, RejectsZeroQueueAndBatchSizesBeforeStarting) {
    app::RuntimeSettings settings = test_settings(0);
    EXPECT_THROW(
        (void)validate_live_session_settings(settings), std::invalid_argument);

    settings = test_settings(2);
    settings.command_batch_size = 0;
    EXPECT_THROW(
        (void)validate_live_session_settings(settings), std::invalid_argument);

    settings = test_settings(2);
    settings.event_batch_size = 0;
    EXPECT_THROW(
        (void)validate_live_session_settings(settings), std::invalid_argument);
}

TEST(LiveSession, RoutesRawAndTypedCommandsOnOneOwnerThread) {
    test::TemporarySessionFile file("live_session_routing");
    auto guide = std::make_shared<test::BackendControls>();
    auto scribe = std::make_shared<test::BackendControls>();
    std::optional<std::string> persisted_default;
    SessionOpener opener = [path = file.path(), guide, scribe, &persisted_default](
                               const FullSessionId& identity,
                               std::shared_ptr<WakeNotifier> notifier) {
        std::vector<std::unique_ptr<test::DescribedModelBackend>> backends;
        backends.push_back(test::scripted_backend(guide, "guide", "Guide"));
        backends.push_back(test::scripted_backend(scribe, "scribe", "Scribe"));
        OpenedSession opened = test::open_scripted_session(
            identity, path, notifier, std::move(backends), {},
            PersonaRoster{
                {.id = "operator", .display_name = "Operator"},
                {.id = "reader", .display_name = "Reader"},
            });
        opened.persist_default_character = [&persisted_default](std::string_view id) {
            persisted_default = std::string(id);
        };
        return opened;
    };
    LiveSessionHost host(test_settings(), std::move(opener));

    ASSERT_TRUE(std::holds_alternative<CommandResult>(
        host->submit(RawCommand{"@- Note"}, 2s)));
    const auto covered = host->submit(CoverCommand{1}, 2s);
    ASSERT_TRUE(std::holds_alternative<CommandResult>(covered));
    EXPECT_TRUE(has_state_update(std::get<CommandResult>(covered).session));

    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        host->submit(StopCommand{}, 2s)));

    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        host->submit(SetDefaultCharacterCommand{"scribe"}, 2s)));
    EXPECT_EQ(persisted_default, "scribe");

    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        host->submit(SetDefaultCharacterCommand{"guide"}, 2s)));
    EXPECT_EQ(persisted_default, "guide");

    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        host->submit(SetDefaultCharacterCommand{"-"}, 2s)));
    EXPECT_EQ(persisted_default, "guide");
    const auto recording = host->snapshot(2s);
    ASSERT_TRUE(std::holds_alternative<SessionSnapshot>(recording));
    EXPECT_EQ(std::get<SessionSnapshot>(recording).default_character_id, "-");

    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        host->submit(SetDefaultCharacterCommand{"guide"}, 2s)));

    const auto renamed = host->submit(RenameSessionCommand{"Renamed live"}, 2s);
    ASSERT_TRUE(std::holds_alternative<SessionLabelResult>(renamed));
    EXPECT_EQ(std::get<SessionLabelResult>(renamed).label, "Renamed live");
    const auto state = host->snapshot(2s);
    ASSERT_TRUE(std::holds_alternative<SessionSnapshot>(state));
    EXPECT_EQ(std::get<SessionSnapshot>(state).default_character_id, "guide");
    EXPECT_EQ(std::get<SessionSnapshot>(state).characters.size(), 2U);
    EXPECT_EQ(std::get<SessionSnapshot>(state).session_label, "Renamed live");
    EXPECT_EQ(read_session_database_metadata(file.path()).label, "Renamed live");
}

TEST(LiveSession, MirrorsOnlyDurableRoundTripRenameAndCoverBoundaries) {
    test::TemporarySessionFile file("live_session_mirror_boundaries");
    auto controls = std::make_shared<test::BackendControls>();
    std::mutex mirror_mutex;
    std::size_t mirror_count{};
    std::string mirrored_label;
    std::vector<TranscriptEntry> mirrored_entries;
    SessionOpener opener = [path = file.path(), controls, &mirror_mutex,
                             &mirror_count, &mirrored_label,
                             &mirrored_entries](
                                const FullSessionId& identity,
                                std::shared_ptr<WakeNotifier> notifier) {
        OpenedSession opened = test::open_scripted_session(
            identity, path, notifier, controls);
        opened.mirror = [&mirror_mutex, &mirror_count, &mirrored_label,
                         &mirrored_entries](
                            std::string_view label,
                            std::span<const TranscriptEntry> entries) {
            const std::lock_guard lock(mirror_mutex);
            ++mirror_count;
            mirrored_label = label;
            mirrored_entries.assign(entries.begin(), entries.end());
        };
        return opened;
    };
    LiveSessionHost host(test_settings(), std::move(opener));

    ASSERT_TRUE(std::holds_alternative<CommandResult>(
        host->submit(RawCommand{"Question"}, 2s)));
    ASSERT_TRUE(controls->wait_until_running());
    {
        const std::lock_guard lock(mirror_mutex);
        EXPECT_EQ(mirror_count, 0U);
    }

    ASSERT_TRUE(std::holds_alternative<SessionLabelResult>(
        host->submit(RenameSessionCommand{"In flight"}, 2s)));
    {
        const std::lock_guard lock(mirror_mutex);
        EXPECT_EQ(mirror_count, 1U);
        EXPECT_EQ(mirrored_label, "In flight");
    }

    controls->emit_answer("Answer");
    controls->finish();
    ASSERT_TRUE(controls->wait_until_idle());
    bool completed = false;
    for (std::size_t attempt{}; attempt != 100; ++attempt) {
        const CommandSubmitResult result = host->snapshot(2s);
        ASSERT_TRUE(std::holds_alternative<SessionSnapshot>(result));
        if (!std::get<SessionSnapshot>(result).generation.active) {
            completed = true;
            break;
        }
    }
    ASSERT_TRUE(completed);
    {
        const std::lock_guard lock(mirror_mutex);
        EXPECT_EQ(mirror_count, 2U);
        ASSERT_EQ(mirrored_entries.size(), 2U);
        EXPECT_EQ(mirrored_entries.back().text, "Answer");
    }

    ASSERT_TRUE(std::holds_alternative<CommandResult>(
        host->submit(SetDefaultCharacterCommand{"guide"}, 2s)));
    {
        const std::lock_guard lock(mirror_mutex);
        EXPECT_EQ(mirror_count, 2U);
    }

    ASSERT_TRUE(std::holds_alternative<SessionLabelResult>(
        host->submit(RenameSessionCommand{"Renamed"}, 2s)));
    {
        const std::lock_guard lock(mirror_mutex);
        EXPECT_EQ(mirror_count, 3U);
        EXPECT_EQ(mirrored_label, "Renamed");
    }

    ASSERT_TRUE(std::holds_alternative<CommandResult>(
        host->submit(CoverCommand{2}, 2s)));
    {
        const std::lock_guard lock(mirror_mutex);
        EXPECT_EQ(mirror_count, 4U);
        EXPECT_EQ(mirrored_entries.size(), 3U);
    }

    ASSERT_TRUE(std::holds_alternative<CommandResult>(
        host->submit(RawCommand{"Cancelled question"}, 2s)));
    ASSERT_TRUE(controls->wait_until_running());
    ASSERT_TRUE(std::holds_alternative<CommandResult>(
        host->submit(StopCommand{}, 2s)));
    ASSERT_TRUE(controls->wait_until_idle());
    completed = false;
    for (std::size_t attempt{}; attempt != 100; ++attempt) {
        const CommandSubmitResult result = host->snapshot(2s);
        ASSERT_TRUE(std::holds_alternative<SessionSnapshot>(result));
        if (!std::get<SessionSnapshot>(result).generation.active) {
            completed = true;
            break;
        }
    }
    ASSERT_TRUE(completed);
    {
        const std::lock_guard lock(mirror_mutex);
        EXPECT_EQ(mirror_count, 5U);
        ASSERT_EQ(mirrored_entries.size(), 4U);
        EXPECT_EQ(mirrored_entries.back().text, "Cancelled question");
    }
}

TEST(LiveSession, KeepsADefaultCharacterThatCouldNotBeSaved) {
    test::TemporarySessionFile file("live_session_unsaved_default");
    auto guide = std::make_shared<test::BackendControls>();
    auto scribe = std::make_shared<test::BackendControls>();
    SessionOpener opener = [path = file.path(), guide, scribe](
                               const FullSessionId& identity, std::shared_ptr<WakeNotifier> notifier) {
        std::vector<std::unique_ptr<test::DescribedModelBackend>> backends;
        backends.push_back(test::scripted_backend(guide, "guide", "Guide"));
        backends.push_back(test::scripted_backend(scribe, "scribe", "Scribe"));
        OpenedSession opened = test::open_scripted_session(
            identity, path, notifier, std::move(backends));
        opened.persist_default_character = [](std::string_view) {
            throw std::runtime_error("workspace is read-only");
        };
        return opened;
    };
    LiveSessionHost host(test_settings(), std::move(opener));

    const auto result = host->submit(SetDefaultCharacterCommand{"scribe"}, 2s);
    ASSERT_TRUE(std::holds_alternative<CommandResult>(result));
    const auto& notice = std::get<CommandResult>(result).session.notice;
    ASSERT_TRUE(notice);
    EXPECT_EQ(*notice, "Default character is now Scribe (not saved)");

    const auto state = host->snapshot(2s);
    ASSERT_TRUE(std::holds_alternative<SessionSnapshot>(state));
    EXPECT_EQ(std::get<SessionSnapshot>(state).default_character_id, "scribe");
}

TEST(LiveSession, FullAndStoppingCommandsDoNotExecute) {
    test::TemporarySessionFile file("live_session_full_queue");
    auto controls = std::make_shared<test::BackendControls>();
    OwnerGate gate;
    LiveSessionHost host(
        test_settings(1),
        scripted_opener(file.path(), controls, [&gate](std::size_t) {
            gate.wait();
        }));

    auto blocked = std::async(std::launch::async, [&] {
        return host->submit(RawCommand{"First question"}, 5s);
    });
    ASSERT_TRUE(gate.wait_until_entered());

    EXPECT_EQ(
        std::get<ErrorCode>(host->submit(RawCommand{"queued"}, 5ms)),
        ErrorCode::command_timeout);
    EXPECT_EQ(
        std::get<ErrorCode>(host->submit(StopCommand{}, 1s)),
        ErrorCode::command_queue_full);

    host->request_shutdown();
    EXPECT_EQ(
        std::get<ErrorCode>(host->submit(StopCommand{}, 1s)),
        ErrorCode::session_not_live);

    gate.release();
    (void)blocked.get();
    EXPECT_TRUE(wait_for_finished(host.handle()));
}

TEST(LiveSession, TimeoutLeavesAcceptedCommandAliveAndLateReplySafe) {
    test::TemporarySessionFile file("live_session_timeout");
    auto controls = std::make_shared<test::BackendControls>();
    OwnerGate gate;
    LiveSessionHost host(
        test_settings(4),
        scripted_opener(file.path(), controls, [&gate](std::size_t) {
            gate.wait();
        }));

    EXPECT_EQ(
        std::get<ErrorCode>(host->submit(RawCommand{"Slow"}, 5ms)),
        ErrorCode::command_timeout);
    ASSERT_TRUE(gate.wait_until_entered());
    gate.release();

    // The abandoned reply is discarded, and the actor keeps serving.
    ASSERT_TRUE(controls->wait_until_running());
    controls->finish();
    const auto later = host->submit(StopCommand{}, 2s);
    EXPECT_TRUE(std::holds_alternative<CommandResult>(later));
}

TEST(LiveSession, ProcessShutdownRejectsQueuedMutationsEvenWhenTheQueueIsFull) {
    test::TemporarySessionFile file("live_session_shutdown_queue");
    auto controls = std::make_shared<test::BackendControls>();
    OwnerGate gate;
    LiveSessionHost host(
        test_settings(2),
        scripted_opener(file.path(), controls, [&gate](std::size_t) {
            gate.wait();
        }));
    const auto running = host->enqueue(RawCommand{"First question"});
    const bool entered = gate.wait_until_entered();
    if (!entered) gate.release();
    ASSERT_TRUE(entered);

    const auto rename = host->enqueue(RenameSessionCommand{"Must not be saved"});
    const auto prompt = host->enqueue(RawCommand{"Must not run"});
    host.manager().begin_shutdown();
    gate.release();

    for (const auto& queued : {rename, prompt}) {
        const auto* reply = std::get_if<std::shared_ptr<CommandReply>>(&queued);
        ASSERT_NE(reply, nullptr);
        const auto result = (*reply)->wait_for(2s);
        ASSERT_TRUE(result);
        ASSERT_TRUE(std::holds_alternative<ErrorCode>(*result));
        EXPECT_EQ(std::get<ErrorCode>(*result), ErrorCode::server_stopping);
    }
    ASSERT_TRUE(host.manager().join_shutdown(2s));
    const auto& reply = std::get<std::shared_ptr<CommandReply>>(running);
    EXPECT_TRUE(reply->wait_for(2s));
    EXPECT_EQ(read_session_database_metadata(file.path()).label, "Test session");
    const auto stored = load_session_state(file.path());
    EXPECT_TRUE(std::ranges::none_of(stored.entries, [](const auto& entry) {
        return entry.text == "Must not run";
    }));
}

TEST(LiveSession, NotificationPressureDoesNotStarveCommands) {
    test::TemporarySessionFile file("live_session_pressure");
    auto controls = std::make_shared<test::BackendControls>();
    // One event per drain keeps the owner reporting a full batch for as long
    // as the backend keeps producing.
    LiveSessionHost host(
        test_settings(8, 8, 1), scripted_opener(file.path(), controls));

    ASSERT_TRUE(std::holds_alternative<CommandResult>(
        host->submit(RawCommand{"Question"}, 2s)));
    ASSERT_TRUE(controls->wait_until_running());
    for (int index = 0; index != 200; ++index) {
        controls->emit_answer("fragment ");
    }

    // A snapshot is an ordinary owner-queue command, so completing it proves
    // event pressure never starves the command half of the loop.
    EXPECT_TRUE(std::holds_alternative<SessionSnapshot>(host->snapshot(2s)));
    controls->finish();
}

TEST(LiveSession, IndependentSessionsProgressWithoutSharedState) {
    test::TemporarySessionFile first_file("live_session_first");
    test::TemporarySessionFile second_file("live_session_second");
    auto first_controls = std::make_shared<test::BackendControls>();
    auto second_controls = std::make_shared<test::BackendControls>();
    LiveSessionManager manager(
        test_settings(),
        [&](const FullSessionId& identity, std::shared_ptr<WakeNotifier> notifier) {
            return test::open_scripted_session(
                identity,
                identity.session_id == "one" ? first_file.path()
                                             : second_file.path(),
                notifier,
                identity.session_id == "one" ? first_controls : second_controls);
        });
    const FullSessionId first_key{"forum", "one"};
    const FullSessionId second_key{"forum", "two"};
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open(first_key, 5s)));
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open(second_key, 5s)));
    LiveSessionHandle first = manager.lookup(first_key);
    LiveSessionHandle second = manager.lookup(second_key);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);

    auto first_result = std::async(std::launch::async, [&] {
        return first->submit(RawCommand{"First"}, 5s);
    });
    auto second_result = std::async(std::launch::async, [&] {
        return second->submit(RawCommand{"Second"}, 5s);
    });
    EXPECT_TRUE(std::holds_alternative<CommandResult>(first_result.get()));
    EXPECT_TRUE(std::holds_alternative<CommandResult>(second_result.get()));
    ASSERT_TRUE(first_controls->wait_until_running());
    ASSERT_TRUE(second_controls->wait_until_running());
    first_controls->finish();
    second_controls->finish();
}

TEST(LiveSession, PublishesExactAppendsAndSnapshotsForStructuralUpdates) {
    test::TemporarySessionFile file("live_session_appends");
    auto controls = std::make_shared<test::BackendControls>();
    LiveSessionHost host(test_settings(), scripted_opener(file.path(), controls));
    subscribe(*host);
    auto initial = next_output(*host);
    ASSERT_TRUE(initial);
    EXPECT_EQ(initial->kind, app::SessionOutputItem::Kind::snapshot);
    EXPECT_TRUE(initial->snapshot.transcript.empty());
    host->acknowledge_output();

    ASSERT_TRUE(std::holds_alternative<CommandResult>(
        host->submit(RawCommand{"Question"}, 2s)));
    auto structural = next_output(*host);
    ASSERT_TRUE(structural);
    EXPECT_EQ(structural->kind, app::SessionOutputItem::Kind::snapshot);
    EXPECT_FALSE(structural->snapshot.transcript.empty());
    host->acknowledge_output();

    ASSERT_TRUE(controls->wait_until_running());
    controls->emit_answer("one");
    auto first = next_output(*host);
    ASSERT_TRUE(first);
    host->acknowledge_output();
    if (first->kind == app::SessionOutputItem::Kind::snapshot) {
        controls->emit_answer(" more");
        first = next_output(*host);
        ASSERT_TRUE(first);
        host->acknowledge_output();
    }
    EXPECT_EQ(first->kind, app::SessionOutputItem::Kind::append);
    EXPECT_TRUE(std::holds_alternative<EntryTextTarget>(first->target));
    EXPECT_FALSE(first->text.empty());
    controls->finish();
}

TEST(LiveSession, StalledRendererDefersSnapshotCaptureUntilItRequestsDelivery) {
    test::TemporarySessionFile file("live_session_dirty_projection");
    auto controls = std::make_shared<test::BackendControls>();
    std::atomic_int captures{};
    auto opener = scripted_opener(file.path(), controls);
    LiveSessionHost host(test_settings(),
        [opener, &captures](const auto& identity, auto notifier) {
            auto opened = opener(identity, notifier);
            opened.cached_audio_entries = [&captures] {
                ++captures;
                return std::set<EntryId>{};
            };
            return opened;
        });
    subscribe(*host);
    auto initial = next_output(*host);
    ASSERT_TRUE(initial);
    const auto initial_captures = captures.load();
    ASSERT_TRUE(std::holds_alternative<CommandResult>(
        host->submit(RawCommand{"Question"}, 2s)));
    ASSERT_TRUE(controls->wait_until_running());
    for (int i = 0; i < 100; ++i) controls->emit_answer("word ");
    ASSERT_TRUE(std::holds_alternative<CommandResult>(host->submit(StopCommand{}, 2s)));
    const auto stopped = std::chrono::steady_clock::now() + 2s;
    while (!host->idle_for_retirement() && std::chrono::steady_clock::now() < stopped) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(host->idle_for_retirement());
    ASSERT_TRUE(std::holds_alternative<SessionLabelResult>(
        host->submit(RenameSessionCommand{"Settled"}, 2s)));
    EXPECT_EQ(captures.load(), initial_captures);
    EXPECT_FALSE(host->take_output());
    host->acknowledge_output();
    const auto repaired = next_output(*host);
    ASSERT_TRUE(repaired);
    EXPECT_EQ(repaired->kind, app::SessionOutputItem::Kind::snapshot);
    EXPECT_EQ(repaired->seq, 1U);
    EXPECT_FALSE(repaired->snapshot.generation.active);
    EXPECT_EQ(captures.load(), initial_captures + 1);
}

TEST(LiveSession, IncompatibleAppendTargetRepairsBrowserStateWithASnapshot) {
    test::TemporarySessionFile file("live_session_target_change");
    auto controls = std::make_shared<test::BackendControls>();
    LiveSessionHost host(test_settings(), scripted_opener(file.path(), controls));
    subscribe(*host);
    ASSERT_TRUE(next_output(*host));
    host->acknowledge_output();

    ASSERT_TRUE(std::holds_alternative<CommandResult>(
        host->submit(RawCommand{"Question"}, 2s)));
    ASSERT_TRUE(controls->wait_until_running());

    bool saw_reasoning_append = false;
    bool saw_answer_snapshot = false;
    const auto drain = [&] {
        for (int index = 0; index != 8; ++index) {
            auto item = next_output(*host, 300ms);
            if (!item) return;
            host->acknowledge_output();
            if (item->kind == app::SessionOutputItem::Kind::append) {
                if (std::holds_alternative<ReasoningTextTarget>(item->target)) {
                    saw_reasoning_append = true;
                }
                continue;
            }
            for (const TranscriptEntry& entry : item->snapshot.transcript) {
                if (entry.kind == EntryKind::character
                    && entry.text.find("answer") != std::string::npos) {
                    saw_answer_snapshot = true;
                }
            }
        }
    };

    controls->emit_reasoning("thinking");
    drain();
    controls->emit_reasoning(" harder");
    drain();
    EXPECT_TRUE(saw_reasoning_append);

    controls->emit_answer("answer");
    drain();
    controls->finish();
    drain();
    EXPECT_TRUE(saw_answer_snapshot);
}

TEST(LiveSession, PresentationChangesPublishOneSnapshotEach) {
    test::TemporarySessionFile file("live_session_notice");
    auto controls = std::make_shared<test::BackendControls>();
    LiveSessionHost host(test_settings(), scripted_opener(file.path(), controls));
    subscribe(*host);
    ASSERT_TRUE(next_output(*host));
    host->acknowledge_output();

    const auto result = host->submit(RawCommand{"/nonsense"}, 2s);
    ASSERT_TRUE(std::holds_alternative<CommandResult>(result));
    ASSERT_TRUE(std::get<CommandResult>(result).session.notice);
    EXPECT_NE(
        std::get<CommandResult>(result).session.notice->find("Unknown command"),
        std::string::npos);
    auto noticed = next_output(*host);
    ASSERT_TRUE(noticed);
    EXPECT_EQ(noticed->kind, app::SessionOutputItem::Kind::snapshot);
    ASSERT_TRUE(noticed->snapshot.notice);
    EXPECT_TRUE(noticed->snapshot.transcript.empty());
    EXPECT_NE(
        noticed->snapshot.notice->find("Unknown command"), std::string::npos);
    host->acknowledge_output();

    ASSERT_TRUE(std::holds_alternative<CommandResult>(
        host->submit(RawCommand{"/nonsense"}, 2s)));
    ASSERT_TRUE(std::holds_alternative<SessionSnapshot>(host->snapshot(2s)));
    EXPECT_FALSE(next_output(*host, 50ms));
}

TEST(LiveSession, PublishesAnOpenedSessionNoticeOnTheFirstSnapshot) {
    test::TemporarySessionFile file("live_session_open_notice");
    auto controls = std::make_shared<test::BackendControls>();
    LiveSessionHost host(
        test_settings(),
        [path = file.path(), controls](
            const FullSessionId& identity, std::shared_ptr<WakeNotifier> notifier) {
            OpenedSession opened = test::open_scripted_session(
                identity, path, notifier, controls);
            opened.notice =
                "Character settings could not be reloaded. This session is "
                "using the settings from startup.";
            return opened;
        });

    subscribe(*host);
    auto initial = next_output(*host);
    ASSERT_TRUE(initial);
    ASSERT_TRUE(initial->snapshot.notice);
    EXPECT_NE(
        initial->snapshot.notice->find("could not be reloaded"),
        std::string::npos);
}

TEST(LiveSession, DefaultShutdownPublishesSessionClosed) {
    test::TemporarySessionFile file("live_session_closed");
    auto controls = std::make_shared<test::BackendControls>();
    LiveSessionHost host(test_settings(), scripted_opener(file.path(), controls));
    subscribe(*host);
    ASSERT_TRUE(next_output(*host));
    host->acknowledge_output();

    host->request_shutdown();
    const auto terminal = next_output(*host);
    ASSERT_TRUE(terminal);
    EXPECT_EQ(terminal->kind, app::SessionOutputItem::Kind::snapshot);
    ASSERT_EQ(terminal->snapshot.shutdown_reason, ShutdownReason::session_closed);
    EXPECT_EQ(to_string(*terminal->snapshot.shutdown_reason), "session_closed");
    EXPECT_TRUE(wait_for_finished(host.handle()));
}

TEST(LiveSession, ReloadingOutranksSessionClosedOnTheFinalSnapshot) {
    test::TemporarySessionFile file("live_session_reloading");
    auto controls = std::make_shared<test::BackendControls>();
    LiveSessionHost host(test_settings(), scripted_opener(file.path(), controls));
    subscribe(*host);
    ASSERT_TRUE(next_output(*host));
    host->acknowledge_output();

    host->request_shutdown(ShutdownReason::reloading);
    auto final_payload = next_output(*host);
    ASSERT_TRUE(final_payload);
    EXPECT_EQ(final_payload->kind, app::SessionOutputItem::Kind::snapshot);
    EXPECT_EQ(
        final_payload->snapshot.shutdown_reason, ShutdownReason::reloading);
    EXPECT_TRUE(wait_for_finished(host.handle()));
}

TEST(LiveSession, FinalSnapshotIncludesReasonsRaisedDuringSnapshotConstruction) {
    test::TemporarySessionFile file("live_session_terminal_reason");
    OwnerGate gate;
    std::atomic<bool> block_snapshot{};
    LiveSessionHost host(test_settings(), [&](
                             const FullSessionId& identity,
                             std::shared_ptr<WakeNotifier> notifier) {
        auto opened = test::open_test_session(identity, file.path(), notifier);
        opened.cached_audio_entries = [&] {
            if (block_snapshot.exchange(false)) gate.wait();
            return std::set<EntryId>{};
        };
        return opened;
    });
    subscribe(*host);
    // Keep the initial payload in flight throughout terminal publication.
    ASSERT_TRUE(next_output(*host));
    block_snapshot.store(true);
    host->request_shutdown();
    const bool entered = gate.wait_until_entered();
    host->request_shutdown(ShutdownReason::reloading);
    host->request_shutdown(ShutdownReason::session_closed);
    gate.release();
    ASSERT_TRUE(entered);
    ASSERT_TRUE(wait_for_finished(host.handle()));

    // Once finalized, a later operation cannot rewrite that instance's event.
    host->request_shutdown(ShutdownReason::session_deleted);
    host->acknowledge_output();
    const auto terminal = next_output(*host);
    ASSERT_TRUE(terminal);
    EXPECT_EQ(terminal->snapshot.lifecycle, SessionLifecycle::stopping);
    EXPECT_EQ(terminal->snapshot.shutdown_reason, ShutdownReason::reloading);
    EXPECT_TRUE(host->output()->closed());
}

TEST(LiveSession, ResubscribeStartsFromAFreshSnapshot) {
    test::TemporarySessionFile file("live_session_reconnect");
    auto controls = std::make_shared<test::BackendControls>();
    LiveSessionHost host(test_settings(), scripted_opener(file.path(), controls));
    subscribe(*host, "sub-1");
    ASSERT_TRUE(next_output(*host));
    host->acknowledge_output();
    ASSERT_TRUE(std::holds_alternative<CommandResult>(
        host->unsubscribe(
            UnsubscribeCommand{
                .connection_id = "view-1",
                .context_epoch = 1,
                .subscription_id = "sub-1",
            },
            2s)));
    subscribe(*host, "sub-2");
    auto reconnected = next_output(*host);
    ASSERT_TRUE(reconnected);
    EXPECT_EQ(reconnected->kind, app::SessionOutputItem::Kind::snapshot);
}

TEST(LiveSession, ProcessStopCompletesWithoutWaitingForPresentation) {
    test::TemporarySessionFile file("live_session_process_stop");
    auto controls = std::make_shared<test::BackendControls>();
    LiveSessionHost host(test_settings(), scripted_opener(file.path(), controls));
    subscribe(*host);
    ASSERT_TRUE(next_output(*host));
    host->request_shutdown(ShutdownReason::server_stopping);
    EXPECT_EQ(
        std::get<ErrorCode>(host->submit(RawCommand{"late"}, 1s)),
        ErrorCode::server_stopping);
    EXPECT_TRUE(wait_for_finished(host.handle(), 500ms));
}

TEST(LiveSession, ConcurrentShutdownRequestsRunTeardownOnce) {
    test::TemporarySessionFile file("live_session_concurrent_stop");
    auto controls = std::make_shared<test::BackendControls>();
    LiveSessionHost host(test_settings(), scripted_opener(file.path(), controls));

    auto local_stop = std::async(std::launch::async, [&] {
        host->request_shutdown();
    });
    auto process_stop = std::async(std::launch::async, [&] {
        host->request_shutdown(ShutdownReason::server_stopping);
    });
    local_stop.get();
    process_stop.get();

    EXPECT_TRUE(wait_for_finished(host.handle()));
    EXPECT_EQ(host->lifecycle(), LiveSessionState::finished);
    host.manager().sweep();
    EXPECT_EQ(host.manager().snapshot().live_session_count, 0U);
}

TEST(LiveSession, ControllerFailureIsContainedAndReleasesOnlyThatSession) {
    test::TemporarySessionFile failing("live_session_failing");
    test::TemporarySessionFile healthy("live_session_healthy");
    LiveSessionManager manager(
        test_settings(),
        [&](const FullSessionId& identity, std::shared_ptr<WakeNotifier> notifier) {
            return test::open_test_session(
                identity,
                identity.session_id == "failing" ? failing.path() : healthy.path(),
                notifier);
        });
    const FullSessionId failing_key{"forum", "failing"};
    const FullSessionId healthy_key{"forum", "healthy"};
    ASSERT_TRUE(
        std::holds_alternative<LiveSessionReady>(manager.open(failing_key, 5s)));
    ASSERT_TRUE(
        std::holds_alternative<LiveSessionReady>(manager.open(healthy_key, 5s)));
    LiveSessionHandle failing_session = manager.lookup(failing_key);
    LiveSessionHandle healthy_session = manager.lookup(healthy_key);
    ASSERT_TRUE(failing_session);
    ASSERT_TRUE(healthy_session);

    // Corrupt only the first live journal after both controllers opened. Its
    // next real write now fails immediately inside the owner loop.
    execute_sql(failing.path(), "DROP TABLE turns");
    EXPECT_EQ(
        std::get<ErrorCode>(
            failing_session->submit(RawCommand{"Question"}, 200ms)),
        ErrorCode::internal_error);
    EXPECT_TRUE(wait_for_finished(failing_session));

    // The failure is isolated; the healthy actor keeps serving.
    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        healthy_session->submit(StopCommand{}, 2s)));
}

} // namespace
} // namespace cha
