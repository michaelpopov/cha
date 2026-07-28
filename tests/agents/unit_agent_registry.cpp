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

class AbortRegistryBackend final : public CompletionBackend {
public:
    AbortRegistryBackend(
        std::string id,
        std::string name,
        const std::atomic_bool* release_after_cancel = nullptr)
        : id_(std::move(id)),
          name_(std::move(name)),
          release_after_cancel_(release_after_cancel) {
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
        while (release_after_cancel_
               && !release_after_cancel_->load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return {CompletionOutcome::cancelled, {}};
    }

    AgentRuntimeInfo info() const override {
        return {{id_, name_}, "model", "test://completion", true};
    }

    std::atomic_bool entered{false};

private:
    std::string id_;
    std::string name_;
    const std::atomic_bool* release_after_cancel_{};
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

BatchId start_run(
    AgentRegistry& registry,
    CompletionInput input) {
    BatchId staged = registry.stage_batch(
        std::vector<CompletionInput>{std::move(input)});
    registry.set_foreground(staged, 0);
    registry.open_batch_gate(staged);
    return staged;
}

void retire_run(AgentRegistry& registry, BatchId staged) {
    registry.retire(staged, 0);
    registry.retire_batch(staged);
}

TEST(AgentRegistry, RoutesPromptTargetThroughTheForegroundQueue) {
    Transcript transcript;
    auto alpha = std::make_unique<RegistryBackend>("alpha-id", "Alpha");
    auto beta = std::make_unique<RegistryBackend>("beta-id", "Beta");
    RegistryBackend* alpha_view = alpha.get();
    RegistryBackend* beta_view = beta.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(alpha));
    backends.push_back(std::move(beta));
    AgentRegistry registry(std::move(backends), notifier());

    const BatchId staged = start_run(
        registry,
        request(transcript, 1, "beta-id", "Beta", "hello"));
    EXPECT_EQ(std::get<AgentDelta>(next_event(registry)).text, "Beta:hello");
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry)).request_id, 1U);
    retire_run(registry, staged);
    EXPECT_TRUE(alpha_view->prepared_requests.empty());
    EXPECT_EQ(beta_view->prepared_requests, (std::vector<RequestId>{1}));

    CompletionInput unknown = request(transcript, 2, "missing-id", "Missing", "nope");
    EXPECT_THROW(
        (void)start_run(registry, std::move(unknown)),
        std::invalid_argument);
    registry.stop();
    AgentEvent event = AgentCompleted{};
    EXPECT_EQ(registry.try_receive(event), ChannelReadStatus::closed);
}

TEST(AgentRegistry, RejectsMissingHistoryBeforeStaging) {
    Transcript transcript;
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::make_unique<ConfigurableBackend>());
    AgentRegistry registry(std::move(backends), notifier());
    CompletionInput invalid =
        request(transcript, 1, "assistant", "Fake", "Invalid");
    invalid.history.reset();

    EXPECT_THROW((void)start_run(registry, std::move(invalid)), std::invalid_argument);
    const BatchId staged = start_run(
        registry,
        request(transcript, 2, "assistant", "Fake", "Valid"));
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry)).request_id, 2U);
    retire_run(registry, staged);
}

TEST(AgentRegistry, RejectsDuplicateBatchTargetsBeforeAcquiringLeases) {
    Transcript transcript;
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::make_unique<RegistryBackend>("alpha-id", "Alpha"));
    AgentRegistry registry(std::move(backends), notifier());

    EXPECT_THROW(
        (void)registry.stage_batch({
            request(transcript, 1, "alpha-id", "Alpha", "one"),
            request(transcript, 2, "alpha-id", "Alpha", "two"),
        }),
        std::invalid_argument);

    const BatchId staged = start_run(
        registry,
        request(transcript, 3, "alpha-id", "Alpha", "ordinary"));
    EXPECT_EQ(std::get<AgentDelta>(next_event(registry)).request_id, 3U);
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry)).request_id, 3U);
    retire_run(registry, staged);
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

TEST(AgentRegistry, SerializesSingleRunBatchesAcrossBackends) {
    Transcript transcript;
    auto alpha = std::make_unique<BlockingRegistryBackend>("alpha-id", "Alpha");
    auto beta = std::make_unique<BlockingRegistryBackend>("beta-id", "Beta");
    BlockingRegistryBackend* alpha_view = alpha.get();
    BlockingRegistryBackend* beta_view = beta.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(alpha));
    backends.push_back(std::move(beta));
    AgentRegistry registry(std::move(backends), notifier());

    const BatchId first = start_run(
        registry,
        request(transcript, 1, "alpha-id", "Alpha", "one"));
    EXPECT_EQ(std::get<AgentDelta>(next_event(registry)).text, "Alpha:one");
    ASSERT_TRUE(alpha_view->entered_perform.load(std::memory_order_acquire));

    EXPECT_THROW(
        (void)start_run(
            registry,
            request(transcript, 2, "beta-id", "Beta", "two")),
        std::runtime_error);
    EXPECT_FALSE(beta_view->prepared.load(std::memory_order_acquire));
    EXPECT_FALSE(beta_view->entered_perform.load(std::memory_order_acquire));

    alpha_view->release.store(true, std::memory_order_release);
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry)).request_id, 1U);
    retire_run(registry, first);

    const BatchId second = start_run(
        registry,
        request(transcript, 3, "beta-id", "Beta", "three"));
    EXPECT_EQ(std::get<AgentDelta>(next_event(registry)).text, "Beta:three");
    ASSERT_TRUE(beta_view->entered_perform.load(std::memory_order_acquire));
    beta_view->release.store(true, std::memory_order_release);
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry)).request_id, 3U);
    retire_run(registry, second);

    registry.stop();
    AgentEvent event = AgentCompleted{};
    EXPECT_EQ(registry.try_receive(event), ChannelReadStatus::closed);
}

TEST(AgentRegistry, StagesDistinctBackendsConcurrentlyAndRoutesOnlyForeground) {
    Transcript transcript;
    auto alpha = std::make_unique<BlockingRegistryBackend>("alpha-id", "Alpha");
    auto beta = std::make_unique<BlockingRegistryBackend>("beta-id", "Beta");
    BlockingRegistryBackend* alpha_view = alpha.get();
    BlockingRegistryBackend* beta_view = beta.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(alpha));
    backends.push_back(std::move(beta));
    AgentRegistry registry(std::move(backends), notifier());

    BatchId staged = registry.stage_batch({
        request(transcript, 1, "alpha-id", "Alpha", "one"),
        request(transcript, 2, "beta-id", "Beta", "two"),
    });
    registry.set_foreground(staged, 0);
    registry.open_batch_gate(staged);
    registry.open_batch_gate(staged);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while ((!alpha_view->entered_perform.load(std::memory_order_acquire)
            || !beta_view->entered_perform.load(std::memory_order_acquire))
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ASSERT_TRUE(alpha_view->entered_perform.load(std::memory_order_acquire));
    ASSERT_TRUE(beta_view->entered_perform.load(std::memory_order_acquire));

    EXPECT_EQ(std::get<AgentDelta>(next_event(registry)).text, "Alpha:one");
    alpha_view->release.store(true, std::memory_order_release);
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry)).request_id, 1U);
    registry.retire(staged, 0);

    // The live batch remains the registry admission reservation after child 0
    // returns its lease and worker execution.
    EXPECT_THROW(
        (void)start_run(
            registry,
            request(transcript, 3, "alpha-id", "Alpha", "ordinary")),
        std::runtime_error);

    registry.set_foreground(staged, 1);
    EXPECT_EQ(std::get<AgentDelta>(next_event(registry)).text, "Beta:two");
    beta_view->release.store(true, std::memory_order_release);
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry)).request_id, 2U);
    registry.retire(staged, 1);
    registry.retire_batch(staged);

    const BatchId reused = start_run(
        registry,
        request(transcript, 4, "alpha-id", "Alpha", "reused"));
    EXPECT_EQ(std::get<AgentDelta>(next_event(registry)).text, "Alpha:reused");
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry)).request_id, 4U);
    retire_run(registry, reused);
}

TEST(AgentRegistry, RollsBackPartialThreadConstructionAndReleasesEveryLease) {
    for (const std::size_t fail_on_start : {1U, 2U}) {
        SCOPED_TRACE(fail_on_start);
        Transcript transcript;
        auto one = std::make_unique<RegistryBackend>("one-id", "One");
        auto two = std::make_unique<RegistryBackend>("two-id", "Two");
        auto three = std::make_unique<RegistryBackend>("three-id", "Three");
        RegistryBackend* const one_view = one.get();
        RegistryBackend* const two_view = two.get();
        RegistryBackend* const three_view = three.get();
        std::vector<std::unique_ptr<CompletionBackend>> backends;
        backends.push_back(std::move(one));
        backends.push_back(std::move(two));
        backends.push_back(std::move(three));
        std::size_t thread_starts{};
        AgentRegistry registry(
            std::move(backends),
            notifier(),
            [&thread_starts, fail_on_start] {
                if (++thread_starts == fail_on_start) {
                    throw std::runtime_error(
                        "injected staged-thread construction failure");
                }
            });

        EXPECT_THROW(
            (void)registry.stage_batch({
                request(transcript, 1, "one-id", "One", "one"),
                request(transcript, 2, "two-id", "Two", "two"),
                request(transcript, 3, "three-id", "Three", "three"),
            }),
            std::runtime_error);
        EXPECT_TRUE(one_view->prepared_requests.empty());
        EXPECT_TRUE(two_view->prepared_requests.empty());
        EXPECT_TRUE(three_view->prepared_requests.empty());
        EXPECT_FALSE(one_view->performed);
        EXPECT_FALSE(two_view->performed);
        EXPECT_FALSE(three_view->performed);

        BatchId staged = registry.stage_batch({
            request(transcript, 4, "one-id", "One", "one"),
            request(transcript, 5, "two-id", "Two", "two"),
            request(transcript, 6, "three-id", "Three", "three"),
        });
        registry.set_foreground(staged, 0);
        registry.open_batch_gate(staged);
        for (std::size_t index = 0; index < 3; ++index) {
            if (index != 0) {
                registry.set_foreground(staged, index);
            }
            EXPECT_TRUE(std::holds_alternative<AgentDelta>(
                next_event(registry)));
            EXPECT_TRUE(std::holds_alternative<AgentCompleted>(
                next_event(registry)));
            registry.retire(staged, index);
        }
        registry.retire_batch(staged);
    }
}

TEST(AgentRegistry, OrdinaryRunUsesOnlyThePersistentRunner) {
    Transcript transcript;
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(
        std::make_unique<RegistryBackend>("one-id", "One"));
    std::size_t temporary_thread_starts{};
    AgentRegistry registry(
        std::move(backends),
        notifier(),
        [&temporary_thread_starts] {
            ++temporary_thread_starts;
        });

    const BatchId batch = start_run(
        registry,
        request(transcript, 1, "one-id", "One", "ordinary"));
    EXPECT_EQ(temporary_thread_starts, 0U);
    EXPECT_TRUE(std::holds_alternative<AgentDelta>(
        next_event(registry)));
    EXPECT_TRUE(std::holds_alternative<AgentCompleted>(
        next_event(registry)));
    retire_run(registry, batch);
}

TEST(AgentRegistry, InteractiveAbortCleanupDoesNotJoinBackgroundOnCaller) {
    Transcript transcript;
    std::atomic_bool release_background{false};
    auto foreground =
        std::make_unique<AbortRegistryBackend>("one-id", "One");
    auto background = std::make_unique<AbortRegistryBackend>(
        "two-id", "Two", &release_background);
    AbortRegistryBackend* foreground_view = foreground.get();
    AbortRegistryBackend* background_view = background.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(foreground));
    backends.push_back(std::move(background));
    AgentRegistry registry(std::move(backends), notifier());

    BatchId staged = registry.stage_batch({
        request(transcript, 1, "one-id", "One", "one"),
        request(transcript, 2, "two-id", "Two", "two"),
    });
    registry.set_foreground(staged, 0);
    registry.open_batch_gate(staged);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while ((!foreground_view->entered.load(std::memory_order_acquire)
            || !background_view->entered.load(std::memory_order_acquire))
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ASSERT_TRUE(foreground_view->entered.load(std::memory_order_acquire));
    ASSERT_TRUE(background_view->entered.load(std::memory_order_acquire));

    const auto started = std::chrono::steady_clock::now();
    registry.begin_abort_cleanup(
        staged,
        0);
    EXPECT_LT(
        std::chrono::steady_clock::now() - started,
        std::chrono::milliseconds(50));
    EXPECT_EQ(
        registry.poll_abort_cleanup(staged),
        CleanupStatus::pending);

    EXPECT_EQ(std::get<AgentCancelled>(next_event(registry)).request_id, 1U);
    registry.release_abort_foreground(staged, 0);
    EXPECT_EQ(
        registry.poll_abort_cleanup(staged),
        CleanupStatus::pending);

    release_background.store(true, std::memory_order_release);
    CleanupStatus status = CleanupStatus::pending;
    const auto cleanup_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while ((status = registry.poll_abort_cleanup(staged))
               == CleanupStatus::pending
           && std::chrono::steady_clock::now() < cleanup_deadline) {
        std::this_thread::yield();
    }
    EXPECT_EQ(status, CleanupStatus::complete);
}

TEST(AgentRegistry, ShutdownDuringAbortStopsWaitingForForegroundHandoff) {
    Transcript transcript;
    std::atomic_bool release_background{false};
    auto foreground =
        std::make_unique<AbortRegistryBackend>("one-id", "One");
    auto background = std::make_unique<AbortRegistryBackend>(
        "two-id", "Two", &release_background);
    AbortRegistryBackend* foreground_view = foreground.get();
    AbortRegistryBackend* background_view = background.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(foreground));
    backends.push_back(std::move(background));
    AgentRegistry registry(std::move(backends), notifier());

    BatchId staged = registry.stage_batch({
        request(transcript, 1, "one-id", "One", "one"),
        request(transcript, 2, "two-id", "Two", "two"),
    });
    registry.set_foreground(staged, 0);
    registry.open_batch_gate(staged);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while ((!foreground_view->entered.load(std::memory_order_acquire)
            || !background_view->entered.load(std::memory_order_acquire))
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ASSERT_TRUE(foreground_view->entered.load(std::memory_order_acquire));
    ASSERT_TRUE(background_view->entered.load(std::memory_order_acquire));

    registry.begin_abort_cleanup(
        staged,
        0);
    std::future<void> stopped = std::async(
        std::launch::async, [&registry] { registry.stop(); });
    EXPECT_EQ(
        stopped.wait_for(std::chrono::milliseconds(50)),
        std::future_status::timeout);

    release_background.store(true, std::memory_order_release);
    stopped.get();

    AgentEvent event = AgentCompleted{};
    ASSERT_EQ(registry.try_receive(event), ChannelReadStatus::value);
    EXPECT_EQ(std::get<AgentCancelled>(event).request_id, 1U);
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

TEST(AgentRegistry, RejectsAnUnknownBatchOrRunPositionWithoutTerminating) {
    Transcript transcript;
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::make_unique<ConfigurableBackend>());
    AgentRegistry registry(std::move(backends), notifier());

    const BatchId staged = registry.stage_batch({
        request(transcript, 1, "assistant", "Fake", "Question"),
    });
    EXPECT_THROW(
        registry.set_foreground(staged + 1, 0),
        std::logic_error);
    EXPECT_THROW(
        registry.set_foreground(staged, 1),
        std::logic_error);

    registry.set_foreground(staged, 0);
    registry.open_batch_gate(staged);
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry)).request_id, 1U);
    retire_run(registry, staged);
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

    const BatchId staged = start_run(
        registry,
        request(transcript, 1, "assistant", "Fake", "Question"));
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry)).request_id, 1U);
    registry.stop();
    EXPECT_NO_THROW(registry.stop());
    EXPECT_THROW(
        (void)start_run(
            registry,
            request(
                transcript,
                2,
                "assistant",
                "Fake",
                "Another question")),
        std::runtime_error);
    (void)staged;
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

    const BatchId staged = start_run(
        registry,
        request(
            transcript,
            10,
            "assistant",
            "Fake",
            "Current question"));
    const AgentDelta first = std::get<AgentDelta>(next_event(registry));
    const AgentDelta second = std::get<AgentDelta>(next_event(registry));
    EXPECT_EQ(first.request_id, 10U);
    EXPECT_EQ(first.kind, CompletionDeltaKind::reasoning);
    EXPECT_EQ(first.text, "Hello");
    EXPECT_EQ(second.request_id, 10U);
    EXPECT_EQ(second.kind, CompletionDeltaKind::reasoning);
    EXPECT_EQ(second.text, " world");
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry)).request_id, 10U);
    retire_run(registry, staged);

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

    const BatchId staged = start_run(
        registry,
        request(transcript, 12, "assistant", "Fake", "Question"));
    const AgentFailed failed = std::get<AgentFailed>(next_event(registry));
    EXPECT_EQ(failed.request_id, 12U);
    EXPECT_EQ(failed.message, "malformed response");
    retire_run(registry, staged);
}

TEST(AgentRegistry, CancelsTheActiveRegularExecution) {
    Transcript transcript;
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::make_unique<ConfigurableBackend>(
        CompletionResult{},
        std::vector<std::string>{"Partial"},
        true));
    AgentRegistry registry(std::move(backends), notifier());

    const BatchId staged = start_run(
        registry,
        request(transcript, 13, "assistant", "Fake", "Question"));
    EXPECT_EQ(std::get<AgentDelta>(next_event(registry)).text, "Partial");
    registry.cancel_all();
    EXPECT_EQ(std::get<AgentCancelled>(next_event(registry)).request_id, 13U);
    retire_run(registry, staged);
}

TEST(AgentRegistry, ClearsRegularRunnerCancellationBeforeReuse) {
    Transcript transcript;
    auto backend =
        std::make_unique<BlockingRegistryBackend>("assistant", "Fake");
    BlockingRegistryBackend* backend_view = backend.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(backend));
    AgentRegistry registry(std::move(backends), notifier());

    const BatchId first = start_run(
        registry,
        request(transcript, 13, "assistant", "Fake", "First"));
    EXPECT_EQ(std::get<AgentDelta>(next_event(registry)).text, "Fake:First");
    registry.cancel_all();
    EXPECT_EQ(std::get<AgentCancelled>(next_event(registry)).request_id, 13U);
    retire_run(registry, first);

    const BatchId second = start_run(
        registry,
        request(transcript, 14, "assistant", "Fake", "Second"));
    EXPECT_EQ(std::get<AgentDelta>(next_event(registry)).text, "Fake:Second");
    backend_view->release.store(true, std::memory_order_release);
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry)).request_id, 14U);
    retire_run(registry, second);
}

TEST(AgentRegistry, CancellationBeforePreparationSkipsBothBackendMethods) {
    Transcript transcript;
    auto backend = std::make_unique<ConfigurableBackend>(
        CompletionResult{},
        std::vector<std::string>{});
    ConfigurableBackend* backend_view = backend.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(backend));
    AgentRegistry registry(std::move(backends), notifier());

    BatchId staged = registry.stage_batch(
        std::vector<CompletionInput>{
            request(transcript, 14, "assistant", "Fake", "Question"),
        });
    registry.set_foreground(staged, 0);
    registry.cancel_all();
    registry.open_batch_gate(staged);

    const AgentCancelled cancelled =
        std::get<AgentCancelled>(next_event(registry));
    EXPECT_EQ(cancelled.request_id, 14U);
    EXPECT_TRUE(backend_view->inputs.empty());
    EXPECT_EQ(
        backend_view->perform_calls.load(std::memory_order_relaxed),
        0U);
    registry.retire(staged, 0);
    registry.retire_batch(staged);
}

TEST(AgentRegistry, CancelledGatePublishesTerminalWithoutCallingBackend) {
    Transcript transcript;
    auto backend = std::make_unique<ConfigurableBackend>();
    ConfigurableBackend* backend_view = backend.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(backend));
    AgentRegistry registry(std::move(backends), notifier());

    const BatchId staged = registry.stage_batch({
        request(transcript, 14, "assistant", "Fake", "Question"),
    });
    registry.set_foreground(staged, 0);
    registry.stop();
    EXPECT_THROW(
        registry.set_foreground(staged, 0),
        std::logic_error);

    EXPECT_TRUE(backend_view->inputs.empty());
    EXPECT_EQ(
        backend_view->perform_calls.load(std::memory_order_relaxed),
        0U);
    AgentEvent event = AgentCompleted{};
    ASSERT_EQ(registry.try_receive(event), ChannelReadStatus::value);
    EXPECT_EQ(std::get<AgentCancelled>(event).request_id, 14U);
    EXPECT_EQ(registry.try_receive(event), ChannelReadStatus::closed);
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

    (void)start_run(
        registry,
        request(transcript, 15, "assistant", "Fake", "Question"));
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

TEST(AgentRegistry, UsesImmutableHistoryAfterTranscriptMutation) {
    Transcript transcript;
    transcript.add_entry(
        make_human_entry(1, "assistant", "Boundary", "Earlier", 20));
    auto backend = std::make_unique<BoundaryBackend>();
    BoundaryBackend* backend_view = backend.get();
    std::future<void> prepared = backend_view->prepared.get_future();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(backend));
    AgentRegistry registry(std::move(backends), notifier());

    const BatchId staged = start_run(
        registry,
        request(transcript, 21, "assistant", "Boundary", "Question"));
    ASSERT_EQ(
        prepared.wait_for(std::chrono::seconds(1)),
        std::future_status::ready);
    transcript.clear();

    ASSERT_EQ(backend_view->captured_history.size(), 1U);
    EXPECT_EQ(backend_view->captured_history.front().text, "Earlier");
    backend_view->release.store(true, std::memory_order_release);
    EXPECT_EQ(std::get<AgentCompleted>(next_event(registry)).request_id, 21U);
    retire_run(registry, staged);
}

TEST(AgentRegistry, FailingPreparationDoesNotPerform) {
    Transcript transcript;
    auto backend = std::make_unique<ThrowingPrepareBackend>();
    ThrowingPrepareBackend* backend_view = backend.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(backend));
    AgentRegistry registry(std::move(backends), notifier());

    const BatchId staged = start_run(
        registry,
        request(transcript, 22, "assistant", "Throwing", "Question"));
    const AgentFailed failed = std::get<AgentFailed>(next_event(registry));
    EXPECT_EQ(failed.request_id, 22U);
    EXPECT_NE(
        failed.message.find("preparation failed"), std::string::npos);
    EXPECT_FALSE(backend_view->performed.load(std::memory_order_acquire));

    AgentEvent no_second_event = AgentCompleted{};
    EXPECT_EQ(
        registry.try_receive(no_second_event), ChannelReadStatus::empty);
    retire_run(registry, staged);
}

} // namespace
} // namespace cha
