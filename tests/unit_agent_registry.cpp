#include "agent_registry.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace cha {
namespace {

class RegistryBackend final : public CompletionBackend {
public:
    RegistryBackend(std::string id, std::string name)
        : id_(std::move(id)), name_(std::move(name)) {
    }

    RequestPayload prepare(
        const CompletionRequest& request,
        const ConversationReadView&) override {
        prepared_requests.push_back(request.request_id);
        return {.bytes = request.prompt.text};
    }

    CompletionResult perform(
        RequestPayload payload,
        const CompletionDeltaSink& on_delta,
        const std::atomic_bool&) override {
        performed = true;
        on_delta(name_ + ":" + payload.bytes);
        return {};
    }

    AgentInfo info() const override {
        return {id_, name_, "model", "test://completion", true};
    }

    const std::string& agent_id() const override { return id_; }

    std::vector<RequestId> prepared_requests;
    bool performed{};

private:
    std::string id_;
    std::string name_;
};

class BlockingRegistryBackend final : public CompletionBackend {
public:
    BlockingRegistryBackend(std::string id, std::string name)
        : id_(std::move(id)), name_(std::move(name)) {
    }

    RequestPayload prepare(
        const CompletionRequest& request,
        const ConversationReadView&) override {
        prepared.store(true, std::memory_order_release);
        return {.bytes = request.prompt.text};
    }

    CompletionResult perform(
        RequestPayload payload,
        const CompletionDeltaSink& on_delta,
        const std::atomic_bool& cancellation) override {
        on_delta(name_ + ":" + payload.bytes);
        entered_perform.store(true, std::memory_order_release);
        while (!release.load(std::memory_order_acquire)
               && !cancellation.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return cancellation.load(std::memory_order_acquire)
            ? CompletionResult{CompletionOutcome::cancelled, {}}
            : CompletionResult{};
    }

    AgentInfo info() const override {
        return {id_, name_, "model", "test://completion", true};
    }

    const std::string& agent_id() const override { return id_; }

    std::atomic_bool entered_perform{false};
    std::atomic_bool prepared{false};
    std::atomic_bool release{false};

private:
    std::string id_;
    std::string name_;
};

class ConfigurableBackend final : public CompletionBackend {
public:
    explicit ConfigurableBackend(
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
            .name = "Fake",
            .model = "fake-model",
            .api = "fake://completion",
            .streaming = true,
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
        return {
            .id = id_,
            .name = "Boundary",
            .model = "fake",
            .api = "fake://",
            .streaming = true,
        };
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
        return {
            .id = id_,
            .name = "Throwing",
            .model = "fake",
            .api = "fake://",
            .streaming = true,
        };
    }

    const std::string& agent_id() const override { return id_; }

    std::atomic_bool performed{false};

private:
    std::string id_{"assistant"};
};

AgentEvent next_event(AgentRegistry& registry) {
    pollfd descriptor{registry.notification_fd(), POLLIN, 0};
    if (::poll(&descriptor, 1, 1000) != 1) {
        throw std::runtime_error("Timed out waiting for registry event");
    }
    AgentEvent event = AgentCompleted{};
    if (registry.try_receive(event) != ChannelReadStatus::value) {
        throw std::runtime_error("Registry event channel closed unexpectedly");
    }
    return event;
}

CompletionRequest request(
    Conversation& conversation,
    RequestId id,
    std::string target,
    std::string name,
    std::string text) {
    CompletionRequest result{
        .request_id = id,
        .prompt = make_human_entry(id, std::move(target), std::move(name), std::move(text), id),
    };
    conversation.add_entry(result.prompt);
    return result;
}

TEST(AgentRegistry, RoutesPromptTargetsToTheMatchingBackendAndSharesOneChannel) {
    Conversation conversation;
    auto alpha = std::make_unique<RegistryBackend>("alpha-id", "Alpha");
    auto beta = std::make_unique<RegistryBackend>("beta-id", "Beta");
    RegistryBackend* alpha_view = alpha.get();
    RegistryBackend* beta_view = beta.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(alpha));
    backends.push_back(std::move(beta));
    AgentRegistry registry(conversation, std::move(backends));

    ASSERT_TRUE(registry.submit(request(conversation, 1, "beta-id", "Beta", "hello")));
    EXPECT_EQ(std::get<AgentDelta>(next_event(registry)).text, "Beta:hello");
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry)).request_id, 1U);
    EXPECT_TRUE(alpha_view->prepared_requests.empty());
    EXPECT_EQ(beta_view->prepared_requests, (std::vector<RequestId>{1}));

    CompletionRequest unknown = request(conversation, 2, "missing-id", "Missing", "nope");
    EXPECT_FALSE(registry.submit(std::move(unknown)));
    registry.stop();
    AgentEvent event = AgentCompleted{};
    EXPECT_EQ(registry.try_receive(event), ChannelReadStatus::closed);
}

TEST(AgentRegistry, RejectsInvalidBackendMetadataAtTheRegistryBoundary) {
    Conversation conversation;
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::make_unique<RegistryBackend>("bad-id", "Bad name"));
    EXPECT_THROW(AgentRegistry registry(conversation, std::move(backends)), std::invalid_argument);
}

TEST(AgentRegistry, SerializesRequestsAcrossDifferentBackends) {
    Conversation conversation;
    auto alpha = std::make_unique<BlockingRegistryBackend>("alpha-id", "Alpha");
    auto beta = std::make_unique<BlockingRegistryBackend>("beta-id", "Beta");
    BlockingRegistryBackend* alpha_view = alpha.get();
    BlockingRegistryBackend* beta_view = beta.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(alpha));
    backends.push_back(std::move(beta));
    AgentRegistry registry(conversation, std::move(backends));

    ASSERT_TRUE(registry.submit(request(conversation, 1, "alpha-id", "Alpha", "one")));
    EXPECT_EQ(std::get<AgentDelta>(next_event(registry)).text, "Alpha:one");
    ASSERT_TRUE(alpha_view->entered_perform.load(std::memory_order_acquire));

    EXPECT_FALSE(registry.submit(request(conversation, 2, "beta-id", "Beta", "two")));
    EXPECT_FALSE(beta_view->prepared.load(std::memory_order_acquire));
    EXPECT_FALSE(beta_view->entered_perform.load(std::memory_order_acquire));

    alpha_view->release.store(true, std::memory_order_release);
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry)).request_id, 1U);

    ASSERT_TRUE(registry.submit(request(conversation, 3, "beta-id", "Beta", "three")));
    EXPECT_EQ(std::get<AgentDelta>(next_event(registry)).text, "Beta:three");
    ASSERT_TRUE(beta_view->entered_perform.load(std::memory_order_acquire));
    beta_view->release.store(true, std::memory_order_release);
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry)).request_id, 3U);

    registry.stop();
    AgentEvent event = AgentCompleted{};
    EXPECT_EQ(registry.try_receive(event), ChannelReadStatus::closed);
}

TEST(AgentRegistry, RejectsEmptyConstruction) {
    Conversation conversation;
    EXPECT_THROW(
        AgentRegistry registry(
            conversation, std::vector<std::unique_ptr<CompletionBackend>>{}),
        std::invalid_argument);
}

TEST(AgentRegistry, RejectsNullBackend) {
    Conversation conversation;
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(nullptr);
    EXPECT_THROW(
        AgentRegistry registry(conversation, std::move(backends)),
        std::invalid_argument);
}

TEST(AgentRegistry, IdentifiesThePersonaWhoseStartupFails) {
    Conversation conversation;
    AgentDefinition definition{
        .config = {
            .id = "alpha-id",
            .name = "Alpha",
            .api_key_env = "__CHA_TEST_MISSING_AGENT_KEY__",
        },
        .system_prompt = "Prompt",
    };

    try {
        AgentRegistry registry(
            conversation, std::vector<AgentDefinition>{std::move(definition)});
        FAIL() << "Expected startup failure";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("Persona 'Alpha'"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("alpha-id"), std::string::npos);
    }
}

TEST(AgentRegistry, StartsOnConstructionAndStopsIdempotently) {
    Conversation conversation;
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::make_unique<ConfigurableBackend>());
    AgentRegistry registry(conversation, std::move(backends));

    ASSERT_TRUE(registry.submit(
        request(conversation, 1, "assistant", "Fake", "Question")));
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry)).request_id, 1U);
    registry.stop();
    EXPECT_NO_THROW(registry.stop());
    EXPECT_FALSE(registry.submit(
        request(
            conversation,
            2,
            "assistant",
            "Fake",
            "Another question")));
    AgentEvent event = AgentCompleted{};
    EXPECT_EQ(registry.try_receive(event), ChannelReadStatus::closed);
}

TEST(AgentRegistry, MapsCompletionDeltasAndSuccessToIdentifiedEvents) {
    Conversation conversation;
    conversation.add_entry(
        make_human_entry(
            1, "assistant", "Fake assistant", "Earlier question", 3));
    auto backend = std::make_unique<ConfigurableBackend>(
        CompletionResult{}, std::vector<std::string>{"Hello", " world"});
    ConfigurableBackend* backend_view = backend.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(backend));
    AgentRegistry registry(conversation, std::move(backends));

    ASSERT_TRUE(registry.submit(
        request(
            conversation,
            10,
            "assistant",
            "Fake",
            "Current question")));
    const AgentDelta first = std::get<AgentDelta>(next_event(registry));
    const AgentDelta second = std::get<AgentDelta>(next_event(registry));
    EXPECT_EQ(first.request_id, 10U);
    EXPECT_EQ(first.text, "Hello");
    EXPECT_EQ(second.request_id, 10U);
    EXPECT_EQ(second.text, " world");
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry)).request_id, 10U);

    ASSERT_EQ(backend_view->requests.size(), 1U);
    EXPECT_EQ(
        backend_view->requests.front().prompt.text, "Current question");
    EXPECT_EQ(
        backend_view->latest_prompts.front(),
        backend_view->requests.front().prompt);
    EXPECT_EQ(registry.roster().first().model, "fake-model");
}

TEST(AgentRegistry, MapsCompletionFailureToAgentFailed) {
    Conversation conversation;
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::make_unique<ConfigurableBackend>(
        CompletionResult{
            CompletionOutcome::protocol_error, "malformed response"}));
    AgentRegistry registry(conversation, std::move(backends));

    ASSERT_TRUE(registry.submit(
        request(conversation, 12, "assistant", "Fake", "Question")));
    const AgentFailed failed = std::get<AgentFailed>(next_event(registry));
    EXPECT_EQ(failed.request_id, 12U);
    EXPECT_EQ(failed.message, "malformed response");
}

TEST(AgentRegistry, CancelsTheActiveRequestWithTheSharedToken) {
    Conversation conversation;
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::make_unique<ConfigurableBackend>(
        CompletionResult{},
        std::vector<std::string>{"Partial"},
        true));
    AgentRegistry registry(conversation, std::move(backends));

    ASSERT_TRUE(registry.submit(
        request(conversation, 13, "assistant", "Fake", "Question")));
    EXPECT_EQ(std::get<AgentDelta>(next_event(registry)).text, "Partial");
    registry.cancel();
    EXPECT_EQ(std::get<AgentCancelled>(next_event(registry)).request_id, 13U);
}

TEST(AgentRegistry, CancelsBeforePrepareWithoutCallingTheBackend) {
    Conversation conversation;
    auto backend = std::make_unique<RegistryBackend>("assistant", "Fake");
    RegistryBackend* backend_view = backend.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(backend));
    AgentRegistry registry(conversation, std::move(backends));
    CompletionRequest pending =
        request(conversation, 14, "assistant", "Fake", "Question");

    {
        ConversationReadView queue_barrier = conversation.read();
        (void)queue_barrier;
        ASSERT_TRUE(registry.submit(std::move(pending)));
        registry.cancel();
    }

    EXPECT_EQ(std::get<AgentCancelled>(next_event(registry)).request_id, 14U);
    EXPECT_TRUE(backend_view->prepared_requests.empty());
    EXPECT_FALSE(backend_view->performed);
}

TEST(AgentRegistry, StopCancelsJoinsAndLeavesTheTerminalEventDrainable) {
    Conversation conversation;
    std::atomic_bool release_after_cancellation{false};
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::make_unique<ConfigurableBackend>(
        CompletionResult{},
        std::vector<std::string>{"Partial"},
        true,
        &release_after_cancellation));
    AgentRegistry registry(conversation, std::move(backends));

    ASSERT_TRUE(registry.submit(
        request(conversation, 15, "assistant", "Fake", "Question")));
    EXPECT_EQ(std::get<AgentDelta>(next_event(registry)).text, "Partial");
    // Use another thread only to prove stop() waits for the active backend.
    // No registry control operation runs concurrently with this call.
    std::future<void> stopped = std::async(
        std::launch::async, [&registry] { registry.stop(); });
    EXPECT_EQ(
        stopped.wait_for(std::chrono::milliseconds(50)),
        std::future_status::timeout);
    release_after_cancellation.store(true, std::memory_order_release);
    stopped.get();

    AgentEvent event = AgentCompleted{};
    EXPECT_EQ(registry.try_receive(event), ChannelReadStatus::value);
    EXPECT_EQ(std::get<AgentCancelled>(event).request_id, 15U);
    EXPECT_EQ(registry.try_receive(event), ChannelReadStatus::closed);
}

TEST(AgentRegistry, ReleasesTheConversationViewBeforePerforming) {
    Conversation conversation;
    auto backend = std::make_unique<BoundaryBackend>();
    BoundaryBackend* backend_view = backend.get();
    std::future<void> prepared = backend_view->prepared.get_future();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(backend));
    AgentRegistry registry(conversation, std::move(backends));

    ASSERT_TRUE(registry.submit(
        request(conversation, 21, "assistant", "Boundary", "Question")));
    ASSERT_EQ(
        prepared.wait_for(std::chrono::seconds(1)),
        std::future_status::ready);
    std::future<void> mutation = std::async(
        std::launch::async, [&conversation] { conversation.clear(); });
    EXPECT_EQ(
        mutation.wait_for(std::chrono::milliseconds(100)),
        std::future_status::ready);
    mutation.get();

    backend_view->release.store(true, std::memory_order_release);
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry)).request_id, 21U);
}

TEST(AgentRegistry, FailingPreparationReleasesTheViewAndDoesNotPerform) {
    Conversation conversation;
    auto backend = std::make_unique<ThrowingPrepareBackend>();
    ThrowingPrepareBackend* backend_view = backend.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(backend));
    AgentRegistry registry(conversation, std::move(backends));

    ASSERT_TRUE(registry.submit(
        request(conversation, 22, "assistant", "Throwing", "Question")));
    const AgentFailed failed = std::get<AgentFailed>(next_event(registry));
    EXPECT_EQ(failed.request_id, 22U);
    EXPECT_NE(
        failed.message.find("preparation failed"), std::string::npos);
    EXPECT_FALSE(backend_view->performed.load(std::memory_order_acquire));

    EXPECT_NO_THROW(conversation.clear());
    AgentEvent no_second_event = AgentCompleted{};
    EXPECT_EQ(
        registry.try_receive(no_second_event), ChannelReadStatus::empty);
}

} // namespace
} // namespace cha
