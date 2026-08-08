#include "agents/completion_batch.h"
#include "agents/completion_executor.h"
#include "support/test_backends.h"
#include "support/test_completions.h"
#include "support/test_notifier.h"
#include "support/test_transcript.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace cha {
namespace {

using namespace std::chrono_literals;
using test::BarrierBackend;
using test::BarrierState;
using test::completion_request;
using test::RecordingBackend;
using test::ThrowingBackend;
using test::wait_until_entered;

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

// Every test below declares the pool, then the executor, then the batch, which
// is the controller's declaration order. Reverse destruction therefore cancels
// and waits for the batch before the executor's backends and the pool's workers
// go away, so no test needs to stop the pool by hand.
std::vector<std::unique_ptr<CompletionBackend>> two_backends(
    std::unique_ptr<CompletionBackend> first,
    std::unique_ptr<CompletionBackend> second) {
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(first));
    backends.push_back(std::move(second));
    return backends;
}

TEST(CompletionBatch, ClosedGateHoldsEveryExecutionUntilOpened) {
    Transcript transcript;
    ThreadPool pool(1);
    auto backend = std::make_unique<RecordingBackend>("one-id", "One", "one");
    RecordingBackend* const view = backend.get();
    CompletionExecutor executor(
        test::one_backend(std::move(backend)), notifier(), pool);

    CompletionBatch batch = executor.stage_batch(
        {completion_request(transcript, 1, "one-id", "One")});

    // Staging accepted the task, but nothing may reach a backend until the
    // controller has durably activated the foreground run.
    AgentEvent event = AgentCompleted{};
    EXPECT_EQ(batch.try_receive_foreground(event), ChannelReadStatus::empty);
    EXPECT_FALSE(view->prepared.load(std::memory_order_acquire));
    EXPECT_FALSE(view->performed.load(std::memory_order_acquire));
    EXPECT_FALSE(batch.cancellation_requested());

    batch.open();
    EXPECT_TRUE(std::holds_alternative<AgentDelta>(next_foreground_event(batch)));
    EXPECT_TRUE(
        std::holds_alternative<AgentCompleted>(next_foreground_event(batch)));
    EXPECT_TRUE(view->prepared.load(std::memory_order_acquire));
}

TEST(CompletionBatch, CancellationBeforeOpeningSkipsPreparationAndPerformance) {
    Transcript transcript;
    ThreadPool pool(1);
    auto backend = std::make_unique<RecordingBackend>("one-id", "One", "one");
    RecordingBackend* const view = backend.get();
    CompletionExecutor executor(
        test::one_backend(std::move(backend)), notifier(), pool);

    CompletionBatch batch = executor.stage_batch(
        {completion_request(transcript, 1, "one-id", "One")});
    batch.cancel();
    EXPECT_TRUE(batch.cancellation_requested());

    EXPECT_EQ(
        std::get<AgentCancelled>(next_foreground_event(batch)).request_id, 1U);
    EXPECT_FALSE(view->prepared.load(std::memory_order_acquire));
    EXPECT_FALSE(view->performed.load(std::memory_order_acquire));
}

TEST(CompletionBatch, CancellationAfterOpeningStillSkipsAQueuedExecution) {
    Transcript transcript;
    ThreadPool pool(1);
    std::promise<void> blocker_entered;
    std::future<void> blocker_ready = blocker_entered.get_future();
    std::atomic_bool release_blocker{};
    ASSERT_TRUE(pool.submit([&] {
        blocker_entered.set_value();
        while (!release_blocker.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }));
    ASSERT_EQ(blocker_ready.wait_for(1s), std::future_status::ready);

    auto backend = std::make_unique<RecordingBackend>("one-id", "One", "");
    RecordingBackend* const view = backend.get();
    CompletionExecutor executor(
        test::one_backend(std::move(backend)), notifier(), pool);
    CompletionBatch batch = executor.stage_batch(
        {completion_request(transcript, 14, "one-id", "One")});
    batch.open();
    batch.cancel();
    release_blocker.store(true, std::memory_order_release);

    EXPECT_EQ(
        std::get<AgentCancelled>(next_foreground_event(batch)).request_id, 14U);
    EXPECT_FALSE(view->prepared.load(std::memory_order_acquire));
    EXPECT_FALSE(view->performed.load(std::memory_order_acquire));
}

TEST(CompletionBatch, OpeningStartsEverySelectedBackendAtFullPoolWidth) {
    Transcript transcript;
    BarrierState state;
    ThreadPool pool(3);
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::make_unique<BarrierBackend>("one-id", "One", state));
    backends.push_back(std::make_unique<BarrierBackend>("two-id", "Two", state));
    backends.push_back(
        std::make_unique<BarrierBackend>("three-id", "Three", state));
    CompletionExecutor executor(std::move(backends), notifier(), pool);

    CompletionBatch batch = executor.stage_batch({
        completion_request(transcript, 1, "one-id", "One"),
        completion_request(transcript, 2, "two-id", "Two"),
        completion_request(transcript, 3, "three-id", "Three"),
    });
    batch.open();
    const bool all_entered = wait_until_entered(state, 3);
    state.release.store(true, std::memory_order_release);
    EXPECT_TRUE(all_entered);

    for (std::size_t index = 0; index < 3; ++index) {
        EXPECT_TRUE(std::holds_alternative<AgentCompleted>(
            next_foreground_event(batch)));
        if (batch.has_next_foreground()) {
            batch.advance_foreground();
        }
    }
    batch.wait_until_finished();
    EXPECT_TRUE(batch.executions_finished());
}

TEST(CompletionBatch, ForegroundRunAndEventsComeFromTheSameSlot) {
    Transcript transcript;
    ThreadPool pool(2);
    auto one = std::make_unique<RecordingBackend>("one-id", "One", "first");
    auto two = std::make_unique<RecordingBackend>("two-id", "Two", "second");
    RecordingBackend* const one_view = one.get();
    RecordingBackend* const two_view = two.get();
    CompletionExecutor executor(
        two_backends(std::move(one), std::move(two)), notifier(), pool);

    CompletionBatch batch = executor.stage_batch({
        completion_request(transcript, 1, "one-id", "One", "One question"),
        completion_request(transcript, 2, "two-id", "Two", "Two question"),
    });
    EXPECT_EQ(batch.foreground_index(), 0U);
    EXPECT_EQ(batch.foreground_run().request_id, 1U);
    EXPECT_EQ(batch.foreground_run().target.id, "one-id");
    EXPECT_TRUE(batch.has_next_foreground());
    batch.open();

    EXPECT_EQ(std::get<AgentDelta>(next_foreground_event(batch)).text, "first");
    EXPECT_EQ(
        std::get<AgentCompleted>(next_foreground_event(batch)).request_id, 1U);

    batch.advance_foreground();
    EXPECT_EQ(batch.foreground_index(), 1U);
    EXPECT_EQ(batch.foreground_run().request_id, 2U);
    EXPECT_EQ(batch.foreground_run().target.id, "two-id");
    EXPECT_FALSE(batch.has_next_foreground());
    EXPECT_EQ(std::get<AgentDelta>(next_foreground_event(batch)).text, "second");
    EXPECT_EQ(
        std::get<AgentCompleted>(next_foreground_event(batch)).request_id, 2U);

    EXPECT_EQ(one_view->received.run.prompt_text, "One question");
    EXPECT_EQ(two_view->received.run.prompt_text, "Two question");
}

TEST(CompletionBatch, LaterSlotsBufferEventsUntilForegroundAdvancement) {
    Transcript transcript;
    BarrierState first_state;
    ThreadPool pool(2);
    auto second = std::make_unique<RecordingBackend>("two-id", "Two", "second");
    RecordingBackend* const second_view = second.get();
    CompletionExecutor executor(
        two_backends(
            std::make_unique<BarrierBackend>("one-id", "One", first_state),
            std::move(second)),
        notifier(),
        pool);

    CompletionBatch batch = executor.stage_batch({
        completion_request(transcript, 1, "one-id", "One"),
        completion_request(transcript, 2, "two-id", "Two"),
    });
    batch.open();
    ASSERT_TRUE(wait_until_entered(first_state, 1));
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (!second_view->performed.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ASSERT_TRUE(second_view->performed.load(std::memory_order_acquire));

    // The second slot has already produced its whole output while the first is
    // still running; none of it is reachable through the foreground channel.
    AgentEvent event = AgentCompleted{};
    EXPECT_EQ(batch.try_receive_foreground(event), ChannelReadStatus::empty);
    EXPECT_THROW(batch.advance_foreground(), std::logic_error);

    first_state.release.store(true, std::memory_order_release);
    EXPECT_TRUE(
        std::holds_alternative<AgentCompleted>(next_foreground_event(batch)));
    batch.advance_foreground();
    EXPECT_EQ(std::get<AgentDelta>(next_foreground_event(batch)).text, "second");
    EXPECT_TRUE(
        std::holds_alternative<AgentCompleted>(next_foreground_event(batch)));
}

TEST(CompletionBatch, AdvancementRequiresAConsumedTerminalAndANextSlot) {
    Transcript transcript;
    ThreadPool pool(2);
    CompletionExecutor executor(
        two_backends(
            std::make_unique<RecordingBackend>("one-id", "One", "first"),
            std::make_unique<RecordingBackend>("two-id", "Two", "second")),
        notifier(),
        pool);

    CompletionBatch batch = executor.stage_batch({
        completion_request(transcript, 1, "one-id", "One"),
        completion_request(transcript, 2, "two-id", "Two"),
    });
    batch.open();

    EXPECT_THROW(batch.advance_foreground(), std::logic_error);
    EXPECT_EQ(std::get<AgentDelta>(next_foreground_event(batch)).text, "first");
    // A delta is not a terminal event.
    EXPECT_THROW(batch.advance_foreground(), std::logic_error);
    EXPECT_TRUE(
        std::holds_alternative<AgentCompleted>(next_foreground_event(batch)));

    batch.advance_foreground();
    EXPECT_TRUE(std::holds_alternative<AgentDelta>(next_foreground_event(batch)));
    EXPECT_TRUE(
        std::holds_alternative<AgentCompleted>(next_foreground_event(batch)));
    // The last slot's terminal was consumed, but there is nowhere to advance.
    EXPECT_THROW(batch.advance_foreground(), std::logic_error);
}

TEST(CompletionBatch, PreservesCapturedHistoryForEveryExecution) {
    Transcript transcript;
    transcript.add_entry(test::human_entry(
        1, {"human", "You"}, {"one-id", "One"}, "Earlier question", 1));
    ThreadPool pool(2);
    auto one = std::make_unique<RecordingBackend>("one-id", "One", "");
    auto two = std::make_unique<RecordingBackend>("two-id", "Two", "");
    RecordingBackend* const one_view = one.get();
    RecordingBackend* const two_view = two.get();
    CompletionExecutor executor(
        two_backends(std::move(one), std::move(two)), notifier(), pool);

    CompletionBatch batch = executor.stage_batch({
        completion_request(transcript, 1, "one-id", "One"),
        completion_request(transcript, 2, "two-id", "Two"),
    });
    transcript.clear();
    batch.open();

    EXPECT_TRUE(
        std::holds_alternative<AgentCompleted>(next_foreground_event(batch)));
    batch.advance_foreground();
    EXPECT_TRUE(
        std::holds_alternative<AgentCompleted>(next_foreground_event(batch)));
    ASSERT_EQ(one_view->received.history->entries.size(), 1U);
    ASSERT_EQ(two_view->received.history->entries.size(), 1U);
    EXPECT_EQ(
        one_view->received.history->entries.front().text, "Earlier question");
    EXPECT_EQ(
        two_view->received.history->entries.front().text, "Earlier question");
}

TEST(CompletionBatch, MapsCompletionFailureAndExceptionsToAgentFailed) {
    Transcript transcript;
    ThreadPool failure_pool(1);
    CompletionExecutor failure_executor(
        test::one_backend(std::make_unique<RecordingBackend>(
            "one-id",
            "One",
            "",
            CompletionResult{
                CompletionOutcome::protocol_error, "malformed response"})),
        notifier(),
        failure_pool);
    CompletionBatch failure_batch = failure_executor.stage_batch(
        {completion_request(transcript, 12, "one-id", "One")});
    failure_batch.open();
    const AgentFailed failed =
        std::get<AgentFailed>(next_foreground_event(failure_batch));
    EXPECT_EQ(failed.request_id, 12U);
    EXPECT_EQ(failed.message, "malformed response");

    ThreadPool throwing_pool(1);
    CompletionExecutor throwing_executor(
        test::one_backend(std::make_unique<ThrowingBackend>()),
        notifier(),
        throwing_pool);
    CompletionBatch throwing_batch = throwing_executor.stage_batch(
        {completion_request(transcript, 1, "one-id", "One")});
    throwing_batch.open();
    const AgentFailed thrown =
        std::get<AgentFailed>(next_foreground_event(throwing_batch));
    EXPECT_EQ(thrown.request_id, 1U);
    EXPECT_NE(thrown.message.find("preparation failed"), std::string::npos);
}

TEST(CompletionBatch, DeliversExactlyOneTerminalThenReportsClosed) {
    Transcript transcript;
    ThreadPool pool(1);
    CompletionExecutor executor(
        test::one_backend(std::make_unique<RecordingBackend>(
            "one-id", "One", "delta")),
        notifier(),
        pool);

    CompletionBatch batch = executor.stage_batch(
        {completion_request(transcript, 1, "one-id", "One")});
    batch.open();

    EXPECT_TRUE(std::holds_alternative<AgentDelta>(next_foreground_event(batch)));
    EXPECT_TRUE(
        std::holds_alternative<AgentCompleted>(next_foreground_event(batch)));
    AgentEvent event = AgentCompleted{};
    EXPECT_EQ(batch.try_receive_foreground(event), ChannelReadStatus::closed);
    EXPECT_EQ(batch.try_receive_foreground(event), ChannelReadStatus::closed);
}

TEST(CompletionBatch, CancellationReachesEveryOpenedExecution) {
    Transcript transcript;
    BarrierState state;
    ThreadPool pool(2);
    CompletionExecutor executor(
        two_backends(
            std::make_unique<BarrierBackend>("one-id", "One", state),
            std::make_unique<BarrierBackend>("two-id", "Two", state)),
        notifier(),
        pool);

    CompletionBatch batch = executor.stage_batch({
        completion_request(transcript, 1, "one-id", "One"),
        completion_request(transcript, 2, "two-id", "Two"),
    });
    batch.open();
    ASSERT_TRUE(wait_until_entered(state, 2));
    batch.cancel();
    // Idempotent: a second request changes nothing.
    batch.cancel();

    EXPECT_TRUE(
        std::holds_alternative<AgentCancelled>(next_foreground_event(batch)));
    batch.advance_foreground();
    EXPECT_TRUE(
        std::holds_alternative<AgentCancelled>(next_foreground_event(batch)));
}

TEST(CompletionBatch, ExplicitWaitingLeavesTerminalEventsDrainable) {
    Transcript transcript;
    BarrierState state;
    ThreadPool pool(1);
    CompletionExecutor executor(
        test::one_backend(std::make_unique<BarrierBackend>(
            "one-id", "One", state)),
        notifier(),
        pool);

    CompletionBatch batch = executor.stage_batch(
        {completion_request(transcript, 1, "one-id", "One")});
    batch.open();
    ASSERT_TRUE(wait_until_entered(state, 1));
    batch.cancel();
    batch.wait_until_finished();

    EXPECT_TRUE(batch.executions_finished());
    AgentEvent event = AgentCompleted{};
    ASSERT_EQ(batch.try_receive_foreground(event), ChannelReadStatus::value);
    EXPECT_TRUE(std::holds_alternative<AgentCancelled>(event));
    EXPECT_EQ(batch.try_receive_foreground(event), ChannelReadStatus::closed);
}

TEST(CompletionBatch, DestructionCancelsAndWaitsForEveryExecution) {
    Transcript transcript;
    BarrierState state;
    ThreadPool pool(2);
    CompletionExecutor executor(
        two_backends(
            std::make_unique<BarrierBackend>("one-id", "One", state),
            std::make_unique<BarrierBackend>("two-id", "Two", state)),
        notifier(),
        pool);

    {
        CompletionBatch batch = executor.stage_batch({
            completion_request(transcript, 1, "one-id", "One"),
            completion_request(transcript, 2, "two-id", "Two"),
        });
        batch.open();
        ASSERT_TRUE(wait_until_entered(state, 2));
        // Leaving the scope must cancel both blocked executions and return
        // only once neither can touch a backend again.
    }

    // Both workers are free, so a later batch through the same executor runs at
    // full width again.
    state.entered.store(0, std::memory_order_release);
    state.release.store(false, std::memory_order_release);
    CompletionBatch later = executor.stage_batch({
        completion_request(transcript, 3, "one-id", "One"),
        completion_request(transcript, 4, "two-id", "Two"),
    });
    later.open();
    const bool both_entered = wait_until_entered(state, 2);
    state.release.store(true, std::memory_order_release);
    EXPECT_TRUE(both_entered);
    EXPECT_TRUE(
        std::holds_alternative<AgentCompleted>(next_foreground_event(later)));
}

TEST(CompletionBatch, CompletedAndCancelledBatchesPermitLaterBatches) {
    Transcript transcript;
    BarrierState state;
    ThreadPool pool(1);
    CompletionExecutor executor(
        test::one_backend(std::make_unique<BarrierBackend>(
            "one-id", "One", state)),
        notifier(),
        pool);

    {
        CompletionBatch completed = executor.stage_batch(
            {completion_request(transcript, 1, "one-id", "One")});
        completed.open();
        ASSERT_TRUE(wait_until_entered(state, 1));
        state.release.store(true, std::memory_order_release);
        EXPECT_TRUE(std::holds_alternative<AgentCompleted>(
            next_foreground_event(completed)));
    }

    state.release.store(false, std::memory_order_release);
    {
        CompletionBatch cancelled = executor.stage_batch(
            {completion_request(transcript, 2, "one-id", "One")});
        cancelled.open();
        ASSERT_TRUE(wait_until_entered(state, 2));
        cancelled.cancel();
        EXPECT_TRUE(std::holds_alternative<AgentCancelled>(
            next_foreground_event(cancelled)));
    }

    state.release.store(true, std::memory_order_release);
    CompletionBatch third = executor.stage_batch(
        {completion_request(transcript, 3, "one-id", "One")});
    third.open();
    EXPECT_TRUE(
        std::holds_alternative<AgentCompleted>(next_foreground_event(third)));
}

TEST(CompletionBatch, MoveTransfersOwnershipWithoutDisturbingExecutions) {
    Transcript transcript;
    ThreadPool pool(1);
    CompletionExecutor executor(
        test::one_backend(std::make_unique<RecordingBackend>(
            "one-id", "One", "answer")),
        notifier(),
        pool);

    std::optional<CompletionBatch> owner;
    owner.emplace(executor.stage_batch(
        {completion_request(transcript, 7, "one-id", "One")}));
    EXPECT_EQ(owner->foreground_run().request_id, 7U);
    owner->open();

    EXPECT_EQ(std::get<AgentDelta>(next_foreground_event(*owner)).text, "answer");
    EXPECT_TRUE(
        std::holds_alternative<AgentCompleted>(next_foreground_event(*owner)));
    owner.reset();
}

} // namespace
} // namespace cha
