#include "agents/completion_batch.h"
#include "agents/completion_executor.h"
#include "support/test_backends.h"
#include "support/test_completions.h"
#include "support/test_notifier.h"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace cha {
namespace {

using namespace std::chrono_literals;
using test::completion_request;
using test::RecordingBackend;

test::TestNotifier& notifier() {
    static test::TestNotifier instance;
    return instance;
}

AgentEvent next_foreground_event(CompletionBatch& batch) {
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline) {
        AgentEvent event = AgentCompleted{};
        if (batch.try_receive_foreground(event) == ChannelReadStatus::value) {
            return event;
        }
        std::this_thread::yield();
    }
    throw std::runtime_error("Timed out waiting for agent event");
}

// Stages, opens, and drains one single-target batch through the executor.
void expect_one_completed_run(
    CompletionExecutor& executor,
    const Transcript& transcript,
    RequestId id,
    const std::string& target,
    const std::string& name) {
    CompletionBatch batch =
        executor.stage_batch({completion_request(transcript, id, target, name)});
    batch.open();
    EXPECT_TRUE(
        std::holds_alternative<AgentCompleted>(next_foreground_event(batch)));
}

TEST(CompletionExecutor, RejectsBorrowedPoolWhoseWidthDiffersFromBackendCount) {
    ThreadPool pool(1);
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::make_unique<RecordingBackend>(
        "one-id", "One", "one"));
    backends.push_back(std::make_unique<RecordingBackend>(
        "two-id", "Two", "two"));

    EXPECT_THROW(
        (void)CompletionExecutor(std::move(backends), notifier(), pool),
        std::invalid_argument);
}

TEST(CompletionExecutor, RejectsEmptyAndNullBackendConstruction) {
    ThreadPool pool(1);
    EXPECT_THROW(
        (void)CompletionExecutor(
            std::vector<std::unique_ptr<CompletionBackend>>{}, notifier(), pool),
        std::invalid_argument);

    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(nullptr);
    EXPECT_THROW(
        (void)CompletionExecutor(std::move(backends), notifier(), pool),
        std::invalid_argument);
}

TEST(CompletionExecutor, RejectsInvalidBackendMetadataAtConstruction) {
    ThreadPool pool(1);
    EXPECT_THROW(
        (void)CompletionExecutor(
            test::one_backend(std::make_unique<RecordingBackend>(
                "valid-id", " Invalid name", "answer")),
            notifier(),
            pool),
        std::invalid_argument);
}

TEST(CompletionExecutor, RejectsDuplicateBackendMetadataAtConstruction) {
    ThreadPool pool(2);
    std::vector<std::unique_ptr<CompletionBackend>> duplicate_ids;
    duplicate_ids.push_back(
        std::make_unique<RecordingBackend>("one-id", "One", ""));
    duplicate_ids.push_back(
        std::make_unique<RecordingBackend>("one-id", "Two", ""));
    EXPECT_THROW(
        (void)CompletionExecutor(std::move(duplicate_ids), notifier(), pool),
        std::invalid_argument);

    std::vector<std::unique_ptr<CompletionBackend>> duplicate_names;
    duplicate_names.push_back(
        std::make_unique<RecordingBackend>("one-id", "One", ""));
    duplicate_names.push_back(
        std::make_unique<RecordingBackend>("two-id", "ONE", ""));
    EXPECT_THROW(
        (void)CompletionExecutor(std::move(duplicate_names), notifier(), pool),
        std::invalid_argument);
}

TEST(CompletionExecutor, ExposesPublicRuntimeInfoInBackendOrder) {
    ThreadPool pool(2);
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::make_unique<RecordingBackend>("one-id", "One", ""));
    backends.push_back(std::make_unique<RecordingBackend>("two-id", "Two", ""));
    CompletionExecutor executor(std::move(backends), notifier(), pool);

    const std::vector<AgentRuntimeInfo>& info = executor.runtime_info();
    ASSERT_EQ(info.size(), 2U);
    EXPECT_EQ(info[0].character.id, "one-id");
    EXPECT_EQ(info[1].character.id, "two-id");
    EXPECT_EQ(info[0].api, "test://completion");
    pool.stop();
}

TEST(CompletionExecutor, IdentifiesCharacterWhoseDefinitionStartupFails) {
    ThreadPool pool(1);
    AgentDefinition definition{
        .config = {
            .id = "alpha-id",
            .display_name = "Alpha",
            .api_key_env = "__CHA_TEST_MISSING_AGENT_KEY__",
        },
        .system_prompt = "Prompt",
    };

    try {
        (void)CompletionExecutor(
            std::vector<AgentDefinition>{std::move(definition)},
            notifier(),
            pool);
        FAIL() << "Expected startup failure";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("Character 'Alpha'"), std::string::npos);
        EXPECT_EQ(message.find("alpha-id"), std::string::npos);
    }
}

TEST(CompletionExecutor, RejectsEmptyBatchAndMissingHistory) {
    Transcript transcript;
    ThreadPool pool(1);
    CompletionExecutor executor(
        test::one_backend(std::make_unique<RecordingBackend>(
            "one-id", "One", "")),
        notifier(),
        pool);

    EXPECT_THROW(
        (void)executor.stage_batch({}),
        std::invalid_argument);

    CompletionInput invalid = completion_request(transcript, 1, "one-id", "One");
    invalid.history.reset();
    EXPECT_THROW(
        (void)executor.stage_batch({std::move(invalid)}),
        std::invalid_argument);

    expect_one_completed_run(executor, transcript, 2, "one-id", "One");
    pool.stop();
}

TEST(CompletionExecutor, RejectsUnknownAndDuplicateTargets) {
    Transcript transcript;
    ThreadPool pool(1);
    CompletionExecutor executor(
        test::one_backend(std::make_unique<RecordingBackend>(
            "one-id", "One", "")),
        notifier(),
        pool);

    EXPECT_THROW(
        (void)executor.stage_batch(
            {completion_request(transcript, 1, "missing-id", "Missing")}),
        std::invalid_argument);
    EXPECT_THROW(
        (void)executor.stage_batch({
            completion_request(transcript, 2, "one-id", "One"),
            completion_request(transcript, 3, "one-id", "One"),
        }),
        std::invalid_argument);

    expect_one_completed_run(executor, transcript, 4, "one-id", "One");
    pool.stop();
}

TEST(CompletionExecutor, RejectsStagingIntoAStoppedPoolWithoutCallingABackend) {
    Transcript transcript;
    ThreadPool pool(1);
    auto backend = std::make_unique<RecordingBackend>("one-id", "One", "");
    RecordingBackend* view = backend.get();
    CompletionExecutor executor(
        test::one_backend(std::move(backend)), notifier(), pool);
    pool.stop();

    EXPECT_THROW(
        (void)executor.stage_batch(
            {completion_request(transcript, 1, "one-id", "One")}),
        std::runtime_error);
    EXPECT_FALSE(view->prepared.load(std::memory_order_acquire));
    EXPECT_FALSE(view->performed.load(std::memory_order_acquire));
    // The failed staging left no executor state that would change how the next
    // attempt fails.
    EXPECT_THROW(
        (void)executor.stage_batch(
            {completion_request(transcript, 2, "one-id", "One")}),
        std::runtime_error);
}

TEST(CompletionExecutor, RollsBackPartialSubmissionAndFreesEveryAcceptedWorker) {
    Transcript transcript;
    test::BarrierState state;
    ThreadPool pool(2);
    auto one = std::make_unique<test::BarrierBackend>("one-id", "One", state);
    auto two = std::make_unique<test::BarrierBackend>("two-id", "Two", state);
    test::BarrierBackend* one_view = one.get();
    test::BarrierBackend* two_view = two.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(one));
    backends.push_back(std::move(two));
    bool inject_failure = true;
    CompletionExecutor executor(
        std::move(backends),
        notifier(),
        pool,
        [&inject_failure](std::size_t submission_index) {
            if (submission_index == 1 && inject_failure) {
                inject_failure = false;
                throw std::runtime_error("injected submission failure");
            }
        });

    EXPECT_THROW(
        (void)executor.stage_batch({
            completion_request(transcript, 1, "one-id", "One"),
            completion_request(transcript, 2, "two-id", "Two"),
        }),
        std::runtime_error);
    EXPECT_FALSE(one_view->prepared.load(std::memory_order_acquire));
    EXPECT_FALSE(two_view->prepared.load(std::memory_order_acquire));

    // Both workers can be inside perform() at once again, which is possible
    // only because the one accepted gated task finished during the rollback.
    CompletionBatch batch = executor.stage_batch({
        completion_request(transcript, 3, "one-id", "One"),
        completion_request(transcript, 4, "two-id", "Two"),
    });
    batch.open();
    const bool both_entered = test::wait_until_entered(state, 2);
    state.release.store(true, std::memory_order_release);
    EXPECT_TRUE(both_entered);
    EXPECT_TRUE(
        std::holds_alternative<AgentCompleted>(next_foreground_event(batch)));
    batch.advance_foreground();
    EXPECT_TRUE(
        std::holds_alternative<AgentCompleted>(next_foreground_event(batch)));
}

} // namespace
} // namespace cha
