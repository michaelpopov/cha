#include "agent_registry.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <poll.h>
#include <set>
#include <stdexcept>
#include <string>
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
        on_delta(name_ + ":" + payload.bytes);
        return {};
    }

    AgentInfo info() const override {
        return {id_, name_, "model", "test://completion", true};
    }

    const std::string& agent_id() const override { return id_; }

    std::vector<RequestId> prepared_requests;

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
    std::atomic_bool release{false};

private:
    std::string id_;
    std::string name_;
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

TEST(AgentRegistry, DrainsConcurrentWorkerEventsBeforeChannelClosure) {
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

    ASSERT_TRUE(registry.submit(request(conversation, 2, "beta-id", "Beta", "two")));
    EXPECT_EQ(std::get<AgentDelta>(next_event(registry)).text, "Beta:two");
    ASSERT_TRUE(beta_view->entered_perform.load(std::memory_order_acquire));

    alpha_view->release.store(true, std::memory_order_release);
    beta_view->release.store(true, std::memory_order_release);
    std::set<RequestId> completed;
    completed.insert(std::get<AgentCompleted>(next_event(registry)).request_id);
    completed.insert(std::get<AgentCompleted>(next_event(registry)).request_id);
    EXPECT_EQ(completed, (std::set<RequestId>{1, 2}));

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

} // namespace
} // namespace cha
