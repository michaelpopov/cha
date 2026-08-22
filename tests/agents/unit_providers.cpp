#include "agents/providers.h"
#include "support/test_generations.h"
#include "support/test_notifier.h"
#include "util/environment.h"
#include "util/logging.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace cha {
namespace {

using namespace std::chrono_literals;

class ProviderDiagnosticLog {
public:
    ProviderDiagnosticLog()
        : directory_(std::filesystem::temp_directory_path()
            / ("cha_provider_logging_"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()))),
          path_(directory_ / "cha.log") {
        shutdown_diagnostic_logging();
        initialize_diagnostic_logging(path_, "debug");
    }

    ~ProviderDiagnosticLog() {
        shutdown_diagnostic_logging();
        std::filesystem::remove_all(directory_);
    }

    std::string contents() const {
        std::ifstream file(path_);
        return {
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
    }

private:
    std::filesystem::path directory_;
    std::filesystem::path path_;
};

struct BackendState {
    std::mutex mutex;
    std::condition_variable changed;
    int constructed{};
    int prepared{};
    int performing{};
    int destroyed{};
    bool block{};
    bool release{};
    bool throw_prepare{};
    GenerationResult result{};
    std::vector<GenerationDelta> deltas;
    std::vector<SharedCharacterDefinition> definitions;
};

class TestBackend final : public ModelBackend {
public:
    explicit TestBackend(std::shared_ptr<BackendState> state)
        : state_(std::move(state)) {
        std::lock_guard lock(state_->mutex);
        ++state_->constructed;
        state_->changed.notify_all();
    }

    ~TestBackend() override {
        std::lock_guard lock(state_->mutex);
        ++state_->destroyed;
        state_->changed.notify_all();
    }

    RequestPayload prepare(const GenerationRequest& input) override {
        std::lock_guard lock(state_->mutex);
        ++state_->prepared;
        state_->changed.notify_all();
        if (state_->throw_prepare) throw std::runtime_error("prepare failed");
        return {.bytes = input.run.prompt_text};
    }

    GenerationResult perform(
        RequestPayload,
        const GenerationDeltaSink& on_delta,
        const std::atomic_bool& cancellation) override {
        std::vector<GenerationDelta> deltas;
        {
            std::lock_guard lock(state_->mutex);
            ++state_->performing;
            deltas = state_->deltas;
            state_->changed.notify_all();
        }
        for (GenerationDelta& delta : deltas) on_delta(std::move(delta));

        std::unique_lock lock(state_->mutex);
        while (state_->block && !state_->release
               && !cancellation.load(std::memory_order_acquire)) {
            state_->changed.wait_for(lock, 1ms);
        }
        return cancellation.load(std::memory_order_acquire)
            ? GenerationResult{GenerationOutcome::cancelled, {}}
            : state_->result;
    }

private:
    std::shared_ptr<BackendState> state_;
};

bool wait_for(
    const std::shared_ptr<BackendState>& state,
    const std::function<bool(const BackendState&)>& predicate) {
    std::unique_lock lock(state->mutex);
    return state->changed.wait_for(lock, 1s, [&] { return predicate(*state); });
}

template <typename T>
bool wait_for_expired(const std::weak_ptr<T>& weak) {
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (weak.expired()) return true;
        std::this_thread::yield();
    }
    return weak.expired();
}

class ScopedEnvironmentVariable {
public:
    explicit ScopedEnvironmentVariable(std::string name) : name_(std::move(name)) {
        if (const char* value = std::getenv(name_.c_str())) previous_ = value;
    }

    ~ScopedEnvironmentVariable() {
        if (previous_) {
            (void)set_environment_variable(name_, *previous_);
        } else {
            (void)unset_environment_variable(name_);
        }
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
};

void release(const std::shared_ptr<BackendState>& state) {
    {
        std::lock_guard lock(state->mutex);
        state->release = true;
    }
    state->changed.notify_all();
}

SharedCharacterDefinition definition(
    std::string id = "test-id",
    std::string model = "test-model") {
    return std::make_shared<const CharacterDefinition>(CharacterDefinition{
        .character = {.id = std::move(id), .display_name = "Test"},
        .provider = {.id = "test-provider", .config = {.model = std::move(model)}},
        .system_prompt = "Test prompt",
    });
}

ProviderRequestInput input(
    const SharedCharacterDefinition& character,
    RequestId request_id) {
    Transcript transcript;
    ProviderRequestInput result{
        .character = character,
        .generation = test::generation_request(
            transcript,
            request_id,
            character->character.id,
            character->character.display_name),
    };
    result.generation.run.session = {"test-forum", "test-session"};
    return result;
}

std::vector<GenerationEvent> receive_terminal(
    const std::shared_ptr<ProviderRequest>& request) {
    std::vector<GenerationEvent> events;
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline) {
        GenerationEvent event = GenerationCompleted{};
        if (request->try_receive(event) == ChannelReadStatus::value) {
            const bool terminal = !std::holds_alternative<GenerationEventDelta>(event);
            events.push_back(std::move(event));
            if (terminal) return events;
        }
        std::this_thread::yield();
    }
    throw std::runtime_error("Timed out waiting for provider request terminal event");
}

ProviderClientFactory factory(const std::shared_ptr<BackendState>& state) {
    return [state](SharedCharacterDefinition character) {
        {
            std::lock_guard lock(state->mutex);
            state->definitions.push_back(std::move(character));
        }
        return std::make_unique<TestBackend>(state);
    };
}

TEST(Providers, StartsEveryRequestImmediatelyWithIndependentBackendsAndQueues) {
    auto state = std::make_shared<BackendState>();
    state->block = true;
    Providers providers(factory(state));
    auto notifier = std::make_shared<test::TestNotifier>();
    const SharedCharacterDefinition character = definition();

    auto first = providers.make_request(input(character, 1), notifier);
    auto second = providers.make_request(input(character, 2), notifier);
    EXPECT_TRUE(wait_for(state, [](const BackendState& value) {
        return value.performing == 2;
    }));

    first->cancel();
    EXPECT_TRUE(std::holds_alternative<GenerationCancelled>(
        receive_terminal(first).back()));
    {
        std::lock_guard lock(state->mutex);
        EXPECT_EQ(state->constructed, 2);
        ASSERT_EQ(state->definitions.size(), 2U);
        EXPECT_EQ(state->definitions[0], character);
        EXPECT_EQ(state->definitions[1], character);
    }

    release(state);
    EXPECT_TRUE(std::holds_alternative<GenerationCompleted>(
        receive_terminal(second).back()));
    providers.shutdown();
}

TEST(Providers, PublishesOrderedDeltasThenExactlyOneTerminal) {
    auto state = std::make_shared<BackendState>();
    state->deltas = {
        {GenerationDeltaKind::reasoning, "Think"},
        {GenerationDeltaKind::answer, "Answer"},
    };
    Providers providers(factory(state));
    auto request = providers.make_request(
        input(definition(), 7), std::make_shared<test::NoopNotifier>());

    const std::vector<GenerationEvent> events = receive_terminal(request);
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(std::get<GenerationEventDelta>(events[0]).text, "Think");
    EXPECT_EQ(std::get<GenerationEventDelta>(events[1]).text, "Answer");
    EXPECT_TRUE(std::holds_alternative<GenerationCompleted>(events[2]));
    GenerationEvent extra = GenerationCompleted{};
    EXPECT_EQ(request->try_receive(extra), ChannelReadStatus::closed);
    providers.shutdown();
}

TEST(Providers, LifecycleLogsIncludeForumAndSessionIdentity) {
    ProviderDiagnosticLog log;
    auto state = std::make_shared<BackendState>();
    Providers providers(factory(state));
    auto request = providers.make_request(
        input(definition(), 17), std::make_shared<test::NoopNotifier>());

    EXPECT_TRUE(std::holds_alternative<GenerationCompleted>(
        receive_terminal(request).back()));
    providers.shutdown();

    const std::string output = log.contents();
    for (const std::string_view event : {
             "Provider request admitted:",
             "Provider request started:",
             "Provider request completed:",
             "Provider request unregistering:",
         }) {
        const std::size_t line = output.find(event);
        ASSERT_NE(line, std::string::npos) << event;
        const std::size_t end = output.find('\n', line);
        const std::size_t length = end == std::string::npos
            ? output.size() - line
            : end - line;
        const std::string_view fields(output.data() + line, length);
        EXPECT_NE(fields.find("forum_id=test-forum"), std::string_view::npos);
        EXPECT_NE(fields.find("session_id=test-session"), std::string_view::npos);
        EXPECT_NE(fields.find("request_id=17"), std::string_view::npos);
    }
}

TEST(Providers, MapsBackendFailuresAndExceptionsToFailedTerminals) {
    auto failed = std::make_shared<BackendState>();
    failed->result = {GenerationOutcome::transport_error, "unavailable"};
    Providers failed_providers(factory(failed));
    auto failed_request = failed_providers.make_request(
        input(definition(), 1), std::make_shared<test::NoopNotifier>());
    const GenerationFailed failure = std::get<GenerationFailed>(
        receive_terminal(failed_request).back());
    EXPECT_EQ(failure.message, "unavailable");
    failed_providers.shutdown();

    auto throwing = std::make_shared<BackendState>();
    throwing->throw_prepare = true;
    Providers throwing_providers(factory(throwing));
    auto throwing_request = throwing_providers.make_request(
        input(definition(), 2), std::make_shared<test::NoopNotifier>());
    const GenerationFailed exception = std::get<GenerationFailed>(
        receive_terminal(throwing_request).back());
    EXPECT_EQ(exception.message, "prepare failed");
    throwing_providers.shutdown();
}

TEST(Providers, CancellingBeforeWorkerExecutionSkipsClientConstruction) {
    struct LaunchGate {
        std::mutex mutex;
        std::condition_variable changed;
        bool open{};
    };
    auto gate = std::make_shared<LaunchGate>();
    auto state = std::make_shared<BackendState>();
    Providers providers(factory(state), [gate](std::function<void()> worker) {
        std::thread([gate, worker = std::move(worker)]() mutable {
            std::unique_lock lock(gate->mutex);
            gate->changed.wait(lock, [&] { return gate->open; });
            lock.unlock();
            worker();
        }).detach();
    });
    auto request = providers.make_request(
        input(definition(), 1), std::make_shared<test::NoopNotifier>());
    request->cancel();
    {
        std::lock_guard lock(gate->mutex);
        gate->open = true;
    }
    gate->changed.notify_all();

    EXPECT_TRUE(std::holds_alternative<GenerationCancelled>(
        receive_terminal(request).back()));
    std::lock_guard lock(state->mutex);
    EXPECT_EQ(state->constructed, 0);
    providers.shutdown();
}

TEST(Providers, CancellingDuringPerformProducesOneCancelledTerminal) {
    auto state = std::make_shared<BackendState>();
    state->block = true;
    Providers providers(factory(state));
    auto request = providers.make_request(
        input(definition(), 3), std::make_shared<test::NoopNotifier>());
    EXPECT_TRUE(wait_for(state, [](const BackendState& value) {
        return value.performing == 1;
    }));
    request->cancel();
    EXPECT_TRUE(std::holds_alternative<GenerationCancelled>(
        receive_terminal(request).back()));
    providers.shutdown();
}

TEST(Providers, ThreadLaunchFailureReturnsFailedHandleAndWakesShutdownWaiter) {
    struct LaunchGate {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered{};
        bool release{};
    };
    auto gate = std::make_shared<LaunchGate>();
    auto state = std::make_shared<BackendState>();
    Providers providers(factory(state), [gate](std::function<void()>) {
        std::unique_lock lock(gate->mutex);
        gate->entered = true;
        gate->changed.notify_all();
        gate->changed.wait(lock, [&] { return gate->release; });
        throw std::runtime_error("thread creation failed");
    });
    auto notifier = std::make_shared<test::TestNotifier>();
    std::shared_ptr<ProviderRequest> request;
    std::thread maker([&] {
        request = providers.make_request(input(definition(), 4), notifier);
    });
    {
        std::unique_lock lock(gate->mutex);
        ASSERT_TRUE(gate->changed.wait_for(lock, 1s, [&] { return gate->entered; }));
    }
    std::atomic_bool shutdown_finished{};
    std::thread stopper([&] {
        providers.shutdown();
        shutdown_finished.store(true, std::memory_order_release);
    });
    EXPECT_TRUE(notifier->wait_for_wake(0));
    EXPECT_FALSE(shutdown_finished.load(std::memory_order_acquire));
    {
        std::lock_guard lock(gate->mutex);
        gate->release = true;
    }
    gate->changed.notify_all();
    maker.join();
    stopper.join();

    const GenerationFailed failure = std::get<GenerationFailed>(
        receive_terminal(request).back());
    EXPECT_EQ(failure.message, "thread creation failed");
    EXPECT_TRUE(shutdown_finished.load(std::memory_order_acquire));
    std::lock_guard lock(state->mutex);
    EXPECT_EQ(state->constructed, 0);
}

TEST(Providers, ClosingAdmissionReturnsFailedHandleWithoutLaunchingWork) {
    auto state = std::make_shared<BackendState>();
    Providers providers(factory(state));
    providers.shutdown();
    auto request = providers.make_request(
        input(definition(), 5), std::make_shared<test::NoopNotifier>());
    EXPECT_TRUE(std::holds_alternative<GenerationFailed>(
        receive_terminal(request).back()));
    std::lock_guard lock(state->mutex);
    EXPECT_EQ(state->constructed, 0);
}

TEST(Providers, OneFailedRequestDoesNotPreventAnotherFromCompleting) {
    auto good = std::make_shared<BackendState>();
    Providers providers([good](SharedCharacterDefinition character) {
        if (character->character.id == "bad") {
            throw std::runtime_error("bad target");
        }
        return std::make_unique<TestBackend>(good);
    });
    auto notifier = std::make_shared<test::NoopNotifier>();
    auto bad = providers.make_request(input(definition("bad"), 1), notifier);
    auto good_request = providers.make_request(input(definition("good"), 2), notifier);

    EXPECT_TRUE(std::holds_alternative<GenerationFailed>(
        receive_terminal(bad).back()));
    EXPECT_TRUE(std::holds_alternative<GenerationCompleted>(
        receive_terminal(good_request).back()));
    providers.shutdown();
}

TEST(Providers, RegistryKeepsDroppedConsumerAliveUntilWorkerQuiesces) {
    auto state = std::make_shared<BackendState>();
    state->block = true;
    Providers providers(factory(state));
    std::weak_ptr<ProviderRequest> weak;
    {
        auto request = providers.make_request(
            input(definition(), 1), std::make_shared<test::NoopNotifier>());
        weak = request;
        EXPECT_TRUE(wait_for(state, [](const BackendState& value) {
            return value.performing == 1;
        }));
    }
    EXPECT_FALSE(weak.expired());
    release(state);
    providers.shutdown();
    EXPECT_TRUE(wait_for_expired(weak));
}

TEST(Providers, SharedNotifierOutlivesItsOriginalOwnerAndIsReleasedAfterRequest) {
    auto state = std::make_shared<BackendState>();
    state->block = true;
    Providers providers(factory(state));
    auto notifier = std::make_shared<test::TestNotifier>();
    std::weak_ptr<test::TestNotifier> weak = notifier;
    auto request = providers.make_request(input(definition(), 1), notifier);
    notifier.reset();
    EXPECT_FALSE(weak.expired());
    EXPECT_TRUE(wait_for(state, [](const BackendState& value) {
        return value.performing == 1;
    }));
    release(state);
    std::shared_ptr<test::TestNotifier> retained = weak.lock();
    ASSERT_TRUE(retained);
    EXPECT_TRUE(retained->wait_for_wake(0));
    (void)receive_terminal(request);
    request.reset();
    retained.reset();
    providers.shutdown();
    EXPECT_TRUE(wait_for_expired(weak));
}

TEST(Providers, DestroysBackendBeforeShutdownObservesRegistryEmpty) {
    auto state = std::make_shared<BackendState>();
    state->block = true;
    Providers providers(factory(state));
    auto request = providers.make_request(
        input(definition(), 1), std::make_shared<test::NoopNotifier>());
    EXPECT_TRUE(wait_for(state, [](const BackendState& value) {
        return value.performing == 1;
    }));
    providers.shutdown();
    std::lock_guard lock(state->mutex);
    EXPECT_EQ(state->destroyed, 1);
    EXPECT_TRUE(std::holds_alternative<GenerationCancelled>(
        receive_terminal(request).back()));
}

TEST(Providers, RetainsEachRequestConfigurationSnapshot) {
    auto first_state = std::make_shared<BackendState>();
    first_state->block = true;
    auto second_state = std::make_shared<BackendState>();
    Providers providers([first_state, second_state](SharedCharacterDefinition character) {
        auto state = character->provider.config.model == "model-a"
            ? first_state
            : second_state;
        {
            std::lock_guard lock(state->mutex);
            state->definitions.push_back(std::move(character));
        }
        return std::make_unique<TestBackend>(std::move(state));
    });
    auto notifier = std::make_shared<test::NoopNotifier>();
    const auto first_definition = definition("same", "model-a");
    auto first = providers.make_request(input(first_definition, 1), notifier);
    EXPECT_TRUE(wait_for(first_state, [](const BackendState& value) {
        return value.performing == 1;
    }));

    // A later definition is selected independently while the first request is
    // still active, so a configuration reload cannot mutate its snapshot.
    auto second = providers.make_request(
        input(definition("same", "model-b"), 2), notifier);
    EXPECT_TRUE(std::holds_alternative<GenerationCompleted>(
        receive_terminal(second).back()));
    {
        std::lock_guard first_lock(first_state->mutex);
        ASSERT_EQ(first_state->definitions.size(), 1U);
        EXPECT_EQ(first_state->definitions[0]->provider.config.model, "model-a");
    }
    {
        std::lock_guard second_lock(second_state->mutex);
        ASSERT_EQ(second_state->definitions.size(), 1U);
        EXPECT_EQ(second_state->definitions[0]->provider.config.model, "model-b");
    }
    release(first_state);
    EXPECT_TRUE(std::holds_alternative<GenerationCompleted>(
        receive_terminal(first).back()));
    providers.shutdown();
}

TEST(Providers, CancellingAnOldSnapshotDoesNotAffectANewerRequest) {
    auto first_state = std::make_shared<BackendState>();
    first_state->block = true;
    auto second_state = std::make_shared<BackendState>();
    Providers providers([first_state, second_state](SharedCharacterDefinition character) {
        return std::make_unique<TestBackend>(
            character->provider.config.model == "model-a" ? first_state : second_state);
    });
    auto notifier = std::make_shared<test::NoopNotifier>();

    auto first = providers.make_request(
        input(definition("same", "model-a"), 1), notifier);
    ASSERT_TRUE(wait_for(first_state, [](const BackendState& state) {
        return state.performing == 1;
    }));
    auto second = providers.make_request(
        input(definition("same", "model-b"), 2), notifier);

    EXPECT_TRUE(std::holds_alternative<GenerationCompleted>(
        receive_terminal(second).back()));
    first->cancel();
    EXPECT_TRUE(std::holds_alternative<GenerationCancelled>(
        receive_terminal(first).back()));
    providers.shutdown();
    {
        std::lock_guard lock(first_state->mutex);
        EXPECT_EQ(first_state->constructed, 1);
        EXPECT_EQ(first_state->destroyed, 1);
    }
    {
        std::lock_guard lock(second_state->mutex);
        EXPECT_EQ(second_state->constructed, 1);
        EXPECT_EQ(second_state->destroyed, 1);
    }
}

TEST(Providers, InvalidInputReturnsFailedTerminalWithoutLaunching) {
    auto state = std::make_shared<BackendState>();
    Providers providers(factory(state));
    EXPECT_THROW(
        (void)providers.make_request(input(definition(), 4), nullptr),
        std::invalid_argument);
    ProviderRequestInput invalid;
    auto request = providers.make_request(
        std::move(invalid), std::make_shared<test::NoopNotifier>());
    EXPECT_TRUE(std::holds_alternative<GenerationFailed>(
        receive_terminal(request).back()));
    std::lock_guard lock(state->mutex);
    EXPECT_EQ(state->constructed, 0);
}

TEST(Providers, DefaultFactoryResolvesCredentialsForEachRequest) {
    constexpr std::string_view variable = "CHA_PROVIDERS_DEFAULT_CLIENT_KEY";
    ScopedEnvironmentVariable environment{std::string(variable)};
    ASSERT_TRUE(set_environment_variable(variable, "first-key"));

    CharacterDefinition configured = *definition();
    configured.provider.config.api_key.clear();
    configured.provider.config.api_key_env = variable;
    const auto character = std::make_shared<const CharacterDefinition>(configured);
    auto notifier = std::make_shared<test::NoopNotifier>();
    {
        Providers providers;
        auto first = providers.make_request(input(character, 1), notifier);
        auto second = providers.make_request(input(character, 2), notifier);

        const std::vector<GenerationEvent> first_events = receive_terminal(first);
        const std::vector<GenerationEvent> second_events = receive_terminal(second);
        ASSERT_EQ(first_events.size(), 2U);
        ASSERT_EQ(second_events.size(), 2U);
        EXPECT_EQ(std::get<GenerationEventDelta>(first_events.front()).text, "Question");
        EXPECT_EQ(std::get<GenerationEventDelta>(second_events.front()).text, "Question");
        EXPECT_TRUE(std::holds_alternative<GenerationCompleted>(first_events.back()));
        EXPECT_TRUE(std::holds_alternative<GenerationCompleted>(second_events.back()));
        providers.shutdown();
    }

    ASSERT_TRUE(unset_environment_variable(variable));
    {
        Providers providers;
        auto request = providers.make_request(input(character, 3), notifier);
        const GenerationFailed failure = std::get<GenerationFailed>(
            receive_terminal(request).back());
        EXPECT_NE(failure.message.find(std::string(variable)), std::string::npos);
        providers.shutdown();
    }
}

TEST(Providers, MakeRequestRacingWithShutdownLeavesEveryCallerWithATerminal) {
    auto state = std::make_shared<BackendState>();
    state->block = true;
    Providers providers(factory(state));
    auto notifier = std::make_shared<test::NoopNotifier>();
    std::atomic_bool start{};
    std::vector<std::shared_ptr<ProviderRequest>> requests(8);
    std::vector<std::thread> callers;
    callers.reserve(requests.size());
    for (std::size_t index = 0; index < requests.size(); ++index) {
        callers.emplace_back([&, index] {
            while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
            requests[index] = providers.make_request(
                input(definition(), index + 1), notifier);
        });
    }
    std::thread stopper([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        providers.shutdown();
    });
    start.store(true, std::memory_order_release);
    for (std::thread& caller : callers) caller.join();
    stopper.join();

    for (const auto& request : requests) {
        ASSERT_TRUE(request);
        const std::vector<GenerationEvent> events = receive_terminal(request);
        EXPECT_EQ(events.size(), 1U);
        EXPECT_FALSE(std::holds_alternative<GenerationEventDelta>(events.back()));
    }
}

} // namespace
} // namespace cha
