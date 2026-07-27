#include "agents/agent_registry.h"
#include "support/test_notifier.h"

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

// The first request emits no deltas, so its first wake is the terminal event.
// Block that wake after publish_terminal() clears the outstanding gate but
// before the worker can pop again. This creates a deterministic window to
// enqueue and cancel a second request before its prepare() check.
class BlockFirstWakeNotifier final : public WakeNotifier {
public:
    void wake() noexcept override {
        if (!blocked_.exchange(true, std::memory_order_acq_rel)) {
            entered.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
    }

    std::atomic_bool entered{false};
    std::atomic_bool release{false};

private:
    std::atomic_bool blocked_{false};
};

class RegistryBackend final : public CompletionBackend {
public:
    RegistryBackend(std::string id, std::string name)
        : id_(std::move(id)), name_(std::move(name)) {
    }

    RequestPayload prepare(const CompletionInput& input) override {
        prepared_requests.push_back(input.run.request_id);
        return {.bytes = input.run.prompt_text};
    }

    CompletionResult perform(
        RequestPayload payload,
        const CompletionDeltaSink& on_delta,
        const std::atomic_bool&) override {
        performed = true;
        on_delta({
            CompletionDeltaKind::answer,
            name_ + ":" + payload.bytes,
        });
        return {};
    }

    AgentRuntimeInfo info() const override {
        return {{id_, name_}, "model", "test://completion", true};
    }

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

    RequestPayload prepare(const CompletionInput& input) override {
        prepared.store(true, std::memory_order_release);
        return {.bytes = input.run.prompt_text};
    }

    CompletionResult perform(
        RequestPayload payload,
        const CompletionDeltaSink& on_delta,
        const std::atomic_bool& cancellation) override {
        entered_perform.store(true, std::memory_order_release);
        on_delta({
            CompletionDeltaKind::answer,
            name_ + ":" + payload.bytes,
        });
        while (!release.load(std::memory_order_acquire)
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

    RequestPayload prepare(const CompletionInput& input) override {
        inputs.push_back(input);
        histories.push_back(input.history->entries);
        return {.bytes = input.run.prompt_text};
    }

    CompletionResult perform(
        RequestPayload,
        const CompletionDeltaSink& on_delta,
        const std::atomic_bool& cancellation) override {
        perform_calls.fetch_add(1, std::memory_order_relaxed);
        for (const std::string& delta : deltas_) {
            on_delta({delta_kind, delta});
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

    AgentRuntimeInfo info() const override {
        return {
            .persona = {
                .id = id_,
                .name = "Fake",
            },
            .model = "fake-model",
            .api = "fake://completion",
            .streaming = true,
        };
    }

    std::vector<CompletionInput> inputs;
    std::vector<std::vector<TranscriptEntry>> histories;
    std::atomic_size_t perform_calls{};
    CompletionDeltaKind delta_kind{CompletionDeltaKind::answer};

private:
    std::string id_{"assistant"};
    CompletionResult result_;
    std::vector<std::string> deltas_;
    bool wait_for_cancellation_{};
    const std::atomic_bool* release_after_cancellation_{};
};

class BoundaryBackend final : public CompletionBackend {
public:
    RequestPayload prepare(const CompletionInput& input) override {
        captured_history = input.history->entries;
        prepared.set_value();
        return {.bytes = input.run.prompt_text};
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

    AgentRuntimeInfo info() const override {
        return {
            .persona = {
                .id = id_,
                .name = "Boundary",
            },
            .model = "fake",
            .api = "fake://",
            .streaming = true,
        };
    }

    std::promise<void> prepared;
    std::atomic_bool release{false};
    std::vector<TranscriptEntry> captured_history;

private:
    std::string id_{"assistant"};
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
        performed.store(true, std::memory_order_release);
        return {};
    }

    AgentRuntimeInfo info() const override {
        return {
            .persona = {
                .id = id_,
                .name = "Throwing",
            },
            .model = "fake",
            .api = "fake://",
            .streaming = true,
        };
    }

    std::atomic_bool performed{false};

private:
    std::string id_{"assistant"};
};

test::TestNotifier& notifier() {
    static test::TestNotifier instance;
    return instance;
}

AgentEvent next_event(AgentRegistry& registry) {
    while (true) {
        const std::size_t observed = notifier().wake_count();
        AgentEvent event = AgentCompleted{};
        const ChannelReadStatus status = registry.try_receive(event);
        if (status == ChannelReadStatus::value) {
            return event;
        }
        if (status == ChannelReadStatus::closed) {
            throw std::runtime_error(
                "Registry event queue closed unexpectedly");
        }
        if (!notifier().wait_for_wake(observed)) {
            throw std::runtime_error("Timed out waiting for registry event");
        }
    }
}

CompletionInput request(
    const Transcript& transcript,
    RequestId id,
    std::string target,
    std::string name,
    std::string text) {
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

TEST(AgentRegistry, RoutesPromptTargetsToTheMatchingBackendAndSharesOneChannel) {
    Transcript transcript;
    auto alpha = std::make_unique<RegistryBackend>("alpha-id", "Alpha");
    auto beta = std::make_unique<RegistryBackend>("beta-id", "Beta");
    RegistryBackend* alpha_view = alpha.get();
    RegistryBackend* beta_view = beta.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(alpha));
    backends.push_back(std::move(beta));
    AgentRegistry registry(std::move(backends), notifier());

    ASSERT_TRUE(registry.submit(request(transcript, 1, "beta-id", "Beta", "hello")));
    EXPECT_EQ(std::get<AgentDelta>(next_event(registry)).text, "Beta:hello");
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry)).request_id, 1U);
    EXPECT_TRUE(alpha_view->prepared_requests.empty());
    EXPECT_EQ(beta_view->prepared_requests, (std::vector<RequestId>{1}));

    CompletionInput unknown = request(transcript, 2, "missing-id", "Missing", "nope");
    EXPECT_FALSE(registry.submit(std::move(unknown)));
    registry.stop();
    AgentEvent event = AgentCompleted{};
    EXPECT_EQ(registry.try_receive(event), ChannelReadStatus::closed);
}

TEST(AgentRegistry, RejectsMissingHistoryBeforeClaimingTheOutstandingGate) {
    Transcript transcript;
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::make_unique<ConfigurableBackend>());
    AgentRegistry registry(std::move(backends), notifier());
    CompletionInput invalid =
        request(transcript, 1, "assistant", "Fake", "Invalid");
    invalid.history.reset();

    EXPECT_THROW(
        (void)registry.submit(std::move(invalid)),
        std::invalid_argument);
    ASSERT_TRUE(registry.submit(
        request(transcript, 2, "assistant", "Fake", "Valid")));
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry)).request_id, 2U);
}

TEST(AgentRegistry, RejectsInvalidBackendMetadataAtTheRegistryBoundary) {
    Transcript transcript;
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::make_unique<RegistryBackend>("bad-id", " Bad name"));
    EXPECT_THROW(
        AgentRegistry registry(
            std::move(backends),
            notifier()),
        std::invalid_argument);
}

TEST(AgentRegistry, SerializesRequestsAcrossDifferentBackends) {
    Transcript transcript;
    auto alpha = std::make_unique<BlockingRegistryBackend>("alpha-id", "Alpha");
    auto beta = std::make_unique<BlockingRegistryBackend>("beta-id", "Beta");
    BlockingRegistryBackend* alpha_view = alpha.get();
    BlockingRegistryBackend* beta_view = beta.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(alpha));
    backends.push_back(std::move(beta));
    AgentRegistry registry(std::move(backends), notifier());

    ASSERT_TRUE(registry.submit(request(transcript, 1, "alpha-id", "Alpha", "one")));
    EXPECT_EQ(std::get<AgentDelta>(next_event(registry)).text, "Alpha:one");
    ASSERT_TRUE(alpha_view->entered_perform.load(std::memory_order_acquire));

    EXPECT_FALSE(registry.submit(request(transcript, 2, "beta-id", "Beta", "two")));
    EXPECT_FALSE(beta_view->prepared.load(std::memory_order_acquire));
    EXPECT_FALSE(beta_view->entered_perform.load(std::memory_order_acquire));

    alpha_view->release.store(true, std::memory_order_release);
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry)).request_id, 1U);

    ASSERT_TRUE(registry.submit(request(transcript, 3, "beta-id", "Beta", "three")));
    EXPECT_EQ(std::get<AgentDelta>(next_event(registry)).text, "Beta:three");
    ASSERT_TRUE(beta_view->entered_perform.load(std::memory_order_acquire));
    beta_view->release.store(true, std::memory_order_release);
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry)).request_id, 3U);

    registry.stop();
    AgentEvent event = AgentCompleted{};
    EXPECT_EQ(registry.try_receive(event), ChannelReadStatus::closed);
}

TEST(AgentRegistry, RejectsEmptyConstruction) {
    Transcript transcript;
    EXPECT_THROW(
        AgentRegistry registry(
            std::vector<std::unique_ptr<CompletionBackend>>{},
            notifier()),
        std::invalid_argument);
}

TEST(AgentRegistry, RejectsNullBackend) {
    Transcript transcript;
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(nullptr);
    EXPECT_THROW(
        AgentRegistry registry(
            std::move(backends),
            notifier()),
        std::invalid_argument);
}

TEST(AgentRegistry, IdentifiesThePersonaWhoseStartupFails) {
    Transcript transcript;
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
            std::vector<AgentDefinition>{std::move(definition)},
            notifier());
        FAIL() << "Expected startup failure";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("Persona 'Alpha'"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("alpha-id"), std::string::npos);
    }
}

TEST(AgentRegistry, StartsOnConstructionAndStopsIdempotently) {
    Transcript transcript;
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::make_unique<ConfigurableBackend>());
    AgentRegistry registry(std::move(backends), notifier());

    ASSERT_TRUE(registry.submit(
        request(transcript, 1, "assistant", "Fake", "Question")));
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry)).request_id, 1U);
    registry.stop();
    EXPECT_NO_THROW(registry.stop());
    EXPECT_FALSE(registry.submit(
        request(
            transcript,
            2,
            "assistant",
            "Fake",
            "Another question")));
    AgentEvent event = AgentCompleted{};
    EXPECT_EQ(registry.try_receive(event), ChannelReadStatus::closed);
}

TEST(AgentRegistry, MapsCompletionDeltasAndSuccessToIdentifiedEvents) {
    Transcript transcript;
    transcript.add_entry(
        make_human_entry(
            1, "assistant", "Fake assistant", "Earlier question", 3));
    auto backend = std::make_unique<ConfigurableBackend>(
        CompletionResult{}, std::vector<std::string>{"Hello", " world"});
    ConfigurableBackend* backend_view = backend.get();
    backend_view->delta_kind = CompletionDeltaKind::reasoning;
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(backend));
    AgentRegistry registry(std::move(backends), notifier());

    ASSERT_TRUE(registry.submit(
        request(
            transcript,
            10,
            "assistant",
            "Fake",
            "Current question")));
    const AgentDelta first = std::get<AgentDelta>(next_event(registry));
    const AgentDelta second = std::get<AgentDelta>(next_event(registry));
    EXPECT_EQ(first.request_id, 10U);
    EXPECT_EQ(first.kind, CompletionDeltaKind::reasoning);
    EXPECT_EQ(first.text, "Hello");
    EXPECT_EQ(second.request_id, 10U);
    EXPECT_EQ(second.kind, CompletionDeltaKind::reasoning);
    EXPECT_EQ(second.text, " world");
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry)).request_id, 10U);

    ASSERT_EQ(backend_view->inputs.size(), 1U);
    EXPECT_EQ(
        backend_view->inputs.front().run.prompt_text, "Current question");
    ASSERT_EQ(backend_view->histories.front().size(), 1U);
    EXPECT_EQ(backend_view->histories.front().front().text, "Earlier question");
    EXPECT_EQ(registry.runtime_info().front().model, "fake-model");
}

TEST(AgentRegistry, MapsCompletionFailureToAgentFailed) {
    Transcript transcript;
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::make_unique<ConfigurableBackend>(
        CompletionResult{
            CompletionOutcome::protocol_error, "malformed response"}));
    AgentRegistry registry(std::move(backends), notifier());

    ASSERT_TRUE(registry.submit(
        request(transcript, 12, "assistant", "Fake", "Question")));
    const AgentFailed failed = std::get<AgentFailed>(next_event(registry));
    EXPECT_EQ(failed.request_id, 12U);
    EXPECT_EQ(failed.message, "malformed response");
}

TEST(AgentRegistry, CancelsTheActiveRequestWithTheSharedToken) {
    Transcript transcript;
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::make_unique<ConfigurableBackend>(
        CompletionResult{},
        std::vector<std::string>{"Partial"},
        true));
    AgentRegistry registry(std::move(backends), notifier());

    ASSERT_TRUE(registry.submit(
        request(transcript, 13, "assistant", "Fake", "Question")));
    EXPECT_EQ(std::get<AgentDelta>(next_event(registry)).text, "Partial");
    registry.cancel();
    EXPECT_EQ(std::get<AgentCancelled>(next_event(registry)).request_id, 13U);
}

TEST(AgentRegistry, CancellationBeforePreparationSkipsBothBackendMethods) {
    Transcript transcript;
    BlockFirstWakeNotifier blocking_notifier;
    auto backend = std::make_unique<ConfigurableBackend>(
        CompletionResult{},
        std::vector<std::string>{});
    ConfigurableBackend* backend_view = backend.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(backend));
    AgentRegistry registry(std::move(backends), blocking_notifier);

    ASSERT_TRUE(registry.submit(
        request(transcript, 14, "assistant", "Fake", "First")));
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!blocking_notifier.entered.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    if (!blocking_notifier.entered.load(std::memory_order_acquire)) {
        blocking_notifier.release.store(true, std::memory_order_release);
        FAIL() << "Timed out waiting for the first terminal notification";
    }

    const bool second_accepted = registry.submit(
        request(transcript, 15, "assistant", "Fake", "Second"));
    if (!second_accepted) {
        blocking_notifier.release.store(true, std::memory_order_release);
    }
    ASSERT_TRUE(second_accepted);
    registry.cancel();
    blocking_notifier.release.store(true, std::memory_order_release);

    AgentEvent first = AgentCompleted{};
    ASSERT_EQ(registry.try_receive(first), ChannelReadStatus::value);
    EXPECT_EQ(std::get<AgentCompleted>(first).request_id, 14U);

    AgentEvent second = AgentCompleted{};
    ChannelReadStatus second_status = ChannelReadStatus::empty;
    const auto second_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while ((second_status = registry.try_receive(second))
               == ChannelReadStatus::empty
           && std::chrono::steady_clock::now() < second_deadline) {
        std::this_thread::yield();
    }
    ASSERT_EQ(second_status, ChannelReadStatus::value);
    EXPECT_EQ(std::get<AgentCancelled>(second).request_id, 15U);
    ASSERT_EQ(backend_view->inputs.size(), 1U);
    EXPECT_EQ(backend_view->inputs.front().run.request_id, 14U);
    EXPECT_EQ(
        backend_view->perform_calls.load(std::memory_order_relaxed),
        1U);
    AgentEvent no_second_event = AgentCompleted{};
    EXPECT_EQ(registry.try_receive(no_second_event), ChannelReadStatus::empty);
}

TEST(AgentRegistry, StopCancelsJoinsAndLeavesTheTerminalEventDrainable) {
    Transcript transcript;
    std::atomic_bool release_after_cancellation{false};
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::make_unique<ConfigurableBackend>(
        CompletionResult{},
        std::vector<std::string>{"Partial"},
        true,
        &release_after_cancellation));
    AgentRegistry registry(std::move(backends), notifier());

    ASSERT_TRUE(registry.submit(
        request(transcript, 15, "assistant", "Fake", "Question")));
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

TEST(AgentRegistry, UsesImmutableHistoryWithoutHoldingTheTranscript) {
    Transcript transcript;
    transcript.add_entry(
        make_human_entry(1, "assistant", "Boundary", "Earlier", 20));
    auto backend = std::make_unique<BoundaryBackend>();
    BoundaryBackend* backend_view = backend.get();
    std::future<void> prepared = backend_view->prepared.get_future();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(backend));
    AgentRegistry registry(std::move(backends), notifier());

    ASSERT_TRUE(registry.submit(
        request(transcript, 21, "assistant", "Boundary", "Question")));
    ASSERT_EQ(
        prepared.wait_for(std::chrono::seconds(1)),
        std::future_status::ready);
    std::future<void> mutation = std::async(
        std::launch::async, [&transcript] { transcript.clear(); });
    EXPECT_EQ(
        mutation.wait_for(std::chrono::milliseconds(100)),
        std::future_status::ready);
    mutation.get();

    ASSERT_EQ(backend_view->captured_history.size(), 1U);
    EXPECT_EQ(backend_view->captured_history.front().text, "Earlier");
    backend_view->release.store(true, std::memory_order_release);
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry)).request_id, 21U);
}

TEST(AgentRegistry, FailingPreparationDoesNotPerform) {
    Transcript transcript;
    auto backend = std::make_unique<ThrowingPrepareBackend>();
    ThrowingPrepareBackend* backend_view = backend.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(backend));
    AgentRegistry registry(std::move(backends), notifier());

    ASSERT_TRUE(registry.submit(
        request(transcript, 22, "assistant", "Throwing", "Question")));
    const AgentFailed failed = std::get<AgentFailed>(next_event(registry));
    EXPECT_EQ(failed.request_id, 22U);
    EXPECT_NE(
        failed.message.find("preparation failed"), std::string::npos);
    EXPECT_FALSE(backend_view->performed.load(std::memory_order_acquire));

    AgentEvent no_second_event = AgentCompleted{};
    EXPECT_EQ(
        registry.try_receive(no_second_event), ChannelReadStatus::empty);
}

} // namespace
} // namespace cha
