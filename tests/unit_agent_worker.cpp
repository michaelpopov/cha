#include "agent_worker.h"

#include <gtest/gtest.h>

#include <poll.h>

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

CompletionRequest request(
    Conversation& conversation,
    RequestId request_id,
    std::string agent_id,
    std::string prompt) {
    CompletionRequest result{
        .request_id = request_id,
        .prompt = make_human_entry(1000 + request_id, std::move(agent_id), "Assistant", std::move(prompt), request_id),
    };
    conversation.add_entry(result.prompt);
    result.conversation_revision = conversation.revision();
    return result;
}

AgentEvent next_event(AgentEventChannel& events) {
    pollfd descriptor{events.notification_fd(), POLLIN, 0};
    if (::poll(&descriptor, 1, 1000) != 1) {
        throw std::runtime_error("Timed out waiting for agent event");
    }
    AgentEvent event = AgentCompleted{};
    if (events.try_get(event) != ChannelReadStatus::value) {
        throw std::runtime_error("Agent event channel closed unexpectedly");
    }
    return event;
}

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

    RequestPayload prepare(
        const CompletionRequest& completion_request,
        const ConversationReadView& conversation) override {
        requests.push_back(completion_request);
        latest_prompts.push_back(conversation.entries().back());
        return {.bytes = completion_request.prompt.text};
    }

    CompletionResult perform(
        RequestPayload,
        const CompletionDeltaSink& on_delta,
        const std::atomic_bool& cancellation) override {
        for (const std::string& delta : deltas_) {
            on_delta(delta);
        }
        if (wait_for_cancellation_) {
            while (!cancellation.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            while (release_after_cancellation_
                   && !release_after_cancellation_->load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return {CompletionOutcome::cancelled, {}};
        }
        return result_;
    }

    AgentInfo info() const override {
        return {
            .id = id_, .name = "Fake assistant", .model = "fake-model",
            .api = "fake://completion", .streaming = true,
        };
    }

    const std::string& agent_id() const override { return id_; }

    std::vector<CompletionRequest> requests;
    std::vector<ConversationEntry> latest_prompts;

private:
    std::string id_{"assistant"};
    CompletionResult result_;
    std::vector<std::string> deltas_;
    bool wait_for_cancellation_{};
    const std::atomic_bool* release_after_cancellation_{};
};

class BoundaryBackend final : public CompletionBackend {
public:
    RequestPayload prepare(
        const CompletionRequest& request,
        const ConversationReadView& conversation) override {
        if (conversation.entries().back() != request.prompt) {
            throw std::logic_error("prompt was not latest");
        }
        prepared.set_value();
        return {.bytes = request.prompt.text};
    }

    CompletionResult perform(
        RequestPayload,
        const CompletionDeltaSink&,
        const std::atomic_bool&) override {
        while (!release.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return {};
    }

    AgentInfo info() const override {
        return {.id = id_, .name = "Boundary", .model = "fake", .api = "fake://", .streaming = true};
    }

    const std::string& agent_id() const override { return id_; }

    std::promise<void> prepared;
    std::atomic_bool release{false};

private:
    std::string id_{"assistant"};
};

class ThrowingPrepareBackend final : public CompletionBackend {
public:
    RequestPayload prepare(
        const CompletionRequest&,
        const ConversationReadView&) override {
        throw std::runtime_error("preparation failed");
    }

    CompletionResult perform(
        RequestPayload,
        const CompletionDeltaSink&,
        const std::atomic_bool&) override {
        performed.store(true, std::memory_order_release);
        return {};
    }

    AgentInfo info() const override {
        return {.id = id_, .name = "Throwing", .model = "fake", .api = "fake://", .streaming = true};
    }

    const std::string& agent_id() const override { return id_; }

    std::atomic_bool performed{false};

private:
    std::string id_{"assistant"};
};

TEST(AgentWorker, StartsOnConstructionAndStopsIdempotently) {
    Conversation conversation;
    AgentEventChannel events;
    AgentWorker worker(conversation, events, std::make_unique<FakeCompletionBackend>());

    ASSERT_TRUE(worker.submit(request(conversation, 1, "assistant", "Question")));
    EXPECT_EQ(std::get<AgentCompleted>(next_event(events)).request_id, 1U);
    worker.stop();
    EXPECT_NO_THROW(worker.stop());
    EXPECT_FALSE(worker.submit(request(conversation, 2, "assistant", "Another question")));
}

TEST(AgentWorker, RejectsARequestForAnotherAgentAndContinues) {
    Conversation conversation;
    auto backend = std::make_unique<FakeCompletionBackend>(
        CompletionResult{}, std::vector<std::string>{"accepted"});
    FakeCompletionBackend* backend_view = backend.get();
    AgentEventChannel events;
    AgentWorker worker(conversation, events, std::move(backend));

    CompletionRequest rejected{
        .request_id = 1,
        .conversation_revision = 1,
        .prompt = make_human_entry(1001, "other-id", "Other", "rejected", 1),
    };
    ASSERT_TRUE(worker.submit(std::move(rejected)));
    const AgentFailed failed = std::get<AgentFailed>(next_event(events));
    EXPECT_EQ(failed.request_id, 1U);
    EXPECT_NE(failed.message.find("targets agent"), std::string::npos);

    ASSERT_TRUE(worker.submit(request(conversation, 2, "assistant", "accepted")));
    EXPECT_EQ(std::get<AgentDelta>(next_event(events)).text, "accepted");
    EXPECT_EQ(std::get<AgentCompleted>(next_event(events)).request_id, 2U);
    worker.stop();
    ASSERT_EQ(backend_view->requests.size(), 1U);
    EXPECT_EQ(backend_view->requests.front().request_id, 2U);
}

TEST(AgentWorker, RejectsAnInvalidTypedPromptAndContinues) {
    Conversation conversation;
    auto backend = std::make_unique<FakeCompletionBackend>();
    FakeCompletionBackend* backend_view = backend.get();
    AgentEventChannel events;
    AgentWorker worker(conversation, events, std::move(backend));
    CompletionRequest invalid = request(conversation, 1, "assistant", "rejected");
    invalid.prompt.status = CompletionStatus::cancelled;

    ASSERT_TRUE(worker.submit(std::move(invalid)));
    const AgentFailed failed = std::get<AgentFailed>(next_event(events));
    EXPECT_EQ(failed.request_id, 1U);
    EXPECT_NE(failed.message.find("require complete status"), std::string::npos);
    worker.stop();
    EXPECT_TRUE(backend_view->requests.empty());
}

TEST(AgentWorker, MapsCompletionDeltasAndSuccessToIdentifiedEvents) {
    Conversation conversation;
    conversation.add_entry(make_human_entry(1, "assistant", "Assistant", "Earlier question", 3));
    auto backend = std::make_unique<FakeCompletionBackend>(
        CompletionResult{}, std::vector<std::string>{"Hello", " world"});
    FakeCompletionBackend* backend_view = backend.get();
    AgentEventChannel events;
    AgentWorker worker(conversation, events, std::move(backend));

    ASSERT_TRUE(worker.submit(request(conversation, 10, "assistant", "Current question")));
    const AgentDelta first = std::get<AgentDelta>(next_event(events));
    const AgentDelta second = std::get<AgentDelta>(next_event(events));
    EXPECT_EQ(first.request_id, 10U);
    EXPECT_EQ(first.text, "Hello");
    EXPECT_EQ(second.request_id, 10U);
    EXPECT_EQ(second.text, " world");
    EXPECT_EQ(std::get<AgentCompleted>(next_event(events)).request_id, 10U);
    worker.stop();

    ASSERT_EQ(backend_view->requests.size(), 1U);
    EXPECT_EQ(backend_view->requests.front().prompt.text, "Current question");
    EXPECT_EQ(backend_view->latest_prompts.front(), backend_view->requests.front().prompt);
    EXPECT_EQ(worker.info().model, "fake-model");
}

TEST(AgentWorker, MapsCompletionFailureToAgentFailed) {
    Conversation conversation;
    AgentEventChannel events;
    AgentWorker worker(conversation, events, std::make_unique<FakeCompletionBackend>(
        CompletionResult{CompletionOutcome::protocol_error, "malformed response"}));

    ASSERT_TRUE(worker.submit(request(conversation, 12, "assistant", "Question")));
    const AgentFailed failed = std::get<AgentFailed>(next_event(events));
    EXPECT_EQ(failed.request_id, 12U);
    EXPECT_EQ(failed.message, "malformed response");
}

TEST(AgentWorker, CancelsItsActiveRequestWithItsOwnToken) {
    Conversation conversation;
    AgentEventChannel events;
    AgentWorker worker(conversation, events, std::make_unique<FakeCompletionBackend>(
        CompletionResult{}, std::vector<std::string>{"Partial"}, true));

    ASSERT_TRUE(worker.submit(request(conversation, 13, "assistant", "Question")));
    EXPECT_EQ(std::get<AgentDelta>(next_event(events)).text, "Partial");
    worker.cancel();
    EXPECT_EQ(std::get<AgentCancelled>(next_event(events)).request_id, 13U);
}

TEST(AgentWorker, RejectsRevisionMismatchBeforeBackendPreparation) {
    Conversation conversation;
    auto backend = std::make_unique<FakeCompletionBackend>();
    FakeCompletionBackend* backend_view = backend.get();
    AgentEventChannel events;
    AgentWorker worker(conversation, events, std::move(backend));
    CompletionRequest pending = request(conversation, 20, "assistant", "Question");
    conversation.clear();

    ASSERT_TRUE(worker.submit(std::move(pending)));
    const AgentFailed failed = std::get<AgentFailed>(next_event(events));
    EXPECT_NE(failed.message.find("revision changed"), std::string::npos);
    EXPECT_TRUE(backend_view->requests.empty());
}

TEST(AgentWorker, ReleasesTheConversationViewBeforePerforming) {
    Conversation conversation;
    auto backend = std::make_unique<BoundaryBackend>();
    BoundaryBackend* backend_view = backend.get();
    std::future<void> prepared = backend_view->prepared.get_future();
    AgentEventChannel events;
    AgentWorker worker(conversation, events, std::move(backend));

    ASSERT_TRUE(worker.submit(request(conversation, 21, "assistant", "Question")));
    ASSERT_EQ(prepared.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    std::future<void> mutation = std::async(std::launch::async, [&conversation] {
        conversation.clear();
    });
    EXPECT_EQ(mutation.wait_for(std::chrono::milliseconds(100)), std::future_status::ready);
    mutation.get();

    backend_view->release.store(true, std::memory_order_release);
    EXPECT_EQ(std::get<AgentCompleted>(next_event(events)).request_id, 21U);
}

TEST(AgentWorker, FailingPreparationReleasesTheViewAndDoesNotPerform) {
    Conversation conversation;
    auto backend = std::make_unique<ThrowingPrepareBackend>();
    ThrowingPrepareBackend* backend_view = backend.get();
    AgentEventChannel events;
    AgentWorker worker(conversation, events, std::move(backend));

    ASSERT_TRUE(worker.submit(request(conversation, 22, "assistant", "Question")));
    const AgentFailed failed = std::get<AgentFailed>(next_event(events));
    EXPECT_EQ(failed.request_id, 22U);
    EXPECT_NE(failed.message.find("preparation failed"), std::string::npos);
    EXPECT_FALSE(backend_view->performed.load(std::memory_order_acquire));

    EXPECT_NO_THROW(conversation.clear());
    AgentEvent no_second_event = AgentCompleted{};
    EXPECT_EQ(events.try_get(no_second_event), ChannelReadStatus::empty);
}

} // namespace
} // namespace cha
