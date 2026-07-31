#include "session/session_controller.h"
#include "agents/agent.h"
#include "agents/completion_backend.h"
#include "session/session_database.h"
#include "support/test_backends.h"
#include "support/test_notifier.h"
#include "support/test_session_database.h"
#include "util/utf8_path.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace cha {
namespace {

test::TestNotifier& notifier() {
    static test::TestNotifier instance;
    return instance;
}

// Blocks the execution's final wake. This makes `execution_finished` true
// while the worker task is still live, so shutdown must join the pool rather
// than treating the registry's backend-safety barrier as full quiescence.
class FinalWakeBlockingNotifier final : public WakeNotifier {
public:
    void block_final_wake() noexcept {
        std::lock_guard lock(mutex_);
        block_final_wake_ = true;
    }

    void wake() noexcept override {
        std::unique_lock lock(mutex_);
        ++wake_count_;
        if (!block_final_wake_ || wake_count_ < 2) {
            return;
        }
        final_wake_blocked_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return release_final_wake_; });
    }

    bool wait_for_final_wake(
        std::chrono::milliseconds timeout = std::chrono::seconds(1)) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [this] {
            return final_wake_blocked_;
        });
    }

    void release_final_wake() noexcept {
        {
            std::lock_guard lock(mutex_);
            release_final_wake_ = true;
        }
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::size_t wake_count_{};
    bool block_final_wake_{};
    bool final_wake_blocked_{};
    bool release_final_wake_{};
};

std::vector<TranscriptEntry> copy_entries(const Transcript& transcript) {
    const std::span<const TranscriptEntry> entries = transcript.entries();
    return {entries.begin(), entries.end()};
}

// Removes one temporary session database when a controller test leaves scope.
class TemporaryJournal {
public:
    TemporaryJournal()
      : path(std::filesystem::temp_directory_path()
             / ("cha_controller_"
                + std::to_string(
                    std::chrono::steady_clock::now()
                        .time_since_epoch()
                        .count())
                + ".sqlite3")) {
        if (!create_session_database(
                path,
                {
                    .id = "controller-test",
                    .forum = "test-forum",
                    .label = "Controller test",
                })) {
            throw std::runtime_error("Failed to create controller test database");
        }
    }

    ~TemporaryJournal() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    std::filesystem::path path;
};

// Returns scripted completion output while retaining immutable inputs for assertions.
class ScriptedBackend final : public CompletionBackend {
public:
    ScriptedBackend(
        CompletionResult result = {},
        std::vector<std::string> deltas = {},
        bool wait_for_cancellation = false,
        std::string id = "guide-id",
        std::string name = "Guide")
      : id_(std::move(id)),
        name_(std::move(name)),
        result_(std::move(result)),
        wait_for_cancellation_(wait_for_cancellation) {
        for (std::string& delta : deltas) {
            deltas_.push_back({
                CompletionDeltaKind::answer,
                std::move(delta),
            });
        }
    }

    ScriptedBackend(
        std::vector<CompletionDelta> deltas,
        CompletionResult result = {},
        bool wait_for_cancellation = false,
        std::string id = "guide-id",
        std::string name = "Guide")
      : id_(std::move(id)),
        name_(std::move(name)),
        result_(std::move(result)),
        deltas_(std::move(deltas)),
        wait_for_cancellation_(wait_for_cancellation) {
    }

    RequestPayload prepare(const CompletionInput& input) override {
        inputs.push_back(input);
        model_contexts.push_back(project_agent_context(input, {}));
        return {.bytes = input.run.prompt_text};
    }

    CompletionResult perform(
        RequestPayload,
        const CompletionDeltaSink& on_delta,
        const std::atomic_bool& cancellation) override {
        for (const CompletionDelta& delta : deltas_) {
            on_delta(delta);
        }
        if (wait_for_cancellation_) {
            while (!cancellation.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return {CompletionOutcome::cancelled, {}};
        }
        return result_;
    }

    AgentRuntimeInfo info() const override {
        return {
            .persona = {
                .id = id_,
                .name = name_,
            },
            .model = "test-model",
            .api = "test://completion",
            .streaming = true,
        };
    }

    std::vector<CompletionInput> inputs;
    std::vector<std::vector<AgentMessage>> model_contexts;

private:
    std::string id_{"guide-id"};
    std::string name_{"Guide"};
    CompletionResult result_;
    std::vector<CompletionDelta> deltas_;
    bool wait_for_cancellation_{};
};

class ConcurrentBackend final : public CompletionBackend {
public:
    ConcurrentBackend(
        std::string id,
        std::string name,
        std::string answer,
        const std::atomic_bool* release = nullptr,
        std::size_t delta_count = 1)
        : id_(std::move(id)),
          name_(std::move(name)),
          answer_(std::move(answer)),
          release_(release),
          delta_count_(delta_count) {
    }

    RequestPayload prepare(const CompletionInput& input) override {
        inputs.push_back(input);
        return {.bytes = input.run.prompt_text};
    }

    CompletionResult perform(
        RequestPayload,
        const CompletionDeltaSink& on_delta,
        const std::atomic_bool& cancellation) override {
        entered.store(true, std::memory_order_release);
        while (release_ && !release_->load(std::memory_order_acquire)
               && !cancellation.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        if (cancellation.load(std::memory_order_acquire)) {
            finished.store(true, std::memory_order_release);
            return {CompletionOutcome::cancelled, {}};
        }
        for (std::size_t index = 0; index < delta_count_; ++index) {
            on_delta({CompletionDeltaKind::answer, answer_});
        }
        finished.store(true, std::memory_order_release);
        return {};
    }

    AgentRuntimeInfo info() const override {
        return {
            .persona = {.id = id_, .name = name_},
            .model = "test-model",
            .api = "test://completion",
            .streaming = true,
        };
    }

    std::vector<CompletionInput> inputs;
    std::atomic_bool entered{false};
    std::atomic_bool finished{false};

private:
    std::string id_;
    std::string name_;
    std::string answer_;
    const std::atomic_bool* release_{};
    std::size_t delta_count_{1};
};

class CancellationBlockingBackend final : public CompletionBackend {
public:
    CancellationBlockingBackend(
        std::string id,
        std::string name,
        std::atomic_bool& release)
        : id_(std::move(id)), name_(std::move(name)), release_(release) {
    }

    RequestPayload prepare(const CompletionInput& input) override {
        return {.bytes = input.run.prompt_text};
    }

    CompletionResult perform(
        RequestPayload,
        const CompletionDeltaSink&,
        const std::atomic_bool& cancellation) override {
        entered.store(true, std::memory_order_release);
        while (!cancellation.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        while (!release_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return {CompletionOutcome::cancelled, {}};
    }

    AgentRuntimeInfo info() const override {
        return {
            .persona = {.id = id_, .name = name_},
            .model = "test-model",
            .api = "test://completion",
            .streaming = true,
        };
    }

    std::atomic_bool entered{};

private:
    std::string id_;
    std::string name_;
    std::atomic_bool& release_;
};

class ThrowingPrepareBackend final : public CompletionBackend {
public:
    RequestPayload prepare(const CompletionInput&) override {
        throw std::runtime_error("preparation failed");
    }

    CompletionResult perform(
        RequestPayload,
        const CompletionDeltaSink&,
        const std::atomic_bool&) override {
        performed = true;
        return {};
    }

    AgentRuntimeInfo info() const override {
        return {
            .persona = {
                .id = "guide-id",
                .name = "Guide",
            },
            .model = "test-model",
            .api = "test://completion",
            .streaming = true,
        };
    }

    bool performed{};
};

SessionUpdate receive_until_idle(SessionController& controller) {
    SessionUpdate combined;
    while (controller.generation_status().active) {
        const std::size_t observed = notifier().wake_count();
        const SessionUpdate update = controller.receive();
        combined.render_needed =
            combined.render_needed || update.render_needed;
        combined.end_session =
            combined.end_session || update.end_session;
        combined.clear_input =
            combined.clear_input || update.clear_input;
        if (update.notice) {
            combined.notice = update.notice;
        }
        if (controller.generation_status().active
            && !notifier().wait_for_wake(observed)) {
            throw std::runtime_error(
                "Timed out waiting for controller event");
        }
    }
    return combined;
}

SessionUpdate receive_when_ready(SessionController& controller) {
    while (true) {
        const std::size_t observed = notifier().wake_count();
        SessionUpdate update = controller.receive();
        if (update.render_needed || update.end_session || update.notice) {
            return update;
        }
        if (!notifier().wait_for_wake(observed)) {
            throw std::runtime_error(
                "Timed out waiting for controller event");
        }
    }
}

SessionRestore restore_with(
    std::vector<TranscriptEntry> entries,
    RequestId next_request_id,
    EntryId next_entry_id) {
    return {
        .entries = std::move(entries),
        .next_request_id = next_request_id,
        .next_entry_id = next_entry_id,
    };
}

TEST(SessionController, RejectsEmptyAgentConfigurationWithRegistryMessage) {
    TemporaryJournal temporary;

    try {
        (void)SessionController::from_definitions_for_testing(
            {}, temporary.path, notifier());
        FAIL() << "Expected empty-agent configuration rejection";
    } catch (const std::invalid_argument& error) {
        EXPECT_EQ(
            error.what(),
            std::string("Agent registry requires at least one agent"));
    }
}

TEST(SessionController, OwnsACompleteIdentifiedTypedTurn) {
    TemporaryJournal temporary;
    const TranscriptEntry earlier =
        make_human_entry(10, "guide-id", "Guide", "Earlier", 16);
    {
        SessionJournal journal(temporary.path);
        journal.start_turn(16, earlier);
        journal.cancel_turn(16, std::nullopt);
    }
    auto backend = std::make_unique<ScriptedBackend>(
        CompletionResult{},
        std::vector<std::string>{"Hello", " there"});
    ScriptedBackend* backend_view = backend.get();
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::move(backend)),
        temporary.path,
        notifier(),
        restore_with({earlier}, 17, 11));

    const SessionUpdate submitted =
        controller->submit_prompt("Current");
    EXPECT_TRUE(submitted.render_needed);
    const SessionUpdate completed =
        receive_until_idle(*controller);
    EXPECT_FALSE(completed.end_session);

    ASSERT_EQ(backend_view->inputs.size(), 1U);
    const CompletionInput& request =
        backend_view->inputs.front();
    EXPECT_EQ(request.run.request_id, 17U);
    EXPECT_EQ(request.run.target.id, "guide-id");
    EXPECT_EQ(request.run.prompt_text, "Current");
    ASSERT_EQ(request.history->entries.size(), 1U);
    EXPECT_EQ(request.history->entries.front(), earlier);
    EXPECT_EQ(
        backend_view->model_contexts.front(),
        (std::vector<AgentMessage>{
            {AgentRole::user, "Earlier"},
            {AgentRole::user, "Current"},
        }));
    EXPECT_TRUE(completed.render_needed);

    const auto entries = copy_entries(controller->transcript());
    ASSERT_EQ(entries.size(), 3U);
    const TranscriptEntry& response = entries.back();
    EXPECT_EQ(response.kind, EntryKind::agent);
    EXPECT_EQ(response.participant_id, "guide-id");
    EXPECT_EQ(response.display_name, "Guide");
    EXPECT_EQ(response.text, "Hello there");
    EXPECT_EQ(response.status, EntryStatus::complete);
    EXPECT_EQ(load_transcript_entries(temporary.path), entries);
}

TEST(SessionController, PreparesTheSecondTurnFromTheSharedCompletedTranscript) {
    TemporaryJournal temporary;
    auto backend = std::make_unique<ScriptedBackend>(
        CompletionResult{}, std::vector<std::string>{"Answer"});
    ScriptedBackend* backend_view = backend.get();
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::move(backend)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("First");
    receive_until_idle(*controller);
    (void)controller->submit_prompt("Second");
    receive_until_idle(*controller);

    ASSERT_EQ(backend_view->model_contexts.size(), 2U);
    EXPECT_EQ(
        backend_view->model_contexts[1],
        (std::vector<AgentMessage>{
            {AgentRole::user, "First"},
            {AgentRole::assistant, "Answer"},
            {AgentRole::user, "Second"},
        }));
}

TEST(SessionController, ClearMakesTheNextRequestSeeOnlyPostClearContext) {
    TemporaryJournal temporary;
    auto backend = std::make_unique<ScriptedBackend>(
        CompletionResult{}, std::vector<std::string>{"Answer"});
    ScriptedBackend* backend_view = backend.get();
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::move(backend)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("First");
    receive_until_idle(*controller);
    (void)controller->clear_transcript();
    (void)controller->submit_prompt("Second");
    receive_until_idle(*controller);

    ASSERT_EQ(backend_view->model_contexts.size(), 2U);
    EXPECT_EQ(
        backend_view->model_contexts[1],
        (std::vector<AgentMessage>{{AgentRole::user, "Second"}}));
}

TEST(SessionController, ExcludesFailedTurnsFromTheFollowingModelContext) {
    TemporaryJournal temporary;
    auto backend = std::make_unique<ScriptedBackend>(
        CompletionResult{CompletionOutcome::transport_error, "unavailable"});
    ScriptedBackend* backend_view = backend.get();
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::move(backend)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("Failed");
    receive_until_idle(*controller);
    (void)controller->submit_prompt("Second");
    receive_until_idle(*controller);

    ASSERT_EQ(backend_view->model_contexts.size(), 2U);
    EXPECT_EQ(
        backend_view->model_contexts[1],
        (std::vector<AgentMessage>{{AgentRole::user, "Second"}}));
}

TEST(SessionController, ExcludesCancelledPartialOutputFromFollowingModelContext) {
    TemporaryJournal temporary;
    auto backend = std::make_unique<ScriptedBackend>(
        CompletionResult{CompletionOutcome::cancelled, {}},
        std::vector<std::string>{"Partial"});
    ScriptedBackend* backend_view = backend.get();
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::move(backend)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("First");
    receive_until_idle(*controller);
    (void)controller->submit_prompt("Second");
    receive_until_idle(*controller);

    ASSERT_EQ(backend_view->model_contexts.size(), 2U);
    EXPECT_EQ(
        backend_view->model_contexts[1],
        (std::vector<AgentMessage>{
            {AgentRole::user, "First"},
            {AgentRole::user, "Second"},
        }));
}

TEST(SessionController, PersistsAnIdentifiedCancelledResponse) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>(
            CompletionResult{CompletionOutcome::cancelled, {}},
            std::vector<std::string>{"Partial"})),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("Question");
    const SessionUpdate update =
        receive_until_idle(*controller);

    ASSERT_TRUE(update.notice);
    EXPECT_EQ(*update.notice, "Generation stopped");
    const auto restored =
        load_transcript_entries(temporary.path);
    ASSERT_EQ(restored.size(), 2U);
    EXPECT_EQ(restored.back().kind, EntryKind::agent);
    EXPECT_EQ(
        restored.back().status,
        EntryStatus::cancelled);
    EXPECT_EQ(restored.back().text, "Partial");
}

TEST(SessionController, RecordsCancellationWithoutAnEmptyAssistantEntry) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>(
            CompletionResult{CompletionOutcome::cancelled, {}})),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("Question");
    receive_until_idle(*controller);

    const auto entries = copy_entries(controller->transcript());
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().kind, EntryKind::human);
    EXPECT_EQ(load_transcript_entries(temporary.path), entries);
}

TEST(SessionController, KeepsReasoningEphemeralWhileAnswerEntersTranscript) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>(
            CompletionResult{},
            std::vector<std::string>{},
            true)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("Question");
    EXPECT_EQ(
        controller->generation_status().phase,
        ResponsePhase::waiting);

    (void)controller->handle_agent_event(AgentDelta{
        1,
        CompletionDeltaKind::reasoning,
        "PRIVATE_REASONING",
    });
    EXPECT_EQ(
        controller->generation_status().phase,
        ResponsePhase::reasoning);
    EXPECT_EQ(
        controller->generation_status().reasoning_text,
        "PRIVATE_REASONING");
    ASSERT_EQ(controller->transcript().entries().size(), 1U);

    (void)controller->handle_agent_event(AgentDelta{
        1,
        CompletionDeltaKind::answer,
        "Answer",
    });
    EXPECT_EQ(
        controller->generation_status().phase,
        ResponsePhase::answering);
    ASSERT_EQ(controller->transcript().entries().size(), 2U);
    EXPECT_EQ(controller->transcript().entries().back().text, "Answer");
    (void)controller->handle_agent_event(AgentDelta{
        1,
        CompletionDeltaKind::reasoning,
        " late",
    });
    EXPECT_EQ(
        controller->generation_status().phase,
        ResponsePhase::answering);
    EXPECT_EQ(
        controller->generation_status().reasoning_text,
        "PRIVATE_REASONING late");

    (void)controller->handle_agent_event(AgentCompleted{1});
    EXPECT_FALSE(controller->generation_status().active);
    EXPECT_TRUE(controller->generation_status().reasoning_text.empty());
    const std::vector<TranscriptEntry> live =
        copy_entries(controller->transcript());
    ASSERT_EQ(live.size(), 2U);
    EXPECT_EQ(live.back().text, "Answer");
    EXPECT_EQ(live.back().status, EntryStatus::complete);

    const std::vector<TranscriptEntry> restored =
        load_transcript_entries(temporary.path);
    EXPECT_EQ(restored, live);
}

TEST(SessionController, ReasoningOnlyCancellationLeavesNoTranscriptEntry) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>(
            CompletionResult{},
            std::vector<std::string>{},
            true)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("Question");
    (void)controller->handle_agent_event(AgentDelta{
        1,
        CompletionDeltaKind::reasoning,
        "EPHEMERAL_REASONING_ONLY",
    });
    EXPECT_EQ(
        controller->generation_status().reasoning_text,
        "EPHEMERAL_REASONING_ONLY");
    ASSERT_EQ(controller->transcript().entries().size(), 1U);
    (void)controller->request_stop();
    receive_until_idle(*controller);
    EXPECT_TRUE(controller->generation_status().reasoning_text.empty());

    const std::vector<TranscriptEntry> live =
        copy_entries(controller->transcript());
    ASSERT_EQ(live.size(), 1U);
    EXPECT_EQ(live.front().kind, EntryKind::human);

    const std::vector<TranscriptEntry> restored =
        load_transcript_entries(temporary.path);
    EXPECT_EQ(restored, live);
}

TEST(SessionController, RejectsCompletionWithoutResponseContent) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("Question");
    const SessionUpdate update =
        receive_until_idle(*controller);

    ASSERT_TRUE(update.notice);
    EXPECT_EQ(*update.notice, "Generation failed");
    const auto entries = copy_entries(controller->transcript());
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries.back().kind, EntryKind::error);
    EXPECT_EQ(
        entries.back().text,
        "Agent completed without answer content");
    EXPECT_EQ(load_transcript_entries(temporary.path), entries);
}

TEST(SessionController, PersistsPreparationFailureAsTheTurnOutcome) {
    TemporaryJournal temporary;
    auto backend = std::make_unique<ThrowingPrepareBackend>();
    ThrowingPrepareBackend* backend_view = backend.get();
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::move(backend)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("Question");
    const SessionUpdate update = receive_until_idle(*controller);

    EXPECT_EQ(update.notice, "Generation failed");
    EXPECT_FALSE(backend_view->performed);
    const std::vector<TranscriptEntry> entries =
        copy_entries(controller->transcript());
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries.front().kind, EntryKind::human);
    EXPECT_EQ(entries.back().kind, EntryKind::error);
    EXPECT_EQ(entries.back().text, "preparation failed");
    EXPECT_EQ(load_transcript_entries(temporary.path), entries);
}

TEST(SessionController, ReplacesPartialOutputWithATypedError) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>(
            CompletionResult{
                CompletionOutcome::transport_error,
                "network unavailable",
            },
            std::vector<std::string>{"Discard me"})),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("Question");
    receive_until_idle(*controller);

    const auto entries = copy_entries(controller->transcript());
    ASSERT_EQ(entries.size(), 2U);
    const TranscriptEntry& error = entries.back();
    EXPECT_EQ(error.kind, EntryKind::error);
    EXPECT_EQ(error.display_name, "Error");
    EXPECT_EQ(error.participant_id, "guide-id");
    EXPECT_EQ(error.text, "network unavailable");
    EXPECT_EQ(load_transcript_entries(temporary.path), entries);
}

TEST(SessionController, OwnsClearAndInformationSemantics) {
    TemporaryJournal temporary;
    const TranscriptEntry existing =
        make_human_entry(1, "guide-id", "Guide", "Existing", 1);
    {
        SessionJournal journal(temporary.path);
        journal.start_turn(1, existing);
        journal.cancel_turn(1, std::nullopt);
    }
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        temporary.path,
        notifier(),
        restore_with({existing}, 2, 2));

    const SessionUpdate cleared =
        controller->clear_transcript();
    EXPECT_EQ(cleared.notice, "Transcript cleared");
    EXPECT_TRUE(controller->transcript().entries().empty());
    EXPECT_TRUE(load_transcript_entries(temporary.path).empty());

    const SessionUpdate info = controller->session_information();
    ASSERT_TRUE(info.notice);
    EXPECT_NE(
        info.notice->find("Transcript entries: 0"),
        std::string::npos);
    EXPECT_NE(
        info.notice->find("* @Guide  test-model  test://completion  streaming"),
        std::string::npos);
    EXPECT_EQ(info.notice->find("Model:"), std::string::npos);
    EXPECT_TRUE(controller->transcript().entries().empty());
    EXPECT_TRUE(load_transcript_entries(temporary.path).empty());

}

TEST(SessionController, KeepsOffrecordMarkersOutOfTheSessionDatabase) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        temporary.path,
        notifier());

    EXPECT_EQ(controller->extend_offrecord().notice, "No off-record span to extend");
    const SessionUpdate opened = controller->open_offrecord();
    EXPECT_TRUE(opened.render_needed);
    EXPECT_TRUE(opened.clear_input);
    EXPECT_FALSE(opened.notice);
    EXPECT_EQ(controller->open_offrecord().notice,
              "Already off the record; use /hide-off first");
    EXPECT_TRUE(controller->extend_offrecord().render_needed);
    EXPECT_TRUE(controller->restore_offrecord().render_needed);
    EXPECT_EQ(controller->restore_offrecord().notice, "Nothing to restore");

    EXPECT_EQ(
        copy_entries(controller->transcript()),
        (std::vector<TranscriptEntry>{
            make_hide_on_marker(1),
            make_hide_marker(2),
            make_hide_off_marker(3),
        }));
    EXPECT_TRUE(load_transcript_entries(temporary.path).empty());
}

TEST(SessionController, ExcludesAHiddenTurnFromTheNextRequestAndRestoresItLater) {
    TemporaryJournal temporary;
    auto backend = std::make_unique<ScriptedBackend>(
        CompletionResult{}, std::vector<std::string>{"Answer"});
    ScriptedBackend* backend_view = backend.get();
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::move(backend)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("Visible");
    receive_until_idle(*controller);
    (void)controller->open_offrecord();
    (void)controller->submit_prompt("Hidden");
    receive_until_idle(*controller);
    (void)controller->extend_offrecord();
    (void)controller->submit_prompt("Current");
    receive_until_idle(*controller);

    ASSERT_EQ(backend_view->model_contexts.size(), 3U);
    EXPECT_EQ(
        backend_view->model_contexts[2],
        (std::vector<AgentMessage>{
            {AgentRole::user, "Visible"},
            {AgentRole::assistant, "Answer"},
            {AgentRole::user, "Current"},
        }));

    (void)controller->restore_offrecord();
    (void)controller->submit_prompt("Restored");
    receive_until_idle(*controller);

    ASSERT_EQ(backend_view->model_contexts.size(), 4U);
    EXPECT_EQ(
        backend_view->model_contexts[3],
        (std::vector<AgentMessage>{
            {AgentRole::user, "Visible"},
            {AgentRole::assistant, "Answer"},
            {AgentRole::user, "Hidden"},
            {AgentRole::assistant, "Answer"},
            {AgentRole::user, "Current"},
            {AgentRole::assistant, "Answer"},
            {AgentRole::user, "Restored"},
        }));
}

TEST(SessionController, RejectsOffrecordCommandsWhileActiveAndClearResetsTheSpan) {
    TemporaryJournal busy_temporary;
    auto busy_controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>(
            CompletionResult{}, std::vector<std::string>{}, true)),
        busy_temporary.path,
        notifier());
    (void)busy_controller->submit_prompt("Question");

    EXPECT_EQ(busy_controller->open_offrecord().notice, generation_in_progress_notice);
    EXPECT_EQ(busy_controller->extend_offrecord().notice, generation_in_progress_notice);
    EXPECT_EQ(busy_controller->restore_offrecord().notice, generation_in_progress_notice);
    busy_controller->shutdown();

    TemporaryJournal clear_temporary;
    auto clear_controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        clear_temporary.path,
        notifier());
    EXPECT_TRUE(clear_controller->open_offrecord().render_needed);
    EXPECT_EQ(clear_controller->clear_transcript().notice, "Transcript cleared");
    EXPECT_EQ(clear_controller->extend_offrecord().notice, "No off-record span to extend");
    EXPECT_TRUE(clear_controller->transcript().entries().empty());
}

TEST(SessionController, MulticastCommitsTargetsInOrderWithIsolatedContexts) {
    TemporaryJournal temporary;
    auto one = std::make_unique<ScriptedBackend>(
        CompletionResult{}, std::vector<std::string>{"One answer"}, false,
        "one-id", "One");
    auto two = std::make_unique<ScriptedBackend>(
        CompletionResult{}, std::vector<std::string>{"Two answer"}, false,
        "two-id", "Two");
    ScriptedBackend* one_view = one.get();
    ScriptedBackend* two_view = two.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(one));
    backends.push_back(std::move(two));
    auto controller = SessionController::from_backends_for_testing(
        std::move(backends), temporary.path, notifier());

    const SessionUpdate started = controller->start_multicast_by_ids(
        "What time is it?",
        {"one-id", "two-id"});
    EXPECT_TRUE(started.clear_input);
    EXPECT_TRUE(controller->generation_status().active);
    EXPECT_EQ(
        controller->transcript().offrecord_span(),
        OffrecordSpan{});

    const SessionUpdate finished = receive_until_idle(*controller);
    EXPECT_TRUE(finished.render_needed);
    EXPECT_FALSE(finished.end_session);
    EXPECT_FALSE(controller->generation_status().active);
    EXPECT_EQ(controller->transcript().offrecord_span(), OffrecordSpan{});

    ASSERT_EQ(one_view->inputs.size(), 1U);
    ASSERT_EQ(two_view->inputs.size(), 1U);
    EXPECT_EQ(one_view->inputs.front().run.target.id, "one-id");
    EXPECT_EQ(two_view->inputs.front().run.target.id, "two-id");
    EXPECT_EQ(one_view->inputs.front().history, two_view->inputs.front().history);
    EXPECT_EQ(
        one_view->model_contexts.front(),
        (std::vector<AgentMessage>{{AgentRole::user, "What time is it?"}}));
    EXPECT_EQ(
        two_view->model_contexts.front(),
        (std::vector<AgentMessage>{{AgentRole::user, "What time is it?"}}));

    const std::vector<TranscriptEntry> multicast_entries =
        copy_entries(controller->transcript());
    ASSERT_EQ(multicast_entries.size(), 4U);
    EXPECT_EQ(multicast_entries[0].addressed_to, "one-id");
    EXPECT_EQ(multicast_entries[0].text, "What time is it?");
    EXPECT_EQ(multicast_entries[1].text, "One answer");
    EXPECT_EQ(multicast_entries[2].addressed_to, "two-id");
    EXPECT_EQ(multicast_entries[2].text, "What time is it?");
    EXPECT_EQ(multicast_entries[3].text, "Two answer");

    (void)controller->submit_prompt("Follow-up");
    receive_until_idle(*controller);
    ASSERT_EQ(one_view->inputs.size(), 2U);
    EXPECT_EQ(
        one_view->inputs.back().history->entries,
        multicast_entries);
    EXPECT_EQ(
        load_transcript_entries(temporary.path),
        copy_entries(controller->transcript()));
}

TEST(SessionController, ResolvesMulticastIdsAndTreatsAnEmptyListAsAllPersonas) {
    TemporaryJournal temporary;
    auto one = std::make_unique<ScriptedBackend>(
        CompletionResult{}, std::vector<std::string>{"One answer"}, false,
        "one-id", "One");
    auto two = std::make_unique<ScriptedBackend>(
        CompletionResult{}, std::vector<std::string>{"Two answer"}, false,
        "two-id", "Two");
    ScriptedBackend* const one_view = one.get();
    ScriptedBackend* const two_view = two.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(one));
    backends.push_back(std::move(two));
    auto controller = SessionController::from_backends_for_testing(
        std::move(backends), temporary.path, notifier());

    EXPECT_EQ(
        controller->start_multicast_by_ids("Question", {"missing-id"}).notice,
        "Unknown multicast target ID 'missing-id'");
    EXPECT_EQ(
        controller->start_multicast_by_ids("Question", {"one-id", "one-id"}).notice,
        "Multicast target @One is duplicated");
    EXPECT_TRUE(controller->transcript().entries().empty());

    const SessionUpdate started =
        controller->start_multicast_by_ids("Question", {});
    EXPECT_TRUE(started.clear_input);
    receive_until_idle(*controller);
    EXPECT_EQ(one_view->inputs.size(), 1U);
    EXPECT_EQ(two_view->inputs.size(), 1U);
}

TEST(SessionController, MulticastRefusesOffrecordAndStopPreventsNextActivation) {
    TemporaryJournal span_temporary;
    auto span_controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        span_temporary.path,
        notifier());
    (void)span_controller->open_offrecord();
    EXPECT_EQ(
        span_controller->start_multicast_by_ids("Question", {"guide-id"}).notice,
        "Cannot start multicast while an off-record span is active");

    TemporaryJournal stop_temporary;
    auto first = std::make_unique<ScriptedBackend>(
        CompletionResult{}, std::vector<std::string>{}, true, "one-id", "One");
    auto second = std::make_unique<ScriptedBackend>(
        CompletionResult{}, std::vector<std::string>{"Two answer"}, false,
        "two-id", "Two");
    ScriptedBackend* second_view = second.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(first));
    backends.push_back(std::move(second));
    auto stop_controller = SessionController::from_backends_for_testing(
        std::move(backends), stop_temporary.path, notifier());

    (void)stop_controller->start_multicast_by_ids(
        "Question", {"one-id", "two-id"});
    EXPECT_EQ(stop_controller->submit_prompt("Another").notice,
              generation_in_progress_notice);
    EXPECT_EQ(stop_controller->clear_transcript().notice,
              generation_in_progress_notice);
    EXPECT_EQ(stop_controller->open_offrecord().notice,
              generation_in_progress_notice);
    EXPECT_EQ(stop_controller->extend_offrecord().notice,
              generation_in_progress_notice);
    EXPECT_EQ(stop_controller->restore_offrecord().notice,
              generation_in_progress_notice);
    EXPECT_EQ(
        stop_controller->start_multicast_by_ids("Again", {"one-id"}).notice,
        generation_in_progress_notice);
    EXPECT_EQ(stop_controller->request_stop().notice, "Stopping generation...");
    const GenerationStatus stopping =
        stop_controller->generation_status();
    EXPECT_TRUE(stopping.active);
    EXPECT_EQ(stopping.agent_name, "One");
    EXPECT_EQ(stopping.phase, ResponsePhase::stopping);
    const SessionUpdate stopped = receive_until_idle(*stop_controller);
    EXPECT_EQ(stopped.notice, "Generation stopped");
    EXPECT_FALSE(stop_controller->generation_status().active);
    // Cancellation may win before the background worker enters prepare(), or
    // it may race with already-started provider work. Neither case activates
    // the child's durable turn.
    EXPECT_LE(second_view->inputs.size(), 1U);
    const std::vector<TranscriptEntry> entries =
        copy_entries(stop_controller->transcript());
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().addressed_to, "one-id");
    EXPECT_EQ(stop_controller->transcript().offrecord_span(), OffrecordSpan{});
    stop_controller->shutdown();
}

TEST(SessionController, CompletedForegroundWinsTheStopRace) {
    TemporaryJournal temporary;
    auto first = std::make_unique<ConcurrentBackend>(
        "one-id", "One", "One answer");
    auto second = std::make_unique<ConcurrentBackend>(
        "two-id", "Two", "Two answer");
    ConcurrentBackend* const first_view = first.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(first));
    backends.push_back(std::move(second));
    auto controller = SessionController::from_backends_for_testing(
        std::move(backends), temporary.path, notifier());

    (void)controller->start_multicast_by_ids("Question", {"one-id", "two-id"});
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!first_view->finished.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ASSERT_TRUE(first_view->finished.load(std::memory_order_acquire));

    EXPECT_EQ(controller->request_stop().notice, "Stopping generation...");
    const SessionUpdate stopped = receive_until_idle(*controller);

    EXPECT_EQ(stopped.notice, "Generation stopped");
    const std::vector<TranscriptEntry> entries =
        copy_entries(controller->transcript());
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].addressed_to, "one-id");
    EXPECT_EQ(entries[1].text, "One answer");
    EXPECT_EQ(entries[1].status, EntryStatus::complete);
    EXPECT_EQ(load_transcript_entries(temporary.path), entries);
}

TEST(SessionController, StopDoesNotWaitForCancelledBackgroundExecution) {
    TemporaryJournal temporary;
    std::atomic_bool release_background{};
    auto foreground = std::make_unique<ConcurrentBackend>(
        "one-id", "One", "unused");
    auto background = std::make_unique<CancellationBlockingBackend>(
        "two-id", "Two", release_background);
    ConcurrentBackend* foreground_view = foreground.get();
    CancellationBlockingBackend* background_view = background.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(foreground));
    backends.push_back(std::move(background));
    auto controller = SessionController::from_backends_for_testing(
        std::move(backends), temporary.path, notifier());

    (void)controller->start_multicast_by_ids("Question", {"one-id", "two-id"});
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while ((!foreground_view->entered.load(std::memory_order_acquire)
            || !background_view->entered.load(std::memory_order_acquire))
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool both_entered =
        foreground_view->entered.load(std::memory_order_acquire)
        && background_view->entered.load(std::memory_order_acquire);

    const auto started = std::chrono::steady_clock::now();
    const SessionUpdate stopping = controller->request_stop();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    release_background.store(true, std::memory_order_release);

    EXPECT_TRUE(both_entered);
    EXPECT_LT(elapsed, std::chrono::milliseconds(50));
    EXPECT_EQ(stopping.notice, "Stopping generation...");
    receive_until_idle(*controller);
}

TEST(SessionController, MulticastContinuesAfterChildFailuresAndRetainsNotices) {
    TemporaryJournal temporary;
    auto failed = std::make_unique<ScriptedBackend>(
        CompletionResult{CompletionOutcome::transport_error, "Unavailable"},
        std::vector<std::string>{}, false, "one-id", "One");
    auto cancelled = std::make_unique<ScriptedBackend>(
        CompletionResult{CompletionOutcome::cancelled, {}},
        std::vector<std::string>{"Partial"}, false, "two-id", "Two");
    auto complete = std::make_unique<ScriptedBackend>(
        CompletionResult{}, std::vector<std::string>{"Three answer"}, false,
        "three-id", "Three");
    ScriptedBackend* failed_view = failed.get();
    ScriptedBackend* cancelled_view = cancelled.get();
    ScriptedBackend* complete_view = complete.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(failed));
    backends.push_back(std::move(cancelled));
    backends.push_back(std::move(complete));
    auto controller = SessionController::from_backends_for_testing(
        std::move(backends), temporary.path, notifier());

    (void)controller->start_multicast_by_ids(
        "Question", {"one-id", "two-id", "three-id"});
    const SessionUpdate finished = receive_until_idle(*controller);

    EXPECT_EQ(finished.notice, "Generation failed\nGeneration stopped");
    ASSERT_EQ(failed_view->inputs.size(), 1U);
    ASSERT_EQ(cancelled_view->inputs.size(), 1U);
    ASSERT_EQ(complete_view->inputs.size(), 1U);
    EXPECT_EQ(
        complete_view->model_contexts.front(),
        (std::vector<AgentMessage>{{AgentRole::user, "Question"}}));
    EXPECT_FALSE(controller->generation_status().active);
    EXPECT_EQ(controller->transcript().offrecord_span(), OffrecordSpan{});
}

TEST(SessionController, StartsAllChildrenAndBuffersLaterOutputUntilForeground) {
    TemporaryJournal temporary;
    std::atomic_bool release_first{false};
    auto first = std::make_unique<ConcurrentBackend>(
        "one-id", "One", "One answer", &release_first);
    auto second = std::make_unique<ConcurrentBackend>(
        "two-id", "Two", "Two answer");
    ConcurrentBackend* first_view = first.get();
    ConcurrentBackend* second_view = second.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(first));
    backends.push_back(std::move(second));
    auto controller = SessionController::from_backends_for_testing(
        std::move(backends), temporary.path, notifier());

    (void)controller->start_multicast_by_ids("Question", {"one-id", "two-id"});

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while ((!first_view->entered.load(std::memory_order_acquire)
            || !second_view->finished.load(std::memory_order_acquire))
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ASSERT_TRUE(first_view->entered.load(std::memory_order_acquire));
    ASSERT_TRUE(second_view->finished.load(std::memory_order_acquire));

    // Child 2 has already completed, but only child 1's prompt is durable and
    // its queued answer remains hidden until child 1 commits.
    ASSERT_EQ(controller->transcript().entries().size(), 1U);
    EXPECT_EQ(controller->transcript().entries().front().addressed_to, "one-id");

    release_first.store(true, std::memory_order_release);
    receive_until_idle(*controller);

    const std::vector<TranscriptEntry> entries =
        copy_entries(controller->transcript());
    ASSERT_EQ(entries.size(), 4U);
    EXPECT_EQ(entries[0].addressed_to, "one-id");
    EXPECT_EQ(entries[1].text, "One answer");
    EXPECT_EQ(entries[2].addressed_to, "two-id");
    EXPECT_EQ(entries[3].text, "Two answer");
    EXPECT_EQ(load_transcript_entries(temporary.path), entries);
}

TEST(SessionController, DrainsLargeCompletedBackgroundBacklogInOrder) {
    TemporaryJournal temporary;
    constexpr std::size_t backlog_size = 4096;
    std::atomic_bool release_first{false};
    auto first = std::make_unique<ConcurrentBackend>(
        "one-id", "One", "One answer", &release_first);
    auto second = std::make_unique<ConcurrentBackend>(
        "two-id", "Two", "x", nullptr, backlog_size);
    ConcurrentBackend* const second_view = second.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(first));
    backends.push_back(std::move(second));
    auto controller = SessionController::from_backends_for_testing(
        std::move(backends), temporary.path, notifier());

    (void)controller->start_multicast_by_ids("Question", {"one-id", "two-id"});
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!second_view->finished.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ASSERT_TRUE(second_view->finished.load(std::memory_order_acquire));
    ASSERT_EQ(controller->transcript().entries().size(), 1U);

    release_first.store(true, std::memory_order_release);
    receive_until_idle(*controller);

    const std::vector<TranscriptEntry> entries =
        copy_entries(controller->transcript());
    ASSERT_EQ(entries.size(), 4U);
    EXPECT_EQ(entries[1].text, "One answer");
    EXPECT_EQ(entries[2].addressed_to, "two-id");
    EXPECT_EQ(entries[3].text, std::string(backlog_size, 'x'));
    EXPECT_EQ(entries[3].status, EntryStatus::complete);
    EXPECT_EQ(load_transcript_entries(temporary.path), entries);
}

TEST(SessionController, PersistenceFailureIdentifiesTheRequestAndAgent) {
    TemporaryJournal temporary;
    auto backend = std::make_unique<ScriptedBackend>();
    ScriptedBackend* backend_view = backend.get();
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::move(backend)),
        temporary.path,
        notifier());
    sqlite3* raw_blocker = nullptr;
    ASSERT_EQ(
        sqlite3_open_v2(
            utf8_path(temporary.path).c_str(),
            &raw_blocker,
            SQLITE_OPEN_READWRITE,
            nullptr),
        SQLITE_OK);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> blocker(
        raw_blocker, &sqlite3_close_v2);
    ASSERT_EQ(
        sqlite3_exec(
            blocker.get(), "BEGIN EXCLUSIVE", nullptr, nullptr, nullptr),
        SQLITE_OK);

    std::string message;
    try {
        (void)controller->submit_prompt("Question");
    } catch (const std::runtime_error& error) {
        message = error.what();
    }

    EXPECT_EQ(
        sqlite3_exec(blocker.get(), "ROLLBACK", nullptr, nullptr, nullptr),
        SQLITE_OK);
    controller->shutdown();
    ASSERT_FALSE(message.empty());
    EXPECT_NE(
        message.find("Failed to persist start of request 1 for @Guide"),
        std::string::npos)
        << message;
    EXPECT_NE(message.find("Session database"), std::string::npos)
        << message;
    EXPECT_TRUE(backend_view->inputs.empty());
}

TEST(SessionController, FirstActivationFailureTearsDownEveryGatedExecution) {
    TemporaryJournal temporary;
    auto first = std::make_unique<ScriptedBackend>(
        CompletionResult{}, std::vector<std::string>{"One answer"}, false,
        "one-id", "One");
    auto second = std::make_unique<ScriptedBackend>(
        CompletionResult{}, std::vector<std::string>{"Two answer"}, false,
        "two-id", "Two");
    ScriptedBackend* const first_view = first.get();
    ScriptedBackend* const second_view = second.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(first));
    backends.push_back(std::move(second));
    bool fail_first_activation = true;
    auto controller = SessionController::from_backends_for_testing(
        std::move(backends),
        temporary.path,
        notifier(),
        {},
        [&fail_first_activation](std::size_t index) {
            if (index == 0 && fail_first_activation) {
                fail_first_activation = false;
                throw std::runtime_error("injected first activation failure");
            }
        });

    EXPECT_THROW(
        (void)controller->start_multicast_by_ids(
            "Question", {"one-id", "two-id"}),
        std::runtime_error);
    EXPECT_FALSE(controller->generation_status().active);
    EXPECT_TRUE(controller->transcript().entries().empty());
    EXPECT_TRUE(first_view->inputs.empty());
    EXPECT_TRUE(second_view->inputs.empty());

    const SessionUpdate restarted = controller->start_multicast_by_ids(
        "Retry", {"one-id", "two-id"});
    EXPECT_TRUE(restarted.clear_input);
    receive_until_idle(*controller);
    EXPECT_EQ(first_view->inputs.size(), 1U);
    EXPECT_EQ(second_view->inputs.size(), 1U);
}

TEST(SessionController, LaterActivationFailureCancelsAndReleasesEveryExecution) {
    TemporaryJournal temporary;
    auto first = std::make_unique<ScriptedBackend>(
        CompletionResult{}, std::vector<std::string>{"One answer"}, false,
        "one-id", "One");
    auto second = std::make_unique<ScriptedBackend>(
        CompletionResult{}, std::vector<std::string>{"Two answer"}, false,
        "two-id", "Two");
    ScriptedBackend* const first_view = first.get();
    ScriptedBackend* const second_view = second.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(first));
    backends.push_back(std::move(second));
    bool fail_second_activation = true;
    auto controller = SessionController::from_backends_for_testing(
        std::move(backends),
        temporary.path,
        notifier(),
        {},
        [&fail_second_activation](std::size_t index) {
            if (index == 1 && fail_second_activation) {
                fail_second_activation = false;
                throw std::runtime_error("injected later activation failure");
            }
        });

    (void)controller->start_multicast_by_ids("Question", {"one-id", "two-id"});
    EXPECT_THROW(
        (void)receive_until_idle(*controller),
        std::runtime_error);
    EXPECT_FALSE(controller->generation_status().active);
    const std::vector<TranscriptEntry> entries =
        copy_entries(controller->transcript());
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].addressed_to, "one-id");
    EXPECT_EQ(entries[1].text, "One answer");

    const SessionUpdate restarted = controller->start_multicast_by_ids(
        "Retry", {"one-id", "two-id"});
    EXPECT_TRUE(restarted.clear_input);
    receive_until_idle(*controller);
    EXPECT_GE(first_view->inputs.size(), 2U);
    EXPECT_GE(second_view->inputs.size(), 1U);
}

TEST(SessionController, RejectsNewOperationsDuringGeneration) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>(
            CompletionResult{},
            std::vector<std::string>{},
            true)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("Question");
    const SessionUpdate blocked =
        controller->submit_prompt("Another");
    EXPECT_FALSE(blocked.clear_input);
    EXPECT_EQ(
        blocked.notice,
        "Generation in progress; use /stop, Esc, or Ctrl-C");
    const SessionUpdate stopping =
        controller->request_stop();
    EXPECT_FALSE(stopping.clear_input);
    EXPECT_EQ(stopping.notice, "Stopping generation...");
    receive_until_idle(*controller);
}

TEST(SessionController, IgnoresEventsForAnotherRequest) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>(
            CompletionResult{},
            std::vector<std::string>{},
            true)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("Question");
    const SessionUpdate delta =
        controller->handle_agent_event(
            AgentDelta{
                999,
                CompletionDeltaKind::answer,
                "Wrong response",
            });
    const SessionUpdate completed =
        controller->handle_agent_event(
            AgentCompleted{999});

    EXPECT_FALSE(delta.render_needed);
    EXPECT_FALSE(completed.render_needed);
    EXPECT_TRUE(controller->generation_status().active);
    const auto entries = copy_entries(controller->transcript());
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().kind, EntryKind::human);

    (void)controller->request_stop();
    receive_until_idle(*controller);
}

TEST(SessionController, StagingFailureLeavesNoDurableTurn) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        temporary.path,
        notifier());
    controller->shutdown();

    const SessionUpdate update =
        controller->submit_prompt("Question");

    EXPECT_EQ(update.notice, "Request could not be dispatched");
    EXPECT_FALSE(update.clear_input);
    const auto restored =
        load_transcript_entries(temporary.path);
    EXPECT_TRUE(restored.empty());
    EXPECT_TRUE(controller->transcript().entries().empty());
}

TEST(SessionController, FinalizesInterruptedTurnsDuringRestore) {
    TemporaryJournal temporary;
    {
        SessionJournal journal(temporary.path);
        const TranscriptEntry prompt =
            make_human_entry(1, "guide-id", "Guide", "Interrupted", 5);
        journal.start_turn(5, prompt);
    }
    SessionRestore restored =
        load_session_state(temporary.path);
    ASSERT_EQ(restored.interrupted_turns.size(), 1U);

    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        temporary.path,
        notifier(),
        std::move(restored));

    const SessionRestore repaired =
        load_session_state(temporary.path);
    EXPECT_TRUE(repaired.interrupted_turns.empty());
    ASSERT_EQ(repaired.entries.size(), 2U);
    EXPECT_EQ(repaired.entries.back().kind, EntryKind::error);
    EXPECT_NE(
        repaired.entries.back().text.find("interrupted"),
        std::string::npos);
}

TEST(SessionController, RoutesStructuredPromptsAndDefaultChangesAcrossForumPersonas) {
    TemporaryJournal temporary;
    auto guide = std::make_unique<ScriptedBackend>(
        CompletionResult{}, std::vector<std::string>{"Guide answer"});
    auto ismael = std::make_unique<ScriptedBackend>(
        CompletionResult{}, std::vector<std::string>{"Ismael answer"}, false,
        "ismael-id", "Ismael");
    ScriptedBackend* guide_view = guide.get();
    ScriptedBackend* ismael_view = ismael.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(guide));
    backends.push_back(std::move(ismael));
    auto controller = SessionController::from_backends_for_testing(
        std::move(backends),
        temporary.path,
        notifier());

    const SessionUpdate mentioned =
        controller->submit_prompt("hello", "Ism");
    EXPECT_TRUE(mentioned.clear_input);
    receive_until_idle(*controller);
    ASSERT_EQ(ismael_view->inputs.size(), 1U);
    EXPECT_EQ(ismael_view->inputs.front().run.target.id, "ismael-id");
    EXPECT_TRUE(guide_view->inputs.empty());

    const SessionUpdate default_changed =
        controller->set_default_agent("Gui");
    EXPECT_TRUE(default_changed.clear_input);
    EXPECT_EQ(default_changed.notice, "Default agent is now Guide");
    (void)controller->submit_prompt("next");
    receive_until_idle(*controller);
    ASSERT_EQ(guide_view->inputs.size(), 1U);
    EXPECT_EQ(guide_view->inputs.front().run.target.id, "guide-id");

    const std::size_t entries_before_rejection = controller->transcript().entries().size();
    const SessionUpdate rejected =
        controller->submit_prompt("text", "nobody");
    EXPECT_FALSE(rejected.clear_input);
    EXPECT_NE(rejected.notice->find("@nobody"), std::string::npos);
    EXPECT_EQ(controller->transcript().entries().size(), entries_before_rejection);

    const std::vector<TranscriptEntry> entries_before_agents =
        copy_entries(controller->transcript());
    const SessionUpdate agents = controller->agent_information();
    EXPECT_TRUE(agents.clear_input);
    ASSERT_TRUE(agents.notice);
    EXPECT_NE(
        agents.notice->find("Any unambiguous prefix works."),
        std::string::npos);
    EXPECT_EQ(agents.notice->find("Cheburashka"), std::string::npos);
    EXPECT_NE(agents.notice->find("@Ismael"), std::string::npos);
    EXPECT_EQ(
        copy_entries(controller->transcript()),
        entries_before_agents);
    EXPECT_EQ(
        load_transcript_entries(temporary.path),
        entries_before_agents);
}

// Foreign-history addressing is a transcript concern; covered in user_session/transcript tests.

TEST(SessionController, ShutdownCancelsAndPersistsAnActiveTurn) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>(
            CompletionResult{},
            std::vector<std::string>{"Partial"},
            true)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("Question");
    const SessionUpdate partial = receive_when_ready(*controller);
    EXPECT_TRUE(partial.render_needed);
    EXPECT_TRUE(controller->generation_status().active);
    controller->shutdown();

    EXPECT_FALSE(controller->generation_status().active);
    EXPECT_TRUE(controller->receive().end_session);
    const auto entries =
        load_transcript_entries(temporary.path);
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(
        entries.back().status,
        EntryStatus::cancelled);
    EXPECT_EQ(entries.back().text, "Partial");
}

TEST(SessionController, ShutdownJoinsPoolBeforeRegistryCanBeDestroyed) {
    TemporaryJournal temporary;
    FinalWakeBlockingNotifier shutdown_notifier;
    auto backend = std::make_unique<ConcurrentBackend>(
        "guide-id", "Guide", "unused");
    ConcurrentBackend* backend_view = backend.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(backend));
    auto controller = SessionController::from_backends_for_testing(
        std::move(backends), temporary.path, shutdown_notifier);

    (void)controller->submit_prompt("Question");
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!backend_view->entered.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ASSERT_TRUE(backend_view->entered.load(std::memory_order_acquire));

    shutdown_notifier.block_final_wake();
    auto shutdown = std::async(std::launch::async, [&controller] {
        controller->shutdown();
    });

    const bool final_wake_blocked = shutdown_notifier.wait_for_final_wake();
    const std::future_status status = final_wake_blocked
        ? shutdown.wait_for(std::chrono::milliseconds(50))
        : std::future_status::deferred;
    shutdown_notifier.release_final_wake();

    ASSERT_TRUE(final_wake_blocked);
    EXPECT_EQ(status, std::future_status::timeout);
    shutdown.get();
}

} // namespace
} // namespace cha
