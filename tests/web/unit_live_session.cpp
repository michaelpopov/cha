#include "web/live_session.h"

#include "web/live_session_manager.h"
#include "web/sse_mailbox.h"

#include "session/session_lease.h"
#include "support/test_backends.h"
#include "support/test_live_session.h"
#include "util/logging.h"
#include "util/utf8_path.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace cha::web {
namespace {

using namespace std::chrono_literals;

WebSettings test_settings(
    std::size_t queue_capacity = 8,
    std::size_t command_batch_size = 8,
    std::size_t event_batch_size = 8) {
    WebSettings settings;
    settings.session_limit = 4;
    settings.command_queue_capacity = queue_capacity;
    settings.command_batch_size = command_batch_size;
    settings.event_batch_size = event_batch_size;
    return settings;
}

// Owner-thread monotonic time under test control. It is a scalar dependency,
// not an ownership facade: the actor still owns its own clock reads.
class FakeSessionClock {
public:
    using Clock = std::chrono::steady_clock;

    FakeSessionClock() : now_(Clock::now() + 24h) {}

    Clock::time_point now() {
        std::lock_guard lock(mutex_);
        ++observations_;
        observed_.notify_all();
        return now_;
    }

    std::size_t advance(std::chrono::milliseconds amount) {
        std::lock_guard lock(mutex_);
        now_ += amount;
        return observations_ + 1;
    }

    bool wait_until_observed(std::size_t target) {
        std::unique_lock lock(mutex_);
        return observed_.wait_for(lock, 2s, [&] { return observations_ >= target; });
    }

private:
    std::mutex mutex_;
    std::condition_variable observed_;
    Clock::time_point now_;
    std::size_t observations_{};
};

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
        WebSettings settings,
        SessionOpener opener,
        LiveSessionClock clock = {},
        SessionIdentity key = {"forum", "session"})
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
    [[nodiscard]] const SessionIdentity& key() const noexcept { return key_; }

private:
    SessionIdentity key_;
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
               const SessionIdentity& identity, WakeNotifier& notifier) {
        return test::open_scripted_session(
            identity, path, notifier, controls, before_activation);
    };
}

SessionOpener two_agent_opener(
    const std::filesystem::path& path,
    std::shared_ptr<test::BackendControls> guide,
    std::shared_ptr<test::BackendControls> scribe) {
    return [path, guide, scribe](
               const SessionIdentity& identity, WakeNotifier& notifier) {
        std::vector<std::unique_ptr<ModelBackend>> backends;
        backends.push_back(test::scripted_backend(guide, "guide", "Guide"));
        backends.push_back(test::scripted_backend(scribe, "scribe", "Scribe"));
        return test::open_scripted_session(
            identity, path, notifier, std::move(backends));
    };
}

SessionOpener leased_opener(const std::filesystem::path& path) {
    return [path](const SessionIdentity& identity, WakeNotifier& notifier) {
        return test::open_leased_session(identity, path, notifier);
    };
}

// Reads the mailbox until it produces a payload or the deadline expires. The
// writer heartbeat is a real result, so a helper is the only way to keep these
// tests bounded without hiding delivery races.
std::shared_ptr<const SsePayload> next_payload(
    const SseConnectResult& connection,
    std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        SseMailbox::Next next = connection.mailbox->next(connection.stream, 10ms);
        if (!next.open) return {};
        if (next.payload) return next.payload;
    }
    return {};
}

SseConnectResult connect(LiveSession& session) {
    CommandSubmitResult connected = session.connect_sse(2s);
    SseConnectResult* result = std::get_if<SseConnectResult>(&connected);
    if (!result) throw std::runtime_error("SSE connect was rejected");
    return std::move(*result);
}

const SessionSnapshot& snapshot_of(const SsePayload& payload) {
    return std::get<SnapshotEvent>(payload).snapshot;
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

TEST(OwnerWakeSignal, RemembersWakeBeforeWait) {
    OwnerWakeSignal notifier;
    notifier.wake();

    EXPECT_TRUE(notifier.wait_until(std::chrono::steady_clock::now() + 50ms));
}

TEST(OwnerWakeSignal, CoalescesMultipleWakes) {
    OwnerWakeSignal notifier;
    notifier.wake();
    notifier.wake();

    EXPECT_TRUE(notifier.wait_until(std::chrono::steady_clock::now() + 50ms));
    EXPECT_FALSE(notifier.wait_until(std::chrono::steady_clock::now() + 5ms));
}

TEST(OwnerWakeSignal, ReturnsFalseAtDeadlineWithoutWake) {
    OwnerWakeSignal notifier;

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
    WebSettings settings = test_settings(0);
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

    settings = test_settings(2);
    settings.orphan_limit = settings.idle_grace - 1ms;
    EXPECT_THROW(
        (void)validate_live_session_settings(settings), std::invalid_argument);
}

TEST(LiveSession, RoutesRawAndTypedCommandsOnOneOwnerThread) {
    test::TemporarySessionFile file("live_session_routing");
    auto guide = std::make_shared<test::BackendControls>();
    auto scribe = std::make_shared<test::BackendControls>();
    LiveSessionHost host(
        test_settings(), two_agent_opener(file.path(), guide, scribe));

    const auto info = host->submit(RawCommand{"reader", "/info"}, 2s);
    ASSERT_TRUE(std::holds_alternative<CommandResult>(info));
    EXPECT_TRUE(std::get<CommandResult>(info).session.notice);

    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        host->submit(StopCommand{}, 2s)));

    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        host->submit(SetDefaultCharacterCommand{"scribe"}, 2s)));
    const auto state = host->snapshot(2s);
    ASSERT_TRUE(std::holds_alternative<SessionSnapshot>(state));
    EXPECT_EQ(std::get<SessionSnapshot>(state).default_character_id, "scribe");
    EXPECT_EQ(std::get<SessionSnapshot>(state).characters.size(), 2U);
}

TEST(LiveSession, UnknownCommandReportsANoticeWithoutTouchingTheTranscript) {
    test::TemporarySessionFile file("live_session_unknown");
    auto controls = std::make_shared<test::BackendControls>();
    LiveSessionHost host(test_settings(), scripted_opener(file.path(), controls));

    const auto result = host->submit(RawCommand{"reader", "/nonsense"}, 2s);
    ASSERT_TRUE(std::holds_alternative<CommandResult>(result));
    ASSERT_TRUE(std::get<CommandResult>(result).session.notice);
    EXPECT_NE(
        std::get<CommandResult>(result).session.notice->find("Unknown command"),
        std::string::npos);

    const auto state = host->snapshot(2s);
    ASSERT_TRUE(std::holds_alternative<SessionSnapshot>(state));
    EXPECT_TRUE(std::get<SessionSnapshot>(state).transcript.empty());
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
        return host->submit(RawCommand{"reader", "First question"}, 5s);
    });
    ASSERT_TRUE(gate.wait_until_entered());

    EXPECT_EQ(
        std::get<ErrorCode>(host->submit(RawCommand{"reader", "queued"}, 5ms)),
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
        std::get<ErrorCode>(host->submit(RawCommand{"reader", "Slow"}, 5ms)),
        ErrorCode::command_timeout);
    ASSERT_TRUE(gate.wait_until_entered());
    gate.release();

    // The abandoned reply is discarded, and the actor keeps serving.
    ASSERT_TRUE(controls->wait_until_running());
    controls->finish();
    const auto later = host->submit(StopCommand{}, 2s);
    EXPECT_TRUE(std::holds_alternative<CommandResult>(later));
}

TEST(LiveSession, NotificationPressureDoesNotStarveCommands) {
    test::TemporarySessionFile file("live_session_pressure");
    auto controls = std::make_shared<test::BackendControls>();
    // One event per drain keeps the owner reporting a full batch for as long
    // as the backend keeps producing.
    LiveSessionHost host(
        test_settings(8, 8, 1), scripted_opener(file.path(), controls));

    ASSERT_TRUE(std::holds_alternative<CommandResult>(
        host->submit(RawCommand{"reader", "Question"}, 2s)));
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
        [&](const SessionIdentity& identity, WakeNotifier& notifier) {
            return test::open_scripted_session(
                identity,
                identity.session_id == "one" ? first_file.path()
                                             : second_file.path(),
                notifier,
                identity.session_id == "one" ? first_controls : second_controls);
        });
    const SessionIdentity first_key{"forum", "one"};
    const SessionIdentity second_key{"forum", "two"};
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open(first_key, 5s)));
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open(second_key, 5s)));
    LiveSessionHandle first = manager.lookup(first_key);
    LiveSessionHandle second = manager.lookup(second_key);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);

    auto first_result = std::async(std::launch::async, [&] {
        return first->submit(RawCommand{"reader", "First"}, 5s);
    });
    auto second_result = std::async(std::launch::async, [&] {
        return second->submit(RawCommand{"reader", "Second"}, 5s);
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

    const SseConnectResult connection = connect(*host);
    std::shared_ptr<const SsePayload> initial = next_payload(connection);
    ASSERT_TRUE(initial);
    ASSERT_TRUE(std::holds_alternative<SnapshotEvent>(*initial));
    EXPECT_TRUE(snapshot_of(*initial).transcript.empty());
    connection.mailbox->written(connection.stream);

    // A new prompt is a structural transcript change, so it must arrive as a
    // full snapshot rather than an append.
    ASSERT_TRUE(std::holds_alternative<CommandResult>(
        host->submit(RawCommand{"reader", "Question"}, 2s)));
    std::shared_ptr<const SsePayload> structural = next_payload(connection);
    ASSERT_TRUE(structural);
    ASSERT_TRUE(std::holds_alternative<SnapshotEvent>(*structural));
    EXPECT_FALSE(snapshot_of(*structural).transcript.empty());
    connection.mailbox->written(connection.stream);

    ASSERT_TRUE(controls->wait_until_running());
    controls->emit_answer("one");
    std::shared_ptr<const SsePayload> first_append = next_payload(connection);
    ASSERT_TRUE(first_append);
    // The answer entry may still arrive as a snapshot when the controller
    // opened it in the same drain; either way the next fragment is an append.
    connection.mailbox->written(connection.stream);
    if (std::holds_alternative<SnapshotEvent>(*first_append)) {
        controls->emit_answer(" more");
        first_append = next_payload(connection);
        ASSERT_TRUE(first_append);
        connection.mailbox->written(connection.stream);
    }
    ASSERT_TRUE(std::holds_alternative<AppendEvent>(*first_append));
    const AppendEvent& append = std::get<AppendEvent>(*first_append);
    EXPECT_TRUE(std::holds_alternative<EntryTextTarget>(append.target));
    EXPECT_FALSE(append.text.empty());

    controls->finish();
    connection.mailbox->end_stream(connection.stream);
    host->disconnect_sse(connection.connection_id, 0);
}

TEST(LiveSession, IncompatibleAppendTargetRepairsBrowserStateWithASnapshot) {
    test::TemporarySessionFile file("live_session_target_change");
    auto controls = std::make_shared<test::BackendControls>();
    LiveSessionHost host(test_settings(), scripted_opener(file.path(), controls));
    const SseConnectResult connection = connect(*host);
    ASSERT_TRUE(next_payload(connection));
    connection.mailbox->written(connection.stream);

    ASSERT_TRUE(std::holds_alternative<CommandResult>(
        host->submit(RawCommand{"reader", "Question"}, 2s)));
    ASSERT_TRUE(controls->wait_until_running());

    // Reasoning text and answer text are different append targets. The first
    // reasoning fragment has no compatible base yet, so it arrives as a
    // snapshot that re-bases the stream on the reasoning target; the second is
    // then an exact append.
    bool saw_reasoning_append = false;
    bool saw_answer_snapshot = false;
    const auto drain = [&](bool& reasoning_flag, bool& answer_flag) {
        for (int index = 0; index != 8; ++index) {
            std::shared_ptr<const SsePayload> payload = next_payload(connection, 300ms);
            if (!payload) return;
            connection.mailbox->written(connection.stream);
            if (const auto* append = std::get_if<AppendEvent>(payload.get())) {
                if (std::holds_alternative<ReasoningTextTarget>(append->target)) {
                    reasoning_flag = true;
                }
                continue;
            }
            for (const cha::TranscriptEntry& entry : snapshot_of(*payload).transcript) {
                if (entry.kind == EntryKind::character
                    && entry.text.find("answer") != std::string::npos) {
                    answer_flag = true;
                }
            }
        }
    };

    controls->emit_reasoning("thinking");
    drain(saw_reasoning_append, saw_answer_snapshot);
    controls->emit_reasoning(" harder");
    drain(saw_reasoning_append, saw_answer_snapshot);
    EXPECT_TRUE(saw_reasoning_append);

    // Switching to answer text cannot be represented against the reasoning
    // base, so the actor repairs the browser with one fresh snapshot.
    controls->emit_answer("answer");
    drain(saw_reasoning_append, saw_answer_snapshot);
    controls->finish();
    drain(saw_reasoning_append, saw_answer_snapshot);
    EXPECT_TRUE(saw_answer_snapshot);
    connection.mailbox->end_stream(connection.stream);
    host->disconnect_sse(connection.connection_id, 0);
}

TEST(LiveSession, PresentationChangesPublishOneSnapshotEach) {
    test::TemporarySessionFile file("live_session_notice");
    auto controls = std::make_shared<test::BackendControls>();
    LiveSessionHost host(test_settings(), scripted_opener(file.path(), controls));
    const SseConnectResult connection = connect(*host);
    ASSERT_TRUE(next_payload(connection));
    connection.mailbox->written(connection.stream);

    ASSERT_TRUE(std::holds_alternative<CommandResult>(
        host->submit(RawCommand{"reader", "/nonsense"}, 2s)));
    std::shared_ptr<const SsePayload> noticed = next_payload(connection);
    ASSERT_TRUE(noticed);
    ASSERT_TRUE(std::holds_alternative<SnapshotEvent>(*noticed));
    ASSERT_TRUE(snapshot_of(*noticed).notice);
    EXPECT_NE(
        snapshot_of(*noticed).notice->find("Unknown command"), std::string::npos);
    connection.mailbox->written(connection.stream);

    // Repeating the same notice is not a presentation change.
    ASSERT_TRUE(std::holds_alternative<CommandResult>(
        host->submit(RawCommand{"reader", "/nonsense"}, 2s)));
    ASSERT_TRUE(std::holds_alternative<SessionSnapshot>(host->snapshot(2s)));
    EXPECT_FALSE(connection.mailbox->next(connection.stream, 50ms).payload);

    connection.mailbox->end_stream(connection.stream);
    host->disconnect_sse(connection.connection_id, 0);
}

TEST(LiveSession, OneActiveStreamRejectsConflictsAndIgnoresStaleCloses) {
    test::TemporarySessionFile file("live_session_streams");
    auto controls = std::make_shared<test::BackendControls>();
    LiveSessionHost host(test_settings(), scripted_opener(file.path(), controls));

    const SseConnectResult first = connect(*host);
    const auto rejected = host->connect_sse(2s);
    ASSERT_TRUE(std::holds_alternative<ErrorCode>(rejected));
    EXPECT_EQ(std::get<ErrorCode>(rejected), ErrorCode::browser_stream_in_use);

    first.mailbox->end_stream(first.stream);
    host->disconnect_sse(first.connection_id, 0);
    const SseConnectResult second = connect(*host);
    EXPECT_NE(second.connection_id, first.connection_id);

    // A stale close for the previous connection must not free the live one.
    host->disconnect_sse(first.connection_id, 0);
    const auto still_rejected = host->connect_sse(2s);
    ASSERT_TRUE(std::holds_alternative<ErrorCode>(still_rejected));
    EXPECT_EQ(
        std::get<ErrorCode>(still_rejected), ErrorCode::browser_stream_in_use);

    second.mailbox->end_stream(second.stream);
    host->disconnect_sse(second.connection_id, 0);
}

TEST(LiveSession, ReconnectStartsFromAFreshSnapshot) {
    test::TemporarySessionFile file("live_session_reconnect");
    auto controls = std::make_shared<test::BackendControls>();
    WebSettings settings = test_settings();
    settings.idle_grace = 5s;
    settings.orphan_limit = 10s;
    LiveSessionHost host(settings, scripted_opener(file.path(), controls));

    const SseConnectResult first = connect(*host);
    ASSERT_TRUE(next_payload(first));
    first.mailbox->end_stream(first.stream);
    host->disconnect_sse(first.connection_id, 0);
    ASSERT_TRUE(std::holds_alternative<SessionSnapshot>(host->snapshot(2s)));

    const SseConnectResult second = connect(*host);
    std::shared_ptr<const SsePayload> reconnected = next_payload(second);
    ASSERT_TRUE(reconnected);
    EXPECT_TRUE(std::holds_alternative<SnapshotEvent>(*reconnected));
    second.mailbox->end_stream(second.stream);
    host->disconnect_sse(second.connection_id, 0);
}

TEST(LiveSession, TimedOutSseConnectReleasesItsUnclaimedStream) {
    test::TemporarySessionFile file("live_session_abandoned_stream");
    auto controls = std::make_shared<test::BackendControls>();
    OwnerGate gate;
    LiveSessionHost host(
        test_settings(4),
        scripted_opener(file.path(), controls, [&gate](std::size_t) {
            gate.wait();
        }));

    EXPECT_EQ(
        std::get<ErrorCode>(host->submit(RawCommand{"reader", "Slow"}, 5ms)),
        ErrorCode::command_timeout);
    ASSERT_TRUE(gate.wait_until_entered());
    const CommandSubmitResult abandoned = host->connect_sse(5ms);
    gate.release();
    ASSERT_TRUE(std::holds_alternative<ErrorCode>(abandoned));
    EXPECT_EQ(std::get<ErrorCode>(abandoned), ErrorCode::command_timeout);

    ASSERT_TRUE(controls->wait_until_running());
    controls->finish();
    const SseConnectResult fresh = connect(*host);
    fresh.mailbox->end_stream(fresh.stream);
    host->disconnect_sse(fresh.connection_id, 0);
}

TEST(LiveSession, InitialIdleDeadlineUnloadsAnUnvisitedSession) {
    test::TemporarySessionFile file("live_session_idle");
    auto controls = std::make_shared<test::BackendControls>();
    auto clock = std::make_shared<FakeSessionClock>();
    WebSettings settings = test_settings();
    settings.idle_grace = 10ms;
    settings.orphan_limit = 20ms;
    LiveSessionHost host(
        settings, scripted_opener(file.path(), controls),
        [clock] { return clock->now(); });

    ASSERT_TRUE(std::holds_alternative<SessionSnapshot>(host->snapshot(2s)));
    const std::size_t before_deadline = clock->advance(9ms);
    ASSERT_TRUE(std::holds_alternative<SessionSnapshot>(host->snapshot(2s)));
    ASSERT_TRUE(clock->wait_until_observed(before_deadline));
    EXPECT_TRUE(std::holds_alternative<SessionSnapshot>(host->snapshot(2s)));

    const std::size_t at_deadline = clock->advance(1ms);
    (void)host->snapshot(2s);
    ASSERT_TRUE(clock->wait_until_observed(at_deadline));
    EXPECT_TRUE(wait_for_finished(host.handle()));
}

TEST(LiveSession, GeneratingDisconnectUsesOrphanLimitFromDisconnection) {
    test::TemporarySessionFile file("live_session_orphan");
    auto controls = std::make_shared<test::BackendControls>();
    auto clock = std::make_shared<FakeSessionClock>();
    WebSettings settings = test_settings();
    settings.idle_grace = 10ms;
    settings.orphan_limit = 100ms;
    LiveSessionHost host(
        settings, scripted_opener(file.path(), controls),
        [clock] { return clock->now(); });

    const SseConnectResult connection = connect(*host);
    connection.mailbox->end_stream(connection.stream);
    host->disconnect_sse(connection.connection_id, 0);
    ASSERT_TRUE(std::holds_alternative<CommandResult>(
        host->submit(RawCommand{"reader", "Question"}, 2s)));
    ASSERT_TRUE(controls->wait_until_running());

    // Generation is active, so the idle grace does not apply.
    const std::size_t before_orphan_limit = clock->advance(99ms);
    ASSERT_TRUE(std::holds_alternative<SessionSnapshot>(host->snapshot(2s)));
    ASSERT_TRUE(clock->wait_until_observed(before_orphan_limit));
    EXPECT_TRUE(std::holds_alternative<SessionSnapshot>(host->snapshot(2s)));

    const std::size_t at_orphan_limit = clock->advance(1ms);
    (void)host->snapshot(2s);
    ASSERT_TRUE(clock->wait_until_observed(at_orphan_limit));
    EXPECT_TRUE(wait_for_finished(host.handle()));
}

TEST(LiveSession, GenerationFinalizationReevaluatesDisconnectDeadline) {
    test::TemporarySessionFile file("live_session_idle_after_generation");
    auto controls = std::make_shared<test::BackendControls>();
    auto clock = std::make_shared<FakeSessionClock>();
    WebSettings settings = test_settings();
    settings.idle_grace = 10ms;
    settings.orphan_limit = 5s;
    LiveSessionHost host(
        settings, scripted_opener(file.path(), controls),
        [clock] { return clock->now(); });

    const SseConnectResult connection = connect(*host);
    connection.mailbox->end_stream(connection.stream);
    host->disconnect_sse(connection.connection_id, 0);
    ASSERT_TRUE(std::holds_alternative<CommandResult>(
        host->submit(RawCommand{"reader", "Question"}, 2s)));
    ASSERT_TRUE(controls->wait_until_running());

    const std::size_t at_idle_deadline = clock->advance(10ms);
    ASSERT_TRUE(std::holds_alternative<SessionSnapshot>(host->snapshot(2s)));
    ASSERT_TRUE(clock->wait_until_observed(at_idle_deadline));
    EXPECT_TRUE(std::holds_alternative<SessionSnapshot>(host->snapshot(2s)));

    // Once generation ends the disconnected session falls back to idle grace,
    // which the clock has already passed.
    controls->finish();
    EXPECT_TRUE(wait_for_finished(host.handle()));
}

TEST(LiveSession, ExitRequestFromRawInputStopsTheSession) {
    test::TemporarySessionFile file("live_session_exit");
    auto controls = std::make_shared<test::BackendControls>();
    LiveSessionHost host(test_settings(), scripted_opener(file.path(), controls));

    const auto exited = host->submit(RawCommand{"reader", "/exit"}, 2s);
    ASSERT_TRUE(std::holds_alternative<CommandResult>(exited));
    EXPECT_TRUE(std::get<CommandResult>(exited).close_session);
    EXPECT_TRUE(wait_for_finished(host.handle()));
    EXPECT_EQ(
        std::get<ErrorCode>(host->submit(StopCommand{}, 1s)),
        ErrorCode::session_not_live);
}

TEST(LiveSession, StalledReaderExpiresTheBoundedFinalDrain) {
    test::TemporarySessionFile file("live_session_drain");
    auto controls = std::make_shared<test::BackendControls>();
    WebSettings settings = test_settings();
    settings.sse_drain_deadline = 100ms;
    LiveSessionHost host(settings, scripted_opener(file.path(), controls));

    const SseConnectResult connection = connect(*host);
    // Take the initial payload but never acknowledge it, matching a reader
    // that stopped after the server began the write.
    ASSERT_TRUE(next_payload(connection));

    const auto started = std::chrono::steady_clock::now();
    host->request_shutdown();
    ASSERT_TRUE(wait_for_finished(host.handle()));
    EXPECT_GE(
        std::chrono::steady_clock::now() - started, settings.sse_drain_deadline);
    EXPECT_FALSE(connection.mailbox->next(connection.stream, 1ms).open);
}

TEST(LiveSession, AcknowledgedFinalSnapshotEndsTheDrainImmediately) {
    test::TemporarySessionFile file("live_session_fast_drain");
    auto controls = std::make_shared<test::BackendControls>();
    WebSettings settings = test_settings();
    settings.sse_drain_deadline = 5s;
    LiveSessionHost host(settings, scripted_opener(file.path(), controls));

    const SseConnectResult connection = connect(*host);
    ASSERT_TRUE(next_payload(connection));
    connection.mailbox->written(connection.stream);

    const auto started = std::chrono::steady_clock::now();
    host->request_shutdown();
    // The writer keeps acknowledging, so the drain must end well inside its
    // configured deadline.
    std::thread reader([&] {
        while (connection.mailbox->next(connection.stream, 5ms).open) {
            connection.mailbox->written(connection.stream);
        }
    });
    EXPECT_TRUE(wait_for_finished(host.handle()));
    reader.join();
    EXPECT_LT(std::chrono::steady_clock::now() - started, 2s);
}

TEST(LiveSession, ProcessStopWinsTheShutdownReasonAndSkipsTheDrain) {
    test::TemporarySessionFile file("live_session_process_stop");
    auto controls = std::make_shared<test::BackendControls>();
    WebSettings settings = test_settings();
    settings.sse_drain_deadline = 5s;
    LiveSessionHost host(settings, scripted_opener(file.path(), controls));

    const SseConnectResult connection = connect(*host);
    const std::shared_ptr<const SsePayload> initial = next_payload(connection);
    ASSERT_TRUE(initial);

    // Enter local teardown first and hold its final snapshot in flight. This
    // deterministically places the owner in the ordinary mailbox drain before
    // the process-wide reason arrives.
    host->request_shutdown(ShutdownReason::browser_disconnected);
    const auto stopping_deadline = std::chrono::steady_clock::now() + 2s;
    while (host->lifecycle() != LiveSessionState::stopping
        && std::chrono::steady_clock::now() < stopping_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_EQ(host->lifecycle(), LiveSessionState::stopping);
    connection.mailbox->written(connection.stream);
    const std::shared_ptr<const SsePayload> local_final = next_payload(connection);
    ASSERT_TRUE(local_final);
    ASSERT_TRUE(std::holds_alternative<SnapshotEvent>(*local_final));
    EXPECT_EQ(
        snapshot_of(*local_final).shutdown_reason,
        ShutdownReason::browser_disconnected);

    host->request_shutdown(ShutdownReason::server_stopping);
    EXPECT_EQ(
        std::get<ErrorCode>(host->submit(RawCommand{"reader", "late"}, 1s)),
        ErrorCode::server_stopping);
    // The higher-priority request interrupts the already-active five-second
    // local drain rather than merely waking the ordinary owner loop.
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
        [&](const SessionIdentity& identity, WakeNotifier& notifier) {
            return test::open_leased_session(
                identity,
                identity.session_id == "failing" ? failing.path() : healthy.path(),
                notifier);
        });
    const SessionIdentity failing_key{"forum", "failing"};
    const SessionIdentity healthy_key{"forum", "healthy"};
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
            failing_session->submit(RawCommand{"reader", "Question"}, 200ms)),
        ErrorCode::command_timeout);
    EXPECT_TRUE(wait_for_finished(failing_session));

    // The failing actor released its lease; the healthy one still holds its
    // own and keeps serving.
    SessionLease reopened = SessionLease::acquire(failing.path());
    EXPECT_TRUE(reopened.active());
    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        healthy_session->submit(StopCommand{}, 2s)));
    EXPECT_THROW((void)SessionLease::acquire(healthy.path()), SessionBusyError);
}

TEST(LiveSession, ReleasesItsLeaseBeforePublishingFinished) {
    test::TemporarySessionFile file("live_session_lease");
    LiveSessionManager manager(test_settings(), leased_opener(file.path()));
    const SessionIdentity key{"forum", "leased"};
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open(key, 5s)));
    LiveSessionHandle session = manager.lookup(key);
    ASSERT_TRUE(session);
    EXPECT_THROW((void)SessionLease::acquire(file.path()), SessionBusyError);

    session->request_shutdown();
    ASSERT_TRUE(wait_for_finished(session));
    // Finished is published only after the controller, and therefore the
    // cross-process lease, has been released.
    SessionLease reopened = SessionLease::acquire(file.path());
    EXPECT_TRUE(reopened.active());
}

TEST(LiveSession, GenerationLoggingRecordsStartAndTerminalTransitions) {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path()
        / ("cha_live_session_log_"
           + std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()));
    const std::filesystem::path log_path = directory / "cha.log";
    initialize_diagnostic_logging(log_path, "info");

    {
        test::TemporarySessionFile file("live_session_logging");
        auto controls = std::make_shared<test::BackendControls>();
        LiveSessionHost host(
            test_settings(), scripted_opener(file.path(), controls),
            {}, SessionIdentity{"lobby", "logged"});
        ASSERT_TRUE(std::holds_alternative<CommandResult>(
            host->submit(RawCommand{"reader", "Question"}, 2s)));
        ASSERT_TRUE(controls->wait_until_running());
        controls->emit_answer("answer");
        controls->finish();
        ASSERT_TRUE(controls->wait_until_idle());
        // Let the owner observe the terminal controller event.
        for (int index = 0; index != 20; ++index) {
            (void)host->snapshot(2s);
            std::this_thread::sleep_for(5ms);
        }
        host->request_shutdown();
        EXPECT_TRUE(wait_for_finished(host.handle()));
    }
    shutdown_diagnostic_logging();

    std::ifstream log(log_path);
    const std::string contents{
        std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);

    EXPECT_NE(
        contents.find("forum_id=lobby session_id=logged event=generation_started"),
        std::string::npos);
    EXPECT_NE(
        contents.find("event=generation_terminal request_id=1 status=complete"),
        std::string::npos);
    EXPECT_NE(contents.find("event=registry_running"), std::string::npos);
    EXPECT_NE(
        contents.find("event=lease_released_owner_finished"), std::string::npos);
}

} // namespace
} // namespace cha::web
