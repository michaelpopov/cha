#include "agent.h"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
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
            make_human_entry(1000 + request_id, std::move(prompt), request_id),
    };
}

AgentEvent next_event(AgentEventChannel& events) {
    std::optional<AgentEvent> event = events.get();
    if (!event) {
        throw std::runtime_error(
            "Agent event channel closed unexpectedly");
    }
    return std::move(*event);
}

// Supplies deterministic completion results without HTTP or worker-channel coupling.
class FakeCompletionBackend final : public CompletionBackend {
public:
    explicit FakeCompletionBackend(
        CompletionResult result = {},
        std::vector<std::string> deltas = {})
        : result_(std::move(result)),
          deltas_(std::move(deltas)) {
    }

    CompletionResult complete(
        const CompletionRequest& completion_request,
        const CompletionDeltaSink& on_delta) override {
        requests.push_back(completion_request);
        for (const std::string& delta : deltas_) {
            on_delta(delta);
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
};

TEST(Agent, CanOnlyBeStartedOnce) {
    CompletionRequestChannel first_requests;
    CompletionRequestChannel second_requests;
    AgentEventChannel events;
    std::atomic_bool cancellation{false};
    Agent agent(
        std::make_unique<FakeCompletionBackend>(),
        cancellation);

    agent.start(first_requests, events);
    EXPECT_THROW(
        agent.start(second_requests, events),
        std::logic_error);
    agent.stop();
    EXPECT_THROW(
        agent.start(second_requests, events),
        std::logic_error);
    EXPECT_NO_THROW(agent.stop());
}

TEST(Agent, StoppingDoesNotCloseTheSharedOutputChannel) {
    CompletionRequestChannel requests;
    AgentEventChannel events;
    std::atomic_bool cancellation{false};
    Agent agent(
        std::make_unique<FakeCompletionBackend>(),
        cancellation);

    agent.start(requests, events);
    agent.stop();

    AgentEvent event = AgentCompleted{};
    EXPECT_EQ(events.try_get(event), ChannelReadStatus::empty);
    EXPECT_TRUE(events.push(AgentCompleted{7}));
    EXPECT_EQ(
        std::get<AgentCompleted>(next_event(events)).request_id,
        7U);
}

TEST(Agent, RejectsARequestForAnotherAgentAndContinues) {
    auto backend = std::make_unique<FakeCompletionBackend>(
        CompletionResult{},
        std::vector<std::string>{"accepted"});
    FakeCompletionBackend* backend_view = backend.get();
    CompletionRequestChannel requests;
    AgentEventChannel events;
    std::atomic_bool cancellation{false};
    Agent agent(std::move(backend), cancellation);

    agent.start(requests, events);
    ASSERT_TRUE(requests.push(request(1, "other-id", "rejected")));
    const AgentFailed failed =
        std::get<AgentFailed>(next_event(events));
    EXPECT_EQ(failed.request_id, 1U);
    EXPECT_NE(
        failed.message.find("targets agent"),
        std::string::npos);

    ASSERT_TRUE(
        requests.push(request(2, "assistant", "accepted")));
    EXPECT_EQ(
        std::get<AgentDelta>(next_event(events)).text,
        "accepted");
    EXPECT_EQ(
        std::get<AgentCompleted>(next_event(events)).request_id,
        2U);
    agent.stop();
    ASSERT_EQ(backend_view->requests.size(), 1U);
    EXPECT_EQ(backend_view->requests.front().request_id, 2U);
}

TEST(Agent, RejectsAnInvalidTypedPromptAndContinues) {
    auto backend = std::make_unique<FakeCompletionBackend>();
    FakeCompletionBackend* backend_view = backend.get();
    CompletionRequestChannel requests;
    AgentEventChannel events;
    std::atomic_bool cancellation{false};
    Agent agent(std::move(backend), cancellation);

    CompletionRequest invalid =
        request(1, "assistant", "rejected");
    invalid.prompt.status = CompletionStatus::cancelled;

    agent.start(requests, events);
    ASSERT_TRUE(requests.push(std::move(invalid)));
    const AgentFailed failed =
        std::get<AgentFailed>(next_event(events));
    EXPECT_EQ(failed.request_id, 1U);
    EXPECT_NE(
        failed.message.find("require complete status"),
        std::string::npos);

    ASSERT_TRUE(
        requests.push(request(2, "assistant", "accepted")));
    EXPECT_EQ(
        std::get<AgentCompleted>(next_event(events)).request_id,
        2U);
    agent.stop();
    ASSERT_EQ(backend_view->requests.size(), 1U);
    EXPECT_EQ(backend_view->requests.front().request_id, 2U);
}

TEST(Agent, CancelsAnIdentifiedRequestBeforeStartingIt) {
    auto backend = std::make_unique<FakeCompletionBackend>();
    FakeCompletionBackend* backend_view = backend.get();
    CompletionRequestChannel requests;
    AgentEventChannel events;
    std::atomic_bool cancellation{true};
    Agent agent(std::move(backend), cancellation);
    agent.start(requests, events);

    ASSERT_TRUE(
        requests.push(request(9, "assistant", "Do not run")));
    EXPECT_EQ(
        std::get<AgentCancelled>(next_event(events)).request_id,
        9U);
    agent.stop();
    EXPECT_TRUE(backend_view->requests.empty());
}

TEST(Agent, MapsCompletionDeltasAndSuccessToIdentifiedEvents) {
    auto backend = std::make_unique<FakeCompletionBackend>(
        CompletionResult{},
        std::vector<std::string>{"Hello", " world"});
    FakeCompletionBackend* backend_view = backend.get();
    CompletionRequestChannel requests;
    AgentEventChannel events;
    std::atomic_bool cancellation{false};
    Agent agent(std::move(backend), cancellation);

    const ConversationEntry prior =
        make_human_entry(1, "Earlier question", 3);
    agent.start(requests, events);
    ASSERT_TRUE(requests.push(request(
        10,
        "assistant",
        "Current question",
        {prior})));

    const AgentDelta first =
        std::get<AgentDelta>(next_event(events));
    const AgentDelta second =
        std::get<AgentDelta>(next_event(events));
    EXPECT_EQ(first.request_id, 10U);
    EXPECT_EQ(first.text, "Hello");
    EXPECT_EQ(second.request_id, 10U);
    EXPECT_EQ(second.text, " world");
    EXPECT_EQ(
        std::get<AgentCompleted>(next_event(events)).request_id,
        10U);
    agent.stop();

    ASSERT_EQ(backend_view->requests.size(), 1U);
    EXPECT_EQ(backend_view->requests.front().history, (std::vector{prior}));
    EXPECT_EQ(
        backend_view->requests.front().prompt.text,
        "Current question");
    EXPECT_EQ(agent.info().model, "fake-model");
}

TEST(Agent, MapsCompletionFailureToAgentFailed) {
    auto backend = std::make_unique<FakeCompletionBackend>(
        CompletionResult{
            CompletionOutcome::protocol_error,
            "malformed response",
        });
    CompletionRequestChannel requests;
    AgentEventChannel events;
    std::atomic_bool cancellation{false};
    Agent agent(std::move(backend), cancellation);

    agent.start(requests, events);
    ASSERT_TRUE(
        requests.push(request(12, "assistant", "Question")));

    const AgentFailed failed =
        std::get<AgentFailed>(next_event(events));
    EXPECT_EQ(failed.request_id, 12U);
    EXPECT_EQ(failed.message, "malformed response");
    agent.stop();
}

TEST(Agent, MapsCompletionCancellationToAgentCancelled) {
    auto backend = std::make_unique<FakeCompletionBackend>(
        CompletionResult{CompletionOutcome::cancelled, {}});
    CompletionRequestChannel requests;
    AgentEventChannel events;
    std::atomic_bool cancellation{false};
    Agent agent(std::move(backend), cancellation);

    agent.start(requests, events);
    ASSERT_TRUE(
        requests.push(request(13, "assistant", "Question")));
    EXPECT_EQ(
        std::get<AgentCancelled>(next_event(events)).request_id,
        13U);
    agent.stop();
}

} // namespace
} // namespace cha
