#include "agents/agent_registry.h"
#include "support/test_backends.h"
#include "support/test_notifier.h"
#include "support/test_transcript.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
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

test::TestNotifier& notifier() {
    static test::TestNotifier instance;
    return instance;
}

class RecordingBackend final : public CompletionBackend {
public:
    RecordingBackend(
        std::string id,
        std::string name,
        std::string answer,
        CompletionResult result = {})
        : id_(std::move(id)),
          name_(std::move(name)),
          answer_(std::move(answer)),
          result_(std::move(result)) {
    }

    RequestPayload prepare(const CompletionInput& input) override {
        prepared.store(true, std::memory_order_release);
        received = input;
        return {.bytes = input.run.prompt_text};
    }

    CompletionResult perform(
        RequestPayload,
        const CompletionDeltaSink& on_delta,
        const std::atomic_bool&) override {
        performed.store(true, std::memory_order_release);
        if (!answer_.empty()) {
            on_delta({CompletionDeltaKind::answer, answer_});
        }
        return result_;
    }

    AgentRuntimeInfo info() const override {
        return {{id_, name_}, "model", "test://completion", true};
    }

    std::atomic_bool prepared{};
    std::atomic_bool performed{};
    CompletionInput received;

private:
    std::string id_;
    std::string name_;
    std::string answer_;
    CompletionResult result_;
};

struct BarrierState {
    std::atomic_int entered{};
    std::atomic_bool release{};
};

class BarrierBackend final : public CompletionBackend {
public:
    BarrierBackend(std::string id, std::string name, BarrierState& state)
        : id_(std::move(id)), name_(std::move(name)), state_(state) {
    }

    RequestPayload prepare(const CompletionInput& input) override {
        prepared.store(true, std::memory_order_release);
        return {.bytes = input.run.prompt_text};
    }

    CompletionResult perform(
        RequestPayload,
        const CompletionDeltaSink&,
        const std::atomic_bool& cancellation) override {
        state_.entered.fetch_add(1, std::memory_order_acq_rel);
        while (!state_.release.load(std::memory_order_acquire)
               && !cancellation.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return cancellation.load(std::memory_order_acquire)
            ? CompletionResult{CompletionOutcome::cancelled, {}}
            : CompletionResult{};
    }

    AgentRuntimeInfo info() const override {
        return {{id_, name_}, "model", "test://completion", true};
    }

    std::atomic_bool prepared{};

private:
    std::string id_;
    std::string name_;
    BarrierState& state_;
};

class ThrowingBackend final : public CompletionBackend {
public:
    RequestPayload prepare(const CompletionInput&) override {
        throw std::runtime_error("preparation failed");
    }

    CompletionResult perform(
        RequestPayload,
        const CompletionDeltaSink&,
        const std::atomic_bool&) override {
        return {};
    }

    AgentRuntimeInfo info() const override {
        return {{"one-id", "One"}, "model", "test://completion", true};
    }
};

CompletionInput request(
    const Transcript& transcript,
    RequestId id,
    std::string target,
    std::string name,
    std::string text = "Question") {
    return {
        .history = std::make_shared<const CompletionHistory>(
            transcript.completion_history()),
        .run = {
            .request_id = id,
            .target = {std::move(target), std::move(name)},
            .prompt_text = std::move(text),
        },
    };
}

AgentEvent next_event(AgentRegistry& registry, std::size_t index) {
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline) {
        AgentEvent event = AgentCompleted{};
        if (registry.try_receive(index, event) == ChannelReadStatus::value) {
            return event;
        }
        std::this_thread::yield();
    }
    throw std::runtime_error("Timed out waiting for agent event");
}

bool wait_until_entered(const BarrierState& state, int expected) {
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (state.entered.load(std::memory_order_acquire) < expected
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return state.entered.load(std::memory_order_acquire) == expected;
}

TEST(AgentRegistry, RejectsBorrowedPoolWhoseWidthDiffersFromBackendCount) {
    ThreadPool pool(1);
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::make_unique<RecordingBackend>(
        "one-id", "One", "one"));
    backends.push_back(std::make_unique<RecordingBackend>(
        "two-id", "Two", "two"));

    EXPECT_THROW(
        (void)AgentRegistry(std::move(backends), notifier(), pool),
        std::invalid_argument);
}

TEST(AgentRegistry, RejectsEmptyAndNullBackendConstruction) {
    ThreadPool pool(1);
    EXPECT_THROW(
        (void)AgentRegistry(
            std::vector<std::unique_ptr<CompletionBackend>>{}, notifier(), pool),
        std::invalid_argument);

    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(nullptr);
    EXPECT_THROW(
        (void)AgentRegistry(std::move(backends), notifier(), pool),
        std::invalid_argument);
}

TEST(AgentRegistry, RejectsInvalidBackendMetadataAtConstruction) {
    ThreadPool pool(1);
    EXPECT_THROW(
        (void)AgentRegistry(
            test::one_backend(std::make_unique<RecordingBackend>(
                "valid-id", " Invalid name", "answer")),
            notifier(),
            pool),
        std::invalid_argument);
}

TEST(AgentRegistry, IdentifiesCharacterWhoseDefinitionStartupFails) {
    ThreadPool pool(1);
    AgentDefinition definition{
        .config = {
            .id = "alpha-id",
            .name = "Alpha",
            .api_key_env = "__CHA_TEST_MISSING_AGENT_KEY__",
        },
        .system_prompt = "Prompt",
    };

    try {
        (void)AgentRegistry(
            std::vector<AgentDefinition>{std::move(definition)}, notifier(), pool);
        FAIL() << "Expected startup failure";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("Character 'Alpha'"), std::string::npos);
        EXPECT_NE(message.find("alpha-id"), std::string::npos);
    }
}

TEST(AgentRegistry, RejectsMissingHistoryWithoutLeavingABatch) {
    Transcript transcript;
    ThreadPool pool(1);
    AgentRegistry registry(
        test::one_backend(std::make_unique<RecordingBackend>(
            "one-id", "One", "")),
        notifier(),
        pool);
    CompletionInput invalid = request(transcript, 1, "one-id", "One");
    invalid.history.reset();

    EXPECT_THROW(registry.stage_batch({std::move(invalid)}), std::invalid_argument);

    registry.stage_batch({request(transcript, 2, "one-id", "One")});
    registry.open_gate();
    EXPECT_TRUE(std::holds_alternative<AgentCompleted>(next_event(registry, 0)));
    registry.clear_batch();
    pool.stop();
}

TEST(AgentRegistry, RejectsDuplicateTargetsWithoutLeavingABatch) {
    Transcript transcript;
    ThreadPool pool(1);
    AgentRegistry registry(
        test::one_backend(std::make_unique<RecordingBackend>(
            "one-id", "One", "")),
        notifier(),
        pool);

    EXPECT_THROW(
        registry.stage_batch({
            request(transcript, 1, "one-id", "One"),
            request(transcript, 2, "one-id", "One"),
        }),
        std::invalid_argument);

    registry.stage_batch({request(transcript, 3, "one-id", "One")});
    registry.open_gate();
    EXPECT_TRUE(std::holds_alternative<AgentCompleted>(next_event(registry, 0)));
    registry.clear_batch();
    pool.stop();
}

TEST(AgentRegistry, RejectsStagingIntoAStoppedPoolWithoutCallingABackend) {
    Transcript transcript;
    ThreadPool pool(1);
    auto backend = std::make_unique<RecordingBackend>("one-id", "One", "");
    RecordingBackend* view = backend.get();
    AgentRegistry registry(test::one_backend(std::move(backend)), notifier(), pool);
    pool.stop();

    EXPECT_THROW(
        registry.stage_batch({request(transcript, 1, "one-id", "One")}),
        std::runtime_error);
    EXPECT_FALSE(view->prepared.load(std::memory_order_acquire));
    EXPECT_FALSE(view->performed.load(std::memory_order_acquire));
    // The failed submission did not install a batch that would make the
    // registry report "busy" instead of the pool admission failure.
    EXPECT_THROW(
        registry.stage_batch({request(transcript, 2, "one-id", "One")}),
        std::runtime_error);
}

TEST(AgentRegistry, RollsBackPartialSubmissionWithoutLeavingABatch) {
    Transcript transcript;
    ThreadPool pool(2);
    auto one = std::make_unique<RecordingBackend>("one-id", "One", "");
    auto two = std::make_unique<RecordingBackend>("two-id", "Two", "");
    RecordingBackend* one_view = one.get();
    RecordingBackend* two_view = two.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(one));
    backends.push_back(std::move(two));
    AgentRegistry registry(
        std::move(backends),
        notifier(),
        pool,
        [](std::size_t submission_index) {
            if (submission_index == 1) {
                throw std::runtime_error("injected submission failure");
            }
        });

    EXPECT_THROW(
        registry.stage_batch({
            request(transcript, 1, "one-id", "One"),
            request(transcript, 2, "two-id", "Two"),
        }),
        std::runtime_error);
    EXPECT_FALSE(one_view->prepared.load(std::memory_order_acquire));
    EXPECT_FALSE(two_view->prepared.load(std::memory_order_acquire));

    registry.stage_batch({request(transcript, 3, "one-id", "One")});
    registry.open_gate();
    EXPECT_TRUE(std::holds_alternative<AgentCompleted>(next_event(registry, 0)));
    registry.clear_batch();
    pool.stop();
}

TEST(AgentRegistry, CancellationBeforeStartDoesNotCallBackend) {
    Transcript transcript;
    ThreadPool pool(1);
    auto backend = std::make_unique<RecordingBackend>("one-id", "One", "one");
    RecordingBackend* view = backend.get();
    AgentRegistry registry(test::one_backend(std::move(backend)), notifier(), pool);

    registry.stage_batch({request(transcript, 1, "one-id", "One")});
    registry.cancel_batch();

    EXPECT_EQ(std::get<AgentCancelled>(next_event(registry, 0)).request_id, 1U);
    EXPECT_FALSE(view->prepared.load(std::memory_order_acquire));
    EXPECT_FALSE(view->performed.load(std::memory_order_acquire));
    registry.clear_batch();
    pool.stop();
}

TEST(AgentRegistry, StartsEveryConfiguredBackendAtFullPoolWidth) {
    Transcript transcript;
    BarrierState state;
    ThreadPool pool(3);
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::make_unique<BarrierBackend>("one-id", "One", state));
    backends.push_back(std::make_unique<BarrierBackend>("two-id", "Two", state));
    backends.push_back(std::make_unique<BarrierBackend>("three-id", "Three", state));
    AgentRegistry registry(std::move(backends), notifier(), pool);

    registry.stage_batch({
        request(transcript, 1, "one-id", "One"),
        request(transcript, 2, "two-id", "Two"),
        request(transcript, 3, "three-id", "Three"),
    });
    registry.open_gate();
    const bool all_entered = wait_until_entered(state, 3);
    state.release.store(true, std::memory_order_release);
    EXPECT_TRUE(all_entered);
    for (std::size_t index = 0; index < 3; ++index) {
        EXPECT_TRUE(std::holds_alternative<AgentCompleted>(
            next_event(registry, index)));
    }
    registry.clear_batch();
    EXPECT_TRUE(registry.executions_finished());
    pool.stop();
}

TEST(AgentRegistry, RoutesEachExecutionThroughItsStableIndex) {
    Transcript transcript;
    ThreadPool pool(2);
    auto one = std::make_unique<RecordingBackend>("one-id", "One", "first");
    auto two = std::make_unique<RecordingBackend>("two-id", "Two", "second");
    RecordingBackend* one_view = one.get();
    RecordingBackend* two_view = two.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(one));
    backends.push_back(std::move(two));
    AgentRegistry registry(std::move(backends), notifier(), pool);

    registry.stage_batch({
        request(transcript, 1, "one-id", "One", "One question"),
        request(transcript, 2, "two-id", "Two", "Two question"),
    });
    registry.open_gate();

    EXPECT_EQ(std::get<AgentDelta>(next_event(registry, 0)).text, "first");
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry, 0)).request_id, 1U);
    EXPECT_EQ(std::get<AgentDelta>(next_event(registry, 1)).text, "second");
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry, 1)).request_id, 2U);
    registry.clear_batch();

    EXPECT_EQ(one_view->received.run.prompt_text, "One question");
    EXPECT_EQ(two_view->received.run.prompt_text, "Two question");
    pool.stop();
}

TEST(AgentRegistry, RejectsAnotherBatchUntilTheLiveBatchIsCleared) {
    Transcript transcript;
    BarrierState state;
    ThreadPool pool(2);
    auto one = std::make_unique<BarrierBackend>("one-id", "One", state);
    auto two = std::make_unique<BarrierBackend>("two-id", "Two", state);
    BarrierBackend* two_view = two.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(one));
    backends.push_back(std::move(two));
    AgentRegistry registry(std::move(backends), notifier(), pool);

    registry.stage_batch({request(transcript, 1, "one-id", "One")});
    registry.open_gate();
    EXPECT_TRUE(wait_until_entered(state, 1));

    EXPECT_THROW(
        registry.stage_batch({request(transcript, 2, "two-id", "Two")}),
        std::runtime_error);
    EXPECT_FALSE(two_view->prepared.load(std::memory_order_acquire));

    state.release.store(true, std::memory_order_release);
    EXPECT_TRUE(std::holds_alternative<AgentCompleted>(next_event(registry, 0)));
    registry.clear_batch();

    registry.stage_batch({request(transcript, 3, "two-id", "Two")});
    registry.open_gate();
    EXPECT_TRUE(std::holds_alternative<AgentCompleted>(next_event(registry, 0)));
    registry.clear_batch();
    pool.stop();
}

TEST(AgentRegistry, MapsCompletionFailureToAgentFailed) {
    Transcript transcript;
    ThreadPool pool(1);
    AgentRegistry registry(
        test::one_backend(std::make_unique<RecordingBackend>(
            "one-id",
            "One",
            "",
            CompletionResult{
                CompletionOutcome::protocol_error,
                "malformed response"})),
        notifier(),
        pool);

    registry.stage_batch({request(transcript, 12, "one-id", "One")});
    registry.open_gate();

    const AgentFailed failed = std::get<AgentFailed>(next_event(registry, 0));
    EXPECT_EQ(failed.request_id, 12U);
    EXPECT_EQ(failed.message, "malformed response");
    registry.clear_batch();
    pool.stop();
}

TEST(AgentRegistry, CancellationAfterGateOpenSkipsBackendPreparation) {
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
    RecordingBackend* view = backend.get();
    AgentRegistry registry(test::one_backend(std::move(backend)), notifier(), pool);
    registry.stage_batch({request(transcript, 14, "one-id", "One")});
    registry.open_gate();
    registry.cancel_batch();
    release_blocker.store(true, std::memory_order_release);

    EXPECT_EQ(std::get<AgentCancelled>(next_event(registry, 0)).request_id, 14U);
    EXPECT_FALSE(view->prepared.load(std::memory_order_acquire));
    EXPECT_FALSE(view->performed.load(std::memory_order_acquire));
    registry.clear_batch();
    pool.stop();
}

TEST(AgentRegistry, PreservesCapturedHistoryForEveryExecution) {
    Transcript transcript;
    transcript.add_entry(test::human_entry(
        1, {"human", "You"}, {"one-id", "One"}, "Earlier question", 1));
    ThreadPool pool(2);
    auto one = std::make_unique<RecordingBackend>("one-id", "One", "");
    auto two = std::make_unique<RecordingBackend>("two-id", "Two", "");
    RecordingBackend* one_view = one.get();
    RecordingBackend* two_view = two.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(one));
    backends.push_back(std::move(two));
    AgentRegistry registry(std::move(backends), notifier(), pool);

    registry.stage_batch({
        request(transcript, 1, "one-id", "One"),
        request(transcript, 2, "two-id", "Two"),
    });
    transcript.clear();
    registry.open_gate();

    EXPECT_TRUE(std::holds_alternative<AgentCompleted>(next_event(registry, 0)));
    EXPECT_TRUE(std::holds_alternative<AgentCompleted>(next_event(registry, 1)));
    ASSERT_EQ(one_view->received.history->entries.size(), 1U);
    ASSERT_EQ(two_view->received.history->entries.size(), 1U);
    EXPECT_EQ(one_view->received.history->entries.front().text, "Earlier question");
    EXPECT_EQ(two_view->received.history->entries.front().text, "Earlier question");
    registry.clear_batch();
    pool.stop();
}

TEST(AgentRegistry, DeliversExactlyOneTerminalThenReportsEmpty) {
    Transcript transcript;
    ThreadPool pool(1);
    AgentRegistry registry(
        test::one_backend(std::make_unique<RecordingBackend>(
            "one-id", "One", "delta")),
        notifier(),
        pool);

    registry.stage_batch({request(transcript, 1, "one-id", "One")});
    registry.open_gate();

    EXPECT_TRUE(std::holds_alternative<AgentDelta>(next_event(registry, 0)));
    EXPECT_TRUE(std::holds_alternative<AgentCompleted>(next_event(registry, 0)));
    AgentEvent event = AgentCompleted{};
    EXPECT_EQ(registry.try_receive(0, event), ChannelReadStatus::empty);
    registry.clear_batch();
    pool.stop();
}

TEST(AgentRegistry, RejectsOutOfRangeRunIndexForALiveBatch) {
    Transcript transcript;
    ThreadPool pool(1);
    AgentRegistry registry(
        test::one_backend(std::make_unique<RecordingBackend>(
            "one-id", "One", "")),
        notifier(),
        pool);

    registry.stage_batch({request(transcript, 1, "one-id", "One")});
    AgentEvent event = AgentCompleted{};
    EXPECT_THROW((void)registry.try_receive(1, event), std::logic_error);
    registry.clear_batch();
    pool.stop();
}

TEST(AgentRegistry, CompletedAndCancelledBatchesPermitLaterBatches) {
    Transcript transcript;
    BarrierState state;
    ThreadPool pool(1);
    AgentRegistry registry(
        test::one_backend(std::make_unique<BarrierBackend>(
            "one-id", "One", state)),
        notifier(),
        pool);

    registry.stage_batch({request(transcript, 1, "one-id", "One")});
    registry.open_gate();
    EXPECT_TRUE(wait_until_entered(state, 1));
    state.release.store(true, std::memory_order_release);
    EXPECT_TRUE(std::holds_alternative<AgentCompleted>(next_event(registry, 0)));
    registry.clear_batch();

    state.release.store(false, std::memory_order_release);
    registry.stage_batch({request(transcript, 2, "one-id", "One")});
    registry.open_gate();
    EXPECT_TRUE(wait_until_entered(state, 2));
    registry.cancel_batch();
    EXPECT_TRUE(std::holds_alternative<AgentCancelled>(next_event(registry, 0)));
    registry.clear_batch();

    state.release.store(true, std::memory_order_release);
    registry.stage_batch({request(transcript, 3, "one-id", "One")});
    registry.open_gate();
    EXPECT_TRUE(std::holds_alternative<AgentCompleted>(next_event(registry, 0)));
    registry.clear_batch();
    pool.stop();
}

TEST(AgentRegistry, CancellationReachesEveryStartedBackend) {
    Transcript transcript;
    BarrierState state;
    ThreadPool pool(2);
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::make_unique<BarrierBackend>("one-id", "One", state));
    backends.push_back(std::make_unique<BarrierBackend>("two-id", "Two", state));
    AgentRegistry registry(std::move(backends), notifier(), pool);

    registry.stage_batch({
        request(transcript, 1, "one-id", "One"),
        request(transcript, 2, "two-id", "Two"),
    });
    registry.open_gate();
    EXPECT_TRUE(wait_until_entered(state, 2));
    registry.cancel_batch();

    EXPECT_TRUE(std::holds_alternative<AgentCancelled>(next_event(registry, 0)));
    EXPECT_TRUE(std::holds_alternative<AgentCancelled>(next_event(registry, 1)));
    registry.clear_batch();
    pool.stop();
}

TEST(AgentRegistry, StopLeavesTerminalEventsDrainableThenReportsClosed) {
    Transcript transcript;
    BarrierState state;
    ThreadPool pool(1);
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::make_unique<BarrierBackend>("one-id", "One", state));
    AgentRegistry registry(std::move(backends), notifier(), pool);

    registry.stage_batch({request(transcript, 1, "one-id", "One")});
    registry.open_gate();
    EXPECT_TRUE(wait_until_entered(state, 1));
    registry.stop();

    EXPECT_TRUE(std::holds_alternative<AgentCancelled>(next_event(registry, 0)));
    AgentEvent event = AgentCompleted{};
    EXPECT_EQ(registry.try_receive(0, event), ChannelReadStatus::closed);
    registry.clear_batch();
    pool.stop();
}

TEST(AgentRegistry, ConvertsBackendExceptionsToFailedTerminalEvents) {
    Transcript transcript;
    ThreadPool pool(1);
    AgentRegistry registry(
        test::one_backend(std::make_unique<ThrowingBackend>()), notifier(), pool);

    registry.stage_batch({request(transcript, 1, "one-id", "One")});
    registry.open_gate();

    const AgentFailed failed = std::get<AgentFailed>(next_event(registry, 0));
    EXPECT_EQ(failed.request_id, 1U);
    EXPECT_NE(failed.message.find("preparation failed"), std::string::npos);
    registry.clear_batch();
    pool.stop();
}

} // namespace
} // namespace cha
