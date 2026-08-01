#include "ui/web/web_session_runtime.h"

#include "ui/web/sse_mailbox.h"

#include "agents/agent.h"
#include "session/session_controller.h"
#include "session/session_database.h"
#include "session/session_lease.h"
#include "util/utf8_path.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

namespace cha::web {
namespace {

using namespace std::chrono_literals;

struct FakeState {
    std::mutex mutex;
    std::condition_variable entered;
    std::condition_variable release;
    std::thread::id owner_id;
    std::vector<std::string> raw_inputs;
    std::vector<std::string> default_ids;
    std::vector<std::size_t> raw_counts_at_receive;
    std::vector<int> receives_at_raw_input;
    int stops{};
    int receives{};
    int snapshot_calls{};
    int append_candidate_calls{};
    bool block_raw{};
    bool raw_entered{};
    bool released{};
    bool block_receive{};
    bool receive_entered{};
    bool receive_released{};
    bool flood_notifications{};
    bool render_on_receive{};
    bool end_on_receive{};
    bool shutdown{};
    bool throw_receive{};
    bool throw_shutdown{};
    bool destroyed{};
    bool fast_append_candidates{};
    SessionSnapshot snapshot{
        .transcript = {{
            .id = 7,
            .kind = TranscriptKind::agent,
            .participant_id = "guide",
            .display_name = "Guide",
            .text = "one",
            .status = TranscriptStatus::streaming,
            .request_id = 3,
        }},
        .generation = {
            .active = true,
            .request_id = 3,
            .agent_id = "guide",
            .agent_name = "Guide",
            .phase = GenerationPhase::answering,
        },
    };
};

class FakeController final : public WebSessionController {
public:
    FakeController(
        std::shared_ptr<FakeState> state,
        WakeNotifier&)
        : state_(std::move(state)) {}

    ~FakeController() override {
        std::lock_guard lock(state_->mutex);
        state_->destroyed = true;
        state_->entered.notify_all();
    }

    SessionUpdate handle_raw_input(std::string input) override {
        std::unique_lock lock(state_->mutex);
        check_owner();
        state_->raw_inputs.push_back(std::move(input));
        state_->receives_at_raw_input.push_back(state_->receives);
        state_->raw_entered = true;
        state_->entered.notify_all();
        state_->release.wait(lock, [this] {
            return !state_->block_raw || state_->released;
        });
        const std::string& command = state_->raw_inputs.back();
        std::optional<std::string> notice = "raw";
        if (command == "append") notice = std::nullopt;
        else if (command == "clear notice") notice = "";
        else if (command == "replace notice") notice = "replacement";
        return {
            .render_needed = command == "append"
                || command == "snapshot"
                || command == "clear notice"
                || command == "replace notice",
            .end_session = state_->raw_inputs.back() == "end session",
            .clear_input = true,
            .notice = std::move(notice),
        };
    }
    SessionUpdate request_stop() override {
        std::lock_guard lock(state_->mutex);
        check_owner();
        ++state_->stops;
        return {.notice = "stopped"};
    }
    SessionUpdate set_default_agent_id(std::string_view id) override {
        std::lock_guard lock(state_->mutex);
        check_owner();
        state_->default_ids.emplace_back(id);
        return {.notice = "default"};
    }
    SessionEventBatch receive(std::size_t) override {
        bool full = false;
        bool render_needed = false;
        bool end_session = false;
        {
            std::unique_lock lock(state_->mutex);
            check_owner();
            ++state_->receives;
            state_->raw_counts_at_receive.push_back(
                state_->raw_inputs.size());
            state_->receive_entered = true;
            state_->entered.notify_all();
            state_->release.wait(lock, [this] {
                return !state_->block_receive || state_->receive_released;
            });
            if (state_->throw_receive) {
                throw std::runtime_error("injected receive failure");
            }
            full = state_->flood_notifications;
            render_needed = state_->render_on_receive;
            state_->render_on_receive = false;
            end_session = state_->end_on_receive;
            state_->end_on_receive = false;
        }
        if (full) {
            return {
                .update = {
                    .render_needed = true,
                    .end_session = end_session,
                    .notice = "event",
                },
                .full = true,
            };
        }
        return {
            .update = {
                .render_needed = render_needed,
                .end_session = end_session,
            },
        };
    }
    void shutdown() override {
        std::lock_guard lock(state_->mutex);
        check_owner();
        state_->shutdown = true;
        state_->entered.notify_all();
        if (state_->throw_shutdown) {
            throw std::runtime_error("injected shutdown failure");
        }
    }
    SessionSnapshot snapshot() override {
        std::lock_guard lock(state_->mutex);
        check_owner();
        ++state_->snapshot_calls;
        return state_->snapshot;
    }
    std::optional<WebAppendCandidate> append_candidate(
        const SessionSnapshot& before) override {
        std::lock_guard lock(state_->mutex);
        check_owner();
        ++state_->append_candidate_calls;
        state_->entered.notify_all();
        if (!state_->fast_append_candidates) return std::nullopt;
        const SessionSnapshot& after = state_->snapshot;
        if (!before.transcript.empty()
            && before.transcript.size() == after.transcript.size()
            && after.transcript.back().status == TranscriptStatus::streaming
            && after.transcript.back().text.starts_with(
                before.transcript.back().text)
            && after.transcript.back().text.size()
                > before.transcript.back().text.size()) {
            return WebAppendCandidate{
                .target = AppendTargetEntry{after.transcript.back().id},
                .text = after.transcript.back().text.substr(
                    before.transcript.back().text.size()),
            };
        }
        if (after.generation.request_id
            && after.generation.reasoning_text.starts_with(
                before.generation.reasoning_text)
            && after.generation.reasoning_text.size()
                > before.generation.reasoning_text.size()) {
            return WebAppendCandidate{
                .target = AppendTargetReasoning{
                    *after.generation.request_id,
                },
                .text = after.generation.reasoning_text.substr(
                    before.generation.reasoning_text.size()),
            };
        }
        return std::nullopt;
    }

private:
    void check_owner() {
        if (state_->owner_id == std::thread::id{}) {
            state_->owner_id = std::this_thread::get_id();
        }
        EXPECT_EQ(state_->owner_id, std::this_thread::get_id());
    }
    std::shared_ptr<FakeState> state_;
};

class FakeSnapshotSink final : public WebSnapshotSink {
public:
    void publish(SnapshotEvent snapshot) override {
        {
            std::lock_guard lock(mutex);
            payloads.push_back(std::move(snapshot));
            next_append_seq = 0;
        }
        changed.notify_all();
    }
    void publish_append(
        WebAppendCandidate candidate,
        const SessionSnapshot&) override {
        {
            std::lock_guard lock(mutex);
            if (merge_pending_appends && !payloads.empty()) {
                if (auto* pending =
                        std::get_if<AppendEvent>(&payloads.back());
                    pending && same_append_target(
                        pending->target, candidate.target)) {
                    pending->text.append(candidate.text);
                    changed.notify_all();
                    return;
                }
            }
            payloads.push_back(AppendEvent{
                std::move(candidate.target),
                std::move(candidate.text),
                next_append_seq++,
            });
        }
        changed.notify_all();
    }
    bool wait_for_written(std::chrono::milliseconds deadline) override {
        std::unique_lock lock(mutex);
        ++drain_waits;
        return changed.wait_for(lock, deadline, [this] { return written || closed; });
    }
    void close() noexcept override {
        {
            std::lock_guard lock(mutex);
            closed = true;
            ++close_calls;
        }
        changed.notify_all();
    }
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::variant<SnapshotEvent, AppendEvent>> payloads;
    bool closed{};
    bool written{true};
    bool merge_pending_appends{};
    int drain_waits{};
    int close_calls{};
    std::uint64_t next_append_seq{};

private:
    static bool same_append_target(
        const AppendTarget& left,
        const AppendTarget& right) {
        if (left.index() != right.index()) return false;
        if (const auto* entry = std::get_if<AppendTargetEntry>(&left)) {
            return entry->entry_id
                == std::get<AppendTargetEntry>(right).entry_id;
        }
        return std::get<AppendTargetReasoning>(left).request_id
            == std::get<AppendTargetReasoning>(right).request_id;
    }
};

class TemporaryWebSession {
public:
    TemporaryWebSession()
        : path(std::filesystem::temp_directory_path()
               / ("cha_web_runtime_"
                  + std::to_string(std::chrono::steady_clock::now()
                                       .time_since_epoch()
                                       .count())
                  + ".sqlite3")) {
        if (!create_session_database(path, {"web-runtime", "forum", "Web runtime"})) {
            throw std::runtime_error("Failed to create web runtime test database");
        }
    }
    ~TemporaryWebSession() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
        std::filesystem::remove(SessionLease::companion_path(path), ignored);
    }

    std::filesystem::path path;
};

AgentDefinition test_definition() {
    return {
        .config = {.id = "guide", .name = "Guide", .host = "127.0.0.1", .port = 1},
        .system_prompt = "Test prompt",
    };
}

void execute_sql(
    const std::filesystem::path& path,
    const char* statement) {
    sqlite3* raw_database = nullptr;
    const int open_result = sqlite3_open_v2(
        utf8_path(path).c_str(),
        &raw_database,
        SQLITE_OPEN_READWRITE,
        nullptr);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> database(
        raw_database,
        &sqlite3_close_v2);
    if (open_result != SQLITE_OK) {
        throw std::runtime_error("Failed to open persistence-failure fixture");
    }
    char* raw_error = nullptr;
    const int execute_result =
        sqlite3_exec(database.get(), statement, nullptr, nullptr, &raw_error);
    const std::string error = raw_error ? raw_error : "unknown SQLite error";
    sqlite3_free(raw_error);
    if (execute_result != SQLITE_OK) {
        throw std::runtime_error(
            "Failed to inject persistence failure: " + error);
    }
}

WebControllerFactory real_factory(const std::filesystem::path& path) {
    return [path](WakeNotifier& notifier) {
        SessionLease lease = SessionLease::acquire(path);
        SessionRestore restore = load_session_state(path);
        return adapt_session_controller(SessionController::from_definitions(
            {test_definition()},
            path,
            std::move(lease),
            notifier,
            std::move(restore)));
    };
}

WebControllerFactory fake_factory(const std::shared_ptr<FakeState>& state) {
    return [state](WakeNotifier& notifier) {
        return std::make_unique<FakeController>(state, notifier);
    };
}

WebSettings test_settings(
    std::size_t queue_capacity,
    std::size_t command_batch_size = 8,
    std::size_t event_batch_size = 8) {
    WebSettings settings;
    settings.command_queue_capacity = queue_capacity;
    settings.command_batch_size = command_batch_size;
    settings.event_batch_size = event_batch_size;
    return settings;
}

TEST(WakeNotifier, RemembersWakeBeforeWait) {
    WakeNotifier notifier;
    notifier.wake();

    EXPECT_TRUE(notifier.wait_until(std::chrono::steady_clock::now() + 50ms));
}

TEST(WakeNotifier, CoalescesMultipleWakes) {
    WakeNotifier notifier;
    notifier.wake();
    notifier.wake();

    EXPECT_TRUE(notifier.wait_until(std::chrono::steady_clock::now() + 50ms));
    EXPECT_FALSE(notifier.wait_until(std::chrono::steady_clock::now() + 5ms));
}

TEST(WakeNotifier, ReturnsFalseAtDeadlineWithoutWake) {
    WakeNotifier notifier;

    EXPECT_FALSE(notifier.wait_until(std::chrono::steady_clock::now() + 5ms));
}

TEST(CommandCompletion, FirstCompletionWins) {
    CommandCompletion completion;
    completion.complete(CommandResult{.notice = "first"});
    completion.complete(ErrorCode::internal_error);

    const auto result = completion.wait_for(1ms);
    ASSERT_TRUE(result);
    EXPECT_EQ(std::get<CommandResult>(*result).notice, "first");
}

TEST(WebSessionRuntime, RoutesRawAndTypedCommandsOnOneOwnerThread) {
    auto state = std::make_shared<FakeState>();
    WebSessionRuntime runtime(fake_factory(state), test_settings(8));

    EXPECT_EQ(
        std::get<CommandResult>(runtime.submit(RawCommand{"/clear"}, 1s))
            .notice,
        "raw");
    EXPECT_EQ(
        std::get<CommandResult>(runtime.submit(StopCommand{}, 1s)).notice,
        "stopped");
    const CommandResult default_changed =
        std::get<CommandResult>(runtime.submit(
            SetDefaultAgentCommand{"stable-id"},
            1s));
    EXPECT_EQ(default_changed.notice, "default");
    EXPECT_FALSE(default_changed.clear_input);

    std::lock_guard lock(state->mutex);
    EXPECT_EQ(state->raw_inputs, std::vector<std::string>({"/clear"}));
    EXPECT_EQ(state->stops, 1);
    EXPECT_EQ(state->default_ids, std::vector<std::string>({"stable-id"}));
    EXPECT_NE(state->owner_id, std::this_thread::get_id());
}

TEST(WebSessionRuntime, TimeoutLeavesAcceptedCommandAliveAndLateCompletionSafe) {
    auto state = std::make_shared<FakeState>();
    state->block_raw = true;
    WebSessionRuntime runtime(fake_factory(state), test_settings(2));

    EXPECT_EQ(
        std::get<ErrorCode>(runtime.submit(RawCommand{"slow"}, 5ms)),
        ErrorCode::command_timeout);
    {
        std::unique_lock lock(state->mutex);
        ASSERT_TRUE(state->entered.wait_for(lock, 1s, [&] {
            return state->raw_entered;
        }));
        state->released = true;
    }
    state->release.notify_all();
    EXPECT_EQ(
        std::get<CommandResult>(runtime.submit(StopCommand{}, 1s)).notice,
        "stopped");
}

TEST(WebSessionRuntime, FullAndStoppingCommandsDoNotExecute) {
    auto state = std::make_shared<FakeState>();
    state->block_raw = true;
    WebSessionRuntime runtime(fake_factory(state), test_settings(1));
    auto first = std::async(std::launch::async, [&] {
        return runtime.submit(RawCommand{"first"}, 1s);
    });
    {
        std::unique_lock lock(state->mutex);
        ASSERT_TRUE(state->entered.wait_for(lock, 1s, [&] {
            return state->raw_entered;
        }));
    }
    EXPECT_EQ(
        std::get<ErrorCode>(runtime.submit(RawCommand{"second"}, 5ms)),
        ErrorCode::command_timeout);
    EXPECT_EQ(
        std::get<ErrorCode>(runtime.submit(StopCommand{}, 1s)),
        ErrorCode::command_queue_full);
    runtime.request_shutdown();
    EXPECT_EQ(
        std::get<ErrorCode>(runtime.submit(StopCommand{}, 1s)),
        ErrorCode::session_not_live);
    {
        std::lock_guard lock(state->mutex);
        state->released = true;
    }
    state->release.notify_all();
    (void)first.get();
    std::lock_guard lock(state->mutex);
    EXPECT_EQ(state->stops, 0);
    EXPECT_EQ(state->raw_inputs, std::vector<std::string>({"first"}));
}

TEST(WebSessionRuntime, ContinuesAfterAFullBatchWithOnlyACoalescedWake) {
    auto state = std::make_shared<FakeState>();
    state->block_receive = true;
    WebSessionRuntime runtime(
        fake_factory(state), test_settings(4, 2));
    {
        std::unique_lock lock(state->mutex);
        ASSERT_TRUE(state->entered.wait_for(lock, 1s, [&] {
            return state->receive_entered;
        }));
    }
    // Only the first push into the empty queue wakes the owner. It observes
    // that wake before starting a batch, leaving no later wake to mask a
    // missing full-batch continuation.
    for (int index = 0; index != 3; ++index) {
        EXPECT_EQ(
            std::get<ErrorCode>(runtime.submit(RawCommand{"queued"}, 1ms)),
            ErrorCode::command_timeout);
    }
    {
        std::lock_guard lock(state->mutex);
        state->receive_released = true;
    }
    state->release.notify_all();

    std::unique_lock lock(state->mutex);
    EXPECT_TRUE(state->entered.wait_for(lock, 500ms, [&] {
        return state->raw_inputs.size() == 3U;
    }));
    EXPECT_GE(state->receives, 2);
}

TEST(WebSessionRuntime, IndependentRuntimesProgressWithoutSharedState) {
    auto first_state = std::make_shared<FakeState>();
    auto second_state = std::make_shared<FakeState>();
    WebSessionRuntime first(fake_factory(first_state), test_settings(4));
    WebSessionRuntime second(fake_factory(second_state), test_settings(4));

    auto first_result = std::async(std::launch::async, [&] {
        return first.submit(RawCommand{"first"}, 1s);
    });
    auto second_result = std::async(std::launch::async, [&] {
        return second.submit(RawCommand{"second"}, 1s);
    });

    EXPECT_TRUE(std::holds_alternative<CommandResult>(first_result.get()));
    EXPECT_TRUE(std::holds_alternative<CommandResult>(second_result.get()));
    std::scoped_lock lock(first_state->mutex, second_state->mutex);
    EXPECT_EQ(first_state->raw_inputs, std::vector<std::string>({"first"}));
    EXPECT_EQ(second_state->raw_inputs, std::vector<std::string>({"second"}));
    EXPECT_NE(first_state->owner_id, second_state->owner_id);
}

TEST(WebSessionRuntime, InterleavesAgentDrainingWithCommandBatches) {
    auto state = std::make_shared<FakeState>();
    state->block_receive = true;
    WebSessionRuntime runtime(
        fake_factory(state), test_settings(8, 2));
    {
        std::unique_lock lock(state->mutex);
        ASSERT_TRUE(state->entered.wait_for(lock, 1s, [&] {
            return state->receive_entered;
        }));
    }
    for (int index = 0; index != 5; ++index) {
        EXPECT_EQ(
            std::get<ErrorCode>(runtime.submit(RawCommand{"/info"}, 1ms)),
            ErrorCode::command_timeout);
    }
    {
        std::lock_guard lock(state->mutex);
        state->receive_released = true;
    }
    state->release.notify_all();

    std::unique_lock lock(state->mutex);
    ASSERT_TRUE(state->entered.wait_for(lock, 500ms, [&] {
        return state->raw_inputs.size() == 5U;
    }));
    EXPECT_NE(
        std::find(
            state->raw_counts_at_receive.begin(),
            state->raw_counts_at_receive.end(),
            2U),
        state->raw_counts_at_receive.end());
    EXPECT_NE(
        std::find(
            state->raw_counts_at_receive.begin(),
            state->raw_counts_at_receive.end(),
            4U),
        state->raw_counts_at_receive.end());
}

TEST(WebSessionRuntime, NotificationPressureDoesNotStarveCommands) {
    auto state = std::make_shared<FakeState>();
    state->flood_notifications = true;
    WebSessionRuntime runtime(fake_factory(state), test_settings(8));

    {
        std::unique_lock lock(state->mutex);
        ASSERT_TRUE(state->entered.wait_for(lock, 1s, [&] {
            return state->receives >= 8;
        }));
    }

    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        runtime.submit(RawCommand{"during notification pressure"}, 1s)));

    std::lock_guard lock(state->mutex);
    EXPECT_TRUE(state->flood_notifications);
    ASSERT_EQ(state->receives_at_raw_input.size(), 1U);
    EXPECT_GE(state->receives_at_raw_input.front(), 8);
    EXPECT_EQ(
        state->raw_inputs,
        std::vector<std::string>({"during notification pressure"}));
}

TEST(WebSessionRuntime, CommandEndSessionStopsTheRuntime) {
    auto state = std::make_shared<FakeState>();
    WebSessionRuntime runtime(fake_factory(state), test_settings(2));

    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        runtime.submit(RawCommand{"end session"}, 1s)));
    EXPECT_EQ(
        std::get<ErrorCode>(runtime.submit(StopCommand{}, 1s)),
        ErrorCode::session_not_live);
}

TEST(WebSessionRuntime, ReceiveEndSessionStopsTheRuntime) {
    auto state = std::make_shared<FakeState>();
    state->end_on_receive = true;
    WebSessionRuntime runtime(fake_factory(state), test_settings(2));
    {
        std::unique_lock lock(state->mutex);
        ASSERT_TRUE(state->entered.wait_for(lock, 1s, [&] {
            return state->shutdown;
        }));
    }

    EXPECT_EQ(
        std::get<ErrorCode>(runtime.submit(StopCommand{}, 1s)),
        ErrorCode::session_not_live);
}

TEST(WebSessionRuntime, RejectsZeroBatchSizesBeforeStartingOwner) {
    WebSettings settings = test_settings(2);
    settings.command_batch_size = 0;

    EXPECT_THROW(
        (void)WebSessionRuntime(fake_factory(std::make_shared<FakeState>()), settings),
        std::invalid_argument);
}

TEST(WebSessionRuntime, CopiesSnapshotsAndUsesAppendOnlyWhenSafe) {
    auto state = std::make_shared<FakeState>();
    auto sink = std::make_shared<FakeSnapshotSink>();
    WebSessionRuntime runtime(
        fake_factory(state), test_settings(2),
        {.forum = {"forum", "Forum"}, .session_id = "session", .session_label = "Label"},
        sink);
    {
        std::unique_lock lock(sink->mutex);
        ASSERT_TRUE(sink->changed.wait_for(lock, 1s, [&] { return sink->payloads.size() == 1U; }));
        ASSERT_TRUE(std::holds_alternative<SnapshotEvent>(sink->payloads.front()));
        EXPECT_EQ(std::get<SnapshotEvent>(sink->payloads.front()).snapshot.transcript[0].text, "one");
    }
    // The command notice is structural, so establish it before testing the
    // subsequent text-only update.
    EXPECT_TRUE(std::holds_alternative<CommandResult>(runtime.submit(RawCommand{"snapshot"}, 1s)));
    {
        std::unique_lock lock(sink->mutex);
        ASSERT_TRUE(sink->changed.wait_for(lock, 1s, [&] { return sink->payloads.size() == 2U; }));
    }
    {
        std::lock_guard lock(state->mutex);
        state->snapshot.transcript[0].text = "one more";
    }
    EXPECT_TRUE(std::holds_alternative<CommandResult>(runtime.submit(RawCommand{"snapshot"}, 1s)));
    std::unique_lock lock(sink->mutex);
    ASSERT_TRUE(sink->changed.wait_for(lock, 1s, [&] { return sink->payloads.size() == 3U; }));
    ASSERT_TRUE(std::holds_alternative<AppendEvent>(sink->payloads.back()));
    EXPECT_EQ(std::get<AppendEvent>(sink->payloads.back()).text, " more");
    EXPECT_EQ(std::get<SnapshotEvent>(sink->payloads.front()).snapshot.transcript[0].text, "one");
    lock.unlock();

    // A render hint with no state change must not consume an append sequence
    // number or publish an empty append.
    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        runtime.submit(RawCommand{"snapshot"}, 1s)));
    std::lock_guard final_lock(sink->mutex);
    EXPECT_EQ(sink->payloads.size(), 3U);
}

TEST(WebSessionRuntime, SinkSequencesStoredPayloadsNotOwnerChanges) {
    auto state = std::make_shared<FakeState>();
    auto sink = std::make_shared<FakeSnapshotSink>();
    {
        std::lock_guard lock(sink->mutex);
        sink->merge_pending_appends = true;
    }
    WebSessionRuntime runtime(fake_factory(state), test_settings(2), {}, sink);
    {
        std::unique_lock lock(sink->mutex);
        ASSERT_TRUE(sink->changed.wait_for(
            lock,
            1s,
            [&] { return sink->payloads.size() == 1U; }));
    }

    {
        std::lock_guard lock(state->mutex);
        state->snapshot.transcript[0].text = "one A";
    }
    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        runtime.submit(RawCommand{"append"}, 1s)));
    {
        std::lock_guard lock(state->mutex);
        state->snapshot.transcript[0].text = "one AB";
    }
    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        runtime.submit(RawCommand{"append"}, 1s)));
    {
        std::lock_guard lock(sink->mutex);
        ASSERT_EQ(sink->payloads.size(), 2U);
        const AppendEvent& merged =
            std::get<AppendEvent>(sink->payloads.back());
        EXPECT_EQ(merged.text, " AB");
        EXPECT_EQ(merged.seq, 0U);
        sink->merge_pending_appends = false;
    }

    {
        std::lock_guard lock(state->mutex);
        state->snapshot.transcript[0].text = "one ABC";
    }
    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        runtime.submit(RawCommand{"append"}, 1s)));
    std::lock_guard lock(sink->mutex);
    ASSERT_EQ(sink->payloads.size(), 3U);
    const AppendEvent& next = std::get<AppendEvent>(sink->payloads.back());
    EXPECT_EQ(next.text, "C");
    EXPECT_EQ(next.seq, 1U);
}

TEST(WebSessionRuntime, TargetChangePublishesSnapshotBeforeResetSequence) {
    auto state = std::make_shared<FakeState>();
    state->fast_append_candidates = true;
    auto sink = std::make_shared<FakeSnapshotSink>();
    WebSessionRuntime runtime(fake_factory(state), test_settings(2), {}, sink);
    {
        std::unique_lock lock(sink->mutex);
        ASSERT_TRUE(sink->changed.wait_for(
            lock,
            1s,
            [&] { return sink->payloads.size() == 1U; }));
    }

    {
        std::lock_guard lock(state->mutex);
        state->snapshot.transcript[0].text = "one more";
    }
    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        runtime.submit(RawCommand{"append"}, 1s)));
    {
        std::lock_guard lock(state->mutex);
        state->snapshot.transcript[0].status = TranscriptStatus::complete;
        state->snapshot.generation.request_id = 9;
        state->snapshot.generation.phase = GenerationPhase::reasoning;
        state->snapshot.generation.reasoning_text = "think";
    }
    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        runtime.submit(RawCommand{"append"}, 1s)));
    {
        std::lock_guard lock(state->mutex);
        state->snapshot.generation.reasoning_text = "think more";
    }
    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        runtime.submit(RawCommand{"append"}, 1s)));

    std::lock_guard lock(sink->mutex);
    ASSERT_EQ(sink->payloads.size(), 4U);
    const AppendEvent& answer = std::get<AppendEvent>(sink->payloads[1]);
    EXPECT_TRUE(std::holds_alternative<AppendTargetEntry>(answer.target));
    EXPECT_EQ(answer.seq, 0U);
    EXPECT_TRUE(std::holds_alternative<SnapshotEvent>(sink->payloads[2]));
    const AppendEvent& reasoning =
        std::get<AppendEvent>(sink->payloads[3]);
    EXPECT_TRUE(
        std::holds_alternative<AppendTargetReasoning>(reasoning.target));
    EXPECT_EQ(reasoning.seq, 0U);
}

TEST(WebSessionRuntime, ProvenAppendBypassesFullSnapshotConstruction) {
    auto state = std::make_shared<FakeState>();
    state->fast_append_candidates = true;
    auto sink = std::make_shared<FakeSnapshotSink>();
    WebSessionRuntime runtime(fake_factory(state), test_settings(2), {}, sink);
    {
        std::unique_lock lock(sink->mutex);
        ASSERT_TRUE(sink->changed.wait_for(
            lock,
            1s,
            [&] { return sink->payloads.size() == 1U; }));
    }
    {
        std::lock_guard lock(state->mutex);
        EXPECT_EQ(state->snapshot_calls, 1);
        state->snapshot.transcript[0].text = "one more";
    }

    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        runtime.submit(RawCommand{"append"}, 1s)));
    {
        std::lock_guard lock(state->mutex);
        EXPECT_EQ(state->append_candidate_calls, 1);
        EXPECT_EQ(state->snapshot_calls, 1);
    }
    std::lock_guard lock(sink->mutex);
    ASSERT_EQ(sink->payloads.size(), 2U);
    EXPECT_TRUE(std::holds_alternative<AppendEvent>(sink->payloads.back()));
}

TEST(WebSessionRuntime, ConnectSnapshotBecomesAppendBaseWhileWriterIsStalled) {
    auto state = std::make_shared<FakeState>();
    state->fast_append_candidates = true;
    auto mailbox = std::make_shared<SseMailbox>();
    WebSessionRuntime runtime(
        fake_factory(state), test_settings(4), {}, mailbox);
    {
        std::unique_lock lock(state->mutex);
        ASSERT_TRUE(state->entered.wait_for(lock, 1s, [&] {
            return state->receive_entered;
        }));
        // Simulate a controller mutation that failed to issue a render hint.
        // The connect snapshot must still become the base for later deltas.
        state->snapshot.transcript[0].text = "one hidden";
    }

    CommandSubmitResult connected = runtime.connect_sse(1s);
    ASSERT_TRUE(std::holds_alternative<SseConnectResult>(connected));
    SseConnectResult connection =
        std::move(std::get<SseConnectResult>(connected));
    const SseMailbox::Next initial =
        connection.mailbox->next(connection.stream, 10ms);
    ASSERT_TRUE(initial.payload);
    const auto* snapshot =
        std::get_if<SnapshotEvent>(initial.payload.get());
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->snapshot.transcript[0].text, "one hidden");

    // Keep the initial payload in flight. Publishing and command completion on
    // the owner must continue without waiting for this simulated slow writer.
    {
        std::lock_guard lock(state->mutex);
        state->snapshot.transcript[0].text = "one hidden more";
    }
    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        runtime.submit(RawCommand{"append"}, 1s)));
    {
        std::lock_guard lock(state->mutex);
        state->snapshot.transcript[0].text = "one hidden more again";
        state->render_on_receive = true;
    }
    runtime.notifier_for_owner().wake();
    {
        std::unique_lock lock(state->mutex);
        ASSERT_TRUE(state->entered.wait_for(lock, 1s, [&] {
            return state->append_candidate_calls >= 2;
        }));
    }
    // Candidate construction is observed under the fake controller lock;
    // queue a later owner command to ensure the candidate has also reached the
    // mailbox before releasing the in-flight payload.
    EXPECT_TRUE(std::holds_alternative<SessionSnapshot>(runtime.snapshot(1s)));

    connection.mailbox->written(connection.stream);
    const SseMailbox::Next next =
        connection.mailbox->next(connection.stream, 10ms);
    ASSERT_TRUE(next.payload);
    const auto* append = std::get_if<AppendEvent>(next.payload.get());
    ASSERT_NE(append, nullptr);
    EXPECT_EQ(append->text, " more again");
    EXPECT_EQ(append->seq, 0U);
    connection.mailbox->end_stream(connection.stream);
}

TEST(WebSessionRuntime, ConnectWithNonStreamingSinkReportsInternalError) {
    auto state = std::make_shared<FakeState>();
    auto sink = std::make_shared<FakeSnapshotSink>();
    WebSessionRuntime runtime(
        fake_factory(state), test_settings(2), {}, sink);
    {
        std::unique_lock lock(sink->mutex);
        ASSERT_TRUE(sink->changed.wait_for(lock, 1s, [&] {
            return !sink->payloads.empty();
        }));
    }

    const CommandSubmitResult result = runtime.connect_sse(1s);
    ASSERT_TRUE(std::holds_alternative<ErrorCode>(result));
    EXPECT_EQ(std::get<ErrorCode>(result), ErrorCode::internal_error);
}

TEST(WebSessionRuntime, StructuralChangesAndNoticeSemanticsUseSnapshots) {
    auto state = std::make_shared<FakeState>();
    auto sink = std::make_shared<FakeSnapshotSink>();
    WebSessionRuntime runtime(fake_factory(state), test_settings(2), {}, sink);
    {
        std::unique_lock lock(sink->mutex);
        ASSERT_TRUE(sink->changed.wait_for(lock, 1s, [&] { return sink->payloads.size() == 1U; }));
    }

    EXPECT_TRUE(std::holds_alternative<CommandResult>(runtime.submit(RawCommand{"snapshot"}, 1s)));
    {
        std::unique_lock lock(sink->mutex);
        ASSERT_TRUE(sink->changed.wait_for(lock, 1s, [&] { return sink->payloads.size() == 2U; }));
        const auto& snapshot = std::get<SnapshotEvent>(sink->payloads.back()).snapshot;
        ASSERT_TRUE(snapshot.notice);
        EXPECT_EQ(*snapshot.notice, "raw");
    }
    EXPECT_TRUE(std::holds_alternative<CommandResult>(runtime.submit(RawCommand{"clear notice"}, 1s)));
    {
        std::unique_lock lock(sink->mutex);
        ASSERT_TRUE(sink->changed.wait_for(lock, 1s, [&] { return sink->payloads.size() == 3U; }));
        EXPECT_FALSE(std::get<SnapshotEvent>(sink->payloads.back()).snapshot.notice);
    }
    EXPECT_TRUE(std::holds_alternative<CommandResult>(runtime.submit(RawCommand{"replace notice"}, 1s)));
    {
        std::unique_lock lock(sink->mutex);
        ASSERT_TRUE(sink->changed.wait_for(lock, 1s, [&] { return sink->payloads.size() == 4U; }));
        EXPECT_EQ(std::get<SnapshotEvent>(sink->payloads.back()).snapshot.notice, "replacement");
    }
    {
        std::lock_guard lock(state->mutex);
        state->snapshot.transcript[0].status = TranscriptStatus::complete;
    }
    EXPECT_TRUE(std::holds_alternative<CommandResult>(runtime.submit(RawCommand{"snapshot"}, 1s)));
    std::unique_lock lock(sink->mutex);
    ASSERT_TRUE(sink->changed.wait_for(lock, 1s, [&] { return sink->payloads.size() == 5U; }));
    EXPECT_TRUE(std::holds_alternative<SnapshotEvent>(sink->payloads.back()));
}

TEST(WebSessionRuntime, ProductionAdapterCopiesControllerState) {
    TemporaryWebSession temporary;
    WakeNotifier notifier;
    SessionRestore restore;
    restore.entries.push_back(make_notice_entry(1, "before"));
    restore.next_entry_id = 2;
    auto adapted = adapt_session_controller(SessionController::from_definitions_for_testing(
        {test_definition()}, temporary.path, notifier, std::move(restore)));

    const SessionSnapshot before = adapted->snapshot();
    EXPECT_EQ(before.personas, std::vector<PersonaSummary>({{"guide", "Guide"}}));
    ASSERT_EQ(before.transcript.size(), 1U);
    EXPECT_EQ(before.transcript.front().text, "before");
    EXPECT_TRUE(adapted->handle_raw_input("/clear").render_needed);
    const SessionSnapshot after = adapted->snapshot();
    EXPECT_TRUE(after.transcript.empty());
    EXPECT_EQ(before.transcript.front().text, "before");
    adapted->shutdown();
}

TEST(WebSessionRuntime, FinalSnapshotUsesBoundedDrainAndHooks) {
    auto state = std::make_shared<FakeState>();
    auto sink = std::make_shared<FakeSnapshotSink>();
    int stopping = 0;
    int finished = 0;
    std::promise<void> finished_signal;
    auto finished_future = finished_signal.get_future();
    WebSettings settings = test_settings(2);
    settings.sse_drain_deadline = 10ms;
    {
        std::lock_guard lock(sink->mutex);
        sink->written = false;
    }
    WebSessionRuntime runtime(
        fake_factory(state), settings, {}, sink,
        {
            .mark_registry_stopping = [&] { ++stopping; },
            .mark_finished = [&] {
                ++finished;
                finished_signal.set_value();
            },
        });
    const auto started = std::chrono::steady_clock::now();
    runtime.request_shutdown();
    std::unique_lock lock(sink->mutex);
    ASSERT_TRUE(sink->changed.wait_for(lock, 1s, [&] { return sink->closed; }));
    EXPECT_EQ(sink->drain_waits, 1);
    EXPECT_GE(std::chrono::steady_clock::now() - started, settings.sse_drain_deadline);
    ASSERT_TRUE(std::holds_alternative<SnapshotEvent>(sink->payloads.back()));
    EXPECT_EQ(std::get<SnapshotEvent>(sink->payloads.back()).snapshot.lifecycle, SessionLifecycle::stopping);
    lock.unlock();
    ASSERT_EQ(finished_future.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(stopping, 1);
    EXPECT_EQ(finished, 1);
}

TEST(WebSessionRuntime, StoppedMailboxReaderExpiresFinalSnapshotDrain) {
    auto state = std::make_shared<FakeState>();
    std::promise<void> finished_signal;
    auto finished = finished_signal.get_future();
    WebSettings settings = test_settings(2);
    settings.sse_drain_deadline = 10ms;
    auto mailbox = std::make_shared<SseMailbox>();
    WebSessionRuntime runtime(
        fake_factory(state), settings, {}, mailbox,
        {.mark_finished = [&] { finished_signal.set_value(); }});

    CommandSubmitResult connected = runtime.connect_sse(1s);
    ASSERT_TRUE(std::holds_alternative<SseConnectResult>(connected));
    SseConnectResult connection =
        std::move(std::get<SseConnectResult>(connected));
    // Take the initial payload but never acknowledge it, matching a reader
    // that stopped after the server began the write.
    ASSERT_TRUE(
        connection.mailbox->next(connection.stream, 10ms).payload);

    const auto started = std::chrono::steady_clock::now();
    runtime.request_shutdown();
    ASSERT_EQ(finished.wait_for(1s), std::future_status::ready);
    EXPECT_GE(
        std::chrono::steady_clock::now() - started,
        settings.sse_drain_deadline);
    EXPECT_FALSE(
        connection.mailbox->next(connection.stream, 1ms).open);
}

TEST(WebSessionRuntime, WrittenFinalSnapshotEndsDrainImmediately) {
    auto state = std::make_shared<FakeState>();
    auto sink = std::make_shared<FakeSnapshotSink>();
    WebSettings settings = test_settings(2);
    settings.sse_drain_deadline = 1s;
    {
        std::lock_guard lock(sink->mutex);
        sink->written = true;
    }
    WebSessionRuntime runtime(fake_factory(state), settings, {}, sink);
    const auto started = std::chrono::steady_clock::now();
    runtime.request_shutdown();
    std::unique_lock lock(sink->mutex);
    ASSERT_TRUE(sink->changed.wait_for(lock, 1s, [&] { return sink->closed; }));
    EXPECT_LT(std::chrono::steady_clock::now() - started, 100ms);
}

TEST(WebSessionRuntime, ProcessStopWinsShutdownReason) {
    auto state = std::make_shared<FakeState>();
    state->block_receive = true;
    auto sink = std::make_shared<FakeSnapshotSink>();
    WebSessionRuntime runtime(fake_factory(state), test_settings(2), {}, sink);
    {
        std::unique_lock lock(state->mutex);
        ASSERT_TRUE(state->entered.wait_for(lock, 1s, [&] { return state->receive_entered; }));
    }
    runtime.request_shutdown();
    runtime.request_shutdown(ShutdownReason::server_stopping);
    {
        std::lock_guard lock(state->mutex);
        state->receive_released = true;
    }
    state->release.notify_all();
    std::unique_lock lock(sink->mutex);
    ASSERT_TRUE(sink->changed.wait_for(lock, 1s, [&] { return sink->closed; }));
    ASSERT_TRUE(std::holds_alternative<SnapshotEvent>(sink->payloads.back()));
    EXPECT_EQ(std::get<SnapshotEvent>(sink->payloads.back()).snapshot.shutdown_reason,
              ShutdownReason::server_stopping);
}

TEST(WebSessionRuntime, ConcurrentShutdownRequestsRunTeardownOnce) {
    auto state = std::make_shared<FakeState>();
    state->block_receive = true;
    auto sink = std::make_shared<FakeSnapshotSink>();
    int stopping = 0;
    int finished = 0;
    std::promise<void> finished_signal;
    auto finished_future = finished_signal.get_future();
    WebSessionRuntime runtime(
        fake_factory(state), test_settings(2), {}, sink,
        {
            .mark_registry_stopping = [&] { ++stopping; },
            .mark_finished = [&] {
                ++finished;
                finished_signal.set_value();
            },
        });
    {
        std::unique_lock lock(state->mutex);
        ASSERT_TRUE(state->entered.wait_for(lock, 1s, [&] { return state->receive_entered; }));
    }
    auto local_stop = std::async(std::launch::async, [&] { runtime.request_shutdown(); });
    auto process_stop = std::async(std::launch::async, [&] {
        runtime.request_shutdown(ShutdownReason::server_stopping);
    });
    local_stop.get();
    process_stop.get();
    {
        std::lock_guard lock(state->mutex);
        state->receive_released = true;
    }
    state->release.notify_all();
    std::unique_lock lock(sink->mutex);
    ASSERT_TRUE(sink->changed.wait_for(lock, 1s, [&] { return sink->closed; }));
    lock.unlock();
    ASSERT_EQ(finished_future.wait_for(1s), std::future_status::ready);
    EXPECT_EQ(stopping, 1);
    EXPECT_EQ(finished, 1);
    {
        std::lock_guard sink_lock(sink->mutex);
        EXPECT_EQ(
            std::get<SnapshotEvent>(sink->payloads.back())
                .snapshot.shutdown_reason,
            ShutdownReason::server_stopping);
    }
}

TEST(WebSessionRuntime, FatalOwnerFailureIsContainedAndSkipsDrainWait) {
    auto state = std::make_shared<FakeState>();
    state->throw_receive = true;
    auto sink = std::make_shared<FakeSnapshotSink>();
    int fatal_logs = 0;
    std::optional<WebSessionMetadata> logged_metadata;
    const WebSessionMetadata metadata{
        .forum = {"forum", "Forum"},
        .session_id = "session",
        .session_label = "Label",
    };
    WebSessionRuntime runtime(
        fake_factory(state), test_settings(2), metadata, sink,
        {.log_fatal = [&](const WebSessionMetadata& identity) {
            ++fatal_logs;
            logged_metadata = identity;
        }});
    std::unique_lock lock(sink->mutex);
    ASSERT_TRUE(sink->changed.wait_for(lock, 1s, [&] { return sink->closed; }));
    EXPECT_EQ(sink->drain_waits, 0);
    ASSERT_TRUE(std::holds_alternative<SnapshotEvent>(sink->payloads.back()));
    EXPECT_EQ(
        std::get<SnapshotEvent>(sink->payloads.back()).snapshot.shutdown_reason,
        ShutdownReason::session_failed);
    EXPECT_EQ(fatal_logs, 1);
    ASSERT_TRUE(logged_metadata);
    EXPECT_EQ(logged_metadata->forum, metadata.forum);
    EXPECT_EQ(logged_metadata->session_id, metadata.session_id);
}

TEST(WebSessionRuntime, FactoryFailureReleasesOpenHandoffLease) {
    TemporaryWebSession temporary;
    auto sink = std::make_shared<FakeSnapshotSink>();
    int stopping = 0;
    int finished = 0;
    int fatal_logs = 0;
    std::optional<WebSessionMetadata> logged_metadata;
    std::promise<void> finished_signal;
    auto finished_future = finished_signal.get_future();
    const WebSessionMetadata metadata{
        .forum = {"forum", "Forum"},
        .session_id = "failed-open",
        .session_label = "Failed open",
    };
    WebSessionRuntime runtime(
        [path = temporary.path](WakeNotifier&)
            -> std::unique_ptr<WebSessionController> {
            SessionLease lease = SessionLease::acquire(path);
            if (!lease.active()) {
                throw std::logic_error("factory did not acquire its lease");
            }
            throw std::runtime_error("injected factory failure");
        },
        test_settings(2),
        metadata,
        sink,
        {
            .mark_registry_stopping = [&] { ++stopping; },
            .mark_finished = [&] {
                ++finished;
                finished_signal.set_value();
            },
            .log_fatal = [&](const WebSessionMetadata& identity) {
                ++fatal_logs;
                logged_metadata = identity;
            },
        });

    ASSERT_EQ(finished_future.wait_for(1s), std::future_status::ready);
    std::lock_guard lock(sink->mutex);
    EXPECT_TRUE(sink->closed);
    EXPECT_EQ(sink->close_calls, 1);
    EXPECT_TRUE(sink->payloads.empty());
    EXPECT_EQ(sink->drain_waits, 0);
    EXPECT_EQ(stopping, 1);
    EXPECT_EQ(finished, 1);
    EXPECT_EQ(fatal_logs, 1);
    ASSERT_TRUE(logged_metadata);
    EXPECT_EQ(logged_metadata->session_id, metadata.session_id);
    SessionLease reopened = SessionLease::acquire(temporary.path);
    EXPECT_TRUE(reopened.active());
}

TEST(WebSessionRuntime, ThrowingControllerShutdownStillDestroysAndFinishes) {
    auto state = std::make_shared<FakeState>();
    state->throw_shutdown = true;
    auto sink = std::make_shared<FakeSnapshotSink>();
    int fatal_logs = 0;
    std::promise<void> finished_signal;
    auto finished_future = finished_signal.get_future();
    std::optional<WebSessionMetadata> logged_metadata;
    const WebSessionMetadata metadata{
        .forum = {"forum", "Forum"},
        .session_id = "throwing-shutdown",
        .session_label = "Throwing shutdown",
    };
    WebSessionRuntime runtime(
        fake_factory(state),
        test_settings(2),
        metadata,
        sink,
        {
            .mark_finished = [&] { finished_signal.set_value(); },
            .log_fatal = [&](const WebSessionMetadata& identity) {
                ++fatal_logs;
                logged_metadata = identity;
            },
        });

    runtime.request_shutdown();
    ASSERT_EQ(finished_future.wait_for(1s), std::future_status::ready);
    {
        std::lock_guard lock(state->mutex);
        EXPECT_TRUE(state->shutdown);
        EXPECT_TRUE(state->destroyed);
    }
    {
        std::lock_guard lock(sink->mutex);
        EXPECT_TRUE(sink->closed);
        EXPECT_EQ(sink->close_calls, 1);
    }
    EXPECT_EQ(fatal_logs, 1);
    ASSERT_TRUE(logged_metadata);
    EXPECT_EQ(logged_metadata->session_id, metadata.session_id);
}

TEST(WebSessionRuntime, PersistenceFailureReleasesOnlyTheFailingRuntimeLease) {
    TemporaryWebSession failing_session;
    TemporaryWebSession healthy_session;
    auto failing_sink = std::make_shared<FakeSnapshotSink>();
    auto healthy_sink = std::make_shared<FakeSnapshotSink>();
    std::promise<void> failing_finished_signal;
    auto failing_finished = failing_finished_signal.get_future();
    auto failing_runtime = std::make_unique<WebSessionRuntime>(
        real_factory(failing_session.path),
        test_settings(2),
        WebSessionMetadata{
            .forum = {"forum", "Forum"},
            .session_id = "failing",
            .session_label = "Failing",
        },
        failing_sink,
        WebRuntimeHooks{
            .mark_finished = [&] { failing_finished_signal.set_value(); },
        });
    WebSessionRuntime healthy_runtime(
        real_factory(healthy_session.path),
        test_settings(2),
        WebSessionMetadata{
            .forum = {"forum", "Forum"},
            .session_id = "healthy",
            .session_label = "Healthy",
        },
        healthy_sink);
    {
        std::unique_lock failing_lock(failing_sink->mutex);
        ASSERT_TRUE(failing_sink->changed.wait_for(
            failing_lock,
            1s,
            [&] { return !failing_sink->payloads.empty(); }));
    }
    {
        std::unique_lock healthy_lock(healthy_sink->mutex);
        ASSERT_TRUE(healthy_sink->changed.wait_for(
            healthy_lock,
            1s,
            [&] { return !healthy_sink->payloads.empty(); }));
    }

    // Corrupt only the first live journal after both controllers have opened.
    // Its next real controller write now fails immediately in start_turn().
    execute_sql(failing_session.path, "DROP TABLE turns");
    EXPECT_EQ(
        std::get<ErrorCode>(
            failing_runtime->submit(RawCommand{"Question"}, 20ms)),
        ErrorCode::command_timeout);
    {
        std::unique_lock lock(failing_sink->mutex);
        ASSERT_TRUE(failing_sink->changed.wait_for(
            lock,
            1s,
            [&] { return failing_sink->closed; }));
        ASSERT_TRUE(std::holds_alternative<SnapshotEvent>(
            failing_sink->payloads.back()));
        EXPECT_EQ(
            std::get<SnapshotEvent>(failing_sink->payloads.back())
                .snapshot.shutdown_reason,
            ShutdownReason::session_failed);
    }
    ASSERT_EQ(failing_finished.wait_for(1s), std::future_status::ready);

    EXPECT_TRUE(std::holds_alternative<CommandResult>(
        healthy_runtime.submit(StopCommand{}, 1s)));
    SessionLease reopened = SessionLease::acquire(failing_session.path);
    EXPECT_TRUE(reopened.active());
    EXPECT_THROW(
        (void)SessionLease::acquire(healthy_session.path),
        SessionBusyError);
}

} // namespace
} // namespace cha::web
