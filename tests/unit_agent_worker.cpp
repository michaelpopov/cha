#include "agent_worker.h"

#include <gtest/gtest.h>

#include <poll.h>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace cha {
namespace {

CompletionRequest request(
    RequestId request_id,
    std::string agent_id,
    std::string prompt,
    std::vector<ConversationEntry> history = {}) {
    return {
        .request_id = request_id,
        .agent_id = std::move(agent_id),
        .history = std::move(history),
        .prompt =
            make_human_entry(
                1000 + request_id,
                std::move(prompt),
                request_id),
    };
}

AgentEvent next_event(AgentWorker& worker) {
    pollfd descriptor{worker.notification_fd(), POLLIN, 0};
    if (::poll(&descriptor, 1, 1000) != 1) {
        throw std::runtime_error("Timed out waiting for agent event");
    }
    AgentEvent event = AgentCompleted{};
    if (worker.try_receive(event) != ChannelReadStatus::value) {
        throw std::runtime_error("Agent event channel closed unexpectedly");
    }
    return event;
}

// Supplies deterministic results while recording immutable requests passed by the worker.
class FakeCompletionBackend final : public CompletionBackend {
public:
    explicit FakeCompletionBackend(
        CompletionResult result = {},
        std::vector<std::string> deltas = {},
        bool wait_for_cancellation = false,
        const std::atomic_bool* release_after_cancellation = nullptr)
      : result_(std::move(result)),
        deltas_(std::move(deltas)),
        wait_for_cancellation_(wait_for_cancellation),
        release_after_cancellation_(release_after_cancellation) {
    }

    CompletionResult complete(
        const CompletionRequest& completion_request,
        const CompletionDeltaSink& on_delta,
        const std::atomic_bool& cancellation) override {
        requests.push_back(completion_request);
        for (const std::string& delta : deltas_) {
            on_delta(delta);
        }
        if (wait_for_cancellation_) {
            while (!cancellation.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            while (release_after_cancellation_
                   && !release_after_cancellation_->load(
                       std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return {CompletionOutcome::cancelled, {}};
        }
        return result_;
    }

    AgentInfo info() const override {
        return {
            .id = id_,
            .name = "Fake assistant",
            .model = "fake-model",
            .api = "fake://completion",
            .streaming = true,
        };
    }

    const std::string& agent_id() const override {
        return id_;
    }

    std::vector<CompletionRequest> requests;

private:
    std::string id_{"assistant"};
    CompletionResult result_;
    std::vector<std::string> deltas_;
    bool wait_for_cancellation_{};
    const std::atomic_bool* release_after_cancellation_{};
};

TEST(AgentWorker, StartsOnConstructionAndStopsIdempotently) {
    AgentWorker worker(std::make_unique<FakeCompletionBackend>());

    ASSERT_TRUE(worker.submit(
        request(1, "assistant", "Question")));
    EXPECT_EQ(
        std::get<AgentCompleted>(next_event(worker)).request_id,
        1U);
    worker.stop();
    EXPECT_NO_THROW(worker.stop());
    EXPECT_FALSE(worker.submit(
        request(2, "assistant", "Another question")));
}

TEST(AgentWorker, RejectsARequestForAnotherAgentAndContinues) {
    auto backend = std::make_unique<FakeCompletionBackend>(
        CompletionResult{},
        std::vector<std::string>{"accepted"});
    FakeCompletionBackend* backend_view = backend.get();
    AgentWorker worker(std::move(backend));

    ASSERT_TRUE(worker.submit(request(1, "other-id", "rejected")));
    const AgentFailed failed =
        std::get<AgentFailed>(next_event(worker));
    EXPECT_EQ(failed.request_id, 1U);
    EXPECT_NE(failed.message.find("targets agent"), std::string::npos);

    ASSERT_TRUE(worker.submit(request(2, "assistant", "accepted")));
    EXPECT_EQ(std::get<AgentDelta>(next_event(worker)).text, "accepted");
    EXPECT_EQ(
        std::get<AgentCompleted>(next_event(worker)).request_id,
        2U);
    worker.stop();
    ASSERT_EQ(backend_view->requests.size(), 1U);
    EXPECT_EQ(backend_view->requests.front().request_id, 2U);
}

TEST(AgentWorker, RejectsAnInvalidTypedPromptAndContinues) {
    auto backend = std::make_unique<FakeCompletionBackend>();
    FakeCompletionBackend* backend_view = backend.get();
    AgentWorker worker(std::move(backend));
    CompletionRequest invalid =
        request(1, "assistant", "rejected");
    invalid.prompt.status = CompletionStatus::cancelled;

    ASSERT_TRUE(worker.submit(std::move(invalid)));
    const AgentFailed failed =
        std::get<AgentFailed>(next_event(worker));
    EXPECT_EQ(failed.request_id, 1U);
    EXPECT_NE(
        failed.message.find("require complete status"),
        std::string::npos);

    ASSERT_TRUE(worker.submit(request(2, "assistant", "accepted")));
    EXPECT_EQ(
        std::get<AgentCompleted>(next_event(worker)).request_id,
        2U);
    worker.stop();
    ASSERT_EQ(backend_view->requests.size(), 1U);
}

TEST(AgentWorker, MapsCompletionDeltasAndSuccessToIdentifiedEvents) {
    auto backend = std::make_unique<FakeCompletionBackend>(
        CompletionResult{},
        std::vector<std::string>{"Hello", " world"});
    FakeCompletionBackend* backend_view = backend.get();
    AgentWorker worker(std::move(backend));
    const ConversationEntry prior =
        make_human_entry(1, "Earlier question", 3);

    ASSERT_TRUE(worker.submit(request(
        10,
        "assistant",
        "Current question",
        {prior})));

    const AgentDelta first =
        std::get<AgentDelta>(next_event(worker));
    const AgentDelta second =
        std::get<AgentDelta>(next_event(worker));
    EXPECT_EQ(first.request_id, 10U);
    EXPECT_EQ(first.text, "Hello");
    EXPECT_EQ(second.request_id, 10U);
    EXPECT_EQ(second.text, " world");
    EXPECT_EQ(
        std::get<AgentCompleted>(next_event(worker)).request_id,
        10U);
    worker.stop();

    ASSERT_EQ(backend_view->requests.size(), 1U);
    EXPECT_EQ(
        backend_view->requests.front().history,
        (std::vector{prior}));
    EXPECT_EQ(
        backend_view->requests.front().prompt.text,
        "Current question");
    EXPECT_EQ(worker.info().model, "fake-model");
}

TEST(AgentWorker, MapsCompletionFailureToAgentFailed) {
    AgentWorker worker(std::make_unique<FakeCompletionBackend>(
        CompletionResult{
            CompletionOutcome::protocol_error,
            "malformed response",
        }));

    ASSERT_TRUE(worker.submit(request(12, "assistant", "Question")));

    const AgentFailed failed =
        std::get<AgentFailed>(next_event(worker));
    EXPECT_EQ(failed.request_id, 12U);
    EXPECT_EQ(failed.message, "malformed response");
}

TEST(AgentWorker, CancelsItsActiveRequestWithItsOwnToken) {
    AgentWorker worker(std::make_unique<FakeCompletionBackend>(
        CompletionResult{},
        std::vector<std::string>{"Partial"},
        true));

    ASSERT_TRUE(worker.submit(request(13, "assistant", "Question")));
    EXPECT_EQ(std::get<AgentDelta>(next_event(worker)).text, "Partial");
    worker.cancel();
    EXPECT_EQ(
        std::get<AgentCancelled>(next_event(worker)).request_id,
        13U);
}

TEST(AgentWorker, RejectsASecondRequestWithoutUndoingCancellation) {
    std::atomic_bool release_after_cancellation{false};
    AgentWorker worker(std::make_unique<FakeCompletionBackend>(
        CompletionResult{},
        std::vector<std::string>{"Partial"},
        true,
        &release_after_cancellation));

    ASSERT_TRUE(worker.submit(
        request(14, "assistant", "First question")));
    EXPECT_EQ(
        std::get<AgentDelta>(next_event(worker)).text,
        "Partial");

    worker.cancel();
    EXPECT_FALSE(worker.submit(
        request(15, "assistant", "Second question")));
    release_after_cancellation.store(
        true,
        std::memory_order_release);
    EXPECT_EQ(
        std::get<AgentCancelled>(next_event(worker)).request_id,
        14U);
}

} // namespace
} // namespace cha
