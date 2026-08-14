#include "agents/generation_batch.h"
#include "agents/generation_executor.h"
#include "support/test_backends.h"
#include "support/test_generations.h"
#include "support/test_notifier.h"

#include <gtest/gtest.h>

#include <chrono>
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
using test::generation_request;
using test::RecordingBackend;

test::TestNotifier& notifier() {
    static test::TestNotifier instance;
    return instance;
}

GenerationEvent next_foreground_event(GenerationBatch& batch) {
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline) {
        GenerationEvent event = GenerationCompleted{};
        if (batch.try_receive_foreground(event) == ChannelReadStatus::value) {
            return event;
        }
        std::this_thread::yield();
    }
    throw std::runtime_error("Timed out waiting for generation event");
}

// Stages, opens, and drains one single-target batch through the executor.
void expect_one_completed_run(
    GenerationExecutor& executor,
    const Transcript& transcript,
    RequestId id,
    const std::string& target,
    const std::string& name) {
    GenerationBatch batch =
        executor.stage_batch({generation_request(transcript, id, target, name)});
    batch.open();
    EXPECT_TRUE(
        std::holds_alternative<GenerationCompleted>(next_foreground_event(batch)));
}

TEST(GenerationExecutor, RejectsBorrowedPoolWhoseWidthDiffersFromBackendCount) {
    ThreadPool pool(1);
    std::vector<std::unique_ptr<ModelBackend>> backends;
    backends.push_back(std::make_unique<RecordingBackend>(
        "one-id", "One", "one"));
    backends.push_back(std::make_unique<RecordingBackend>(
        "two-id", "Two", "two"));

    EXPECT_THROW(
        (void)GenerationExecutor(std::move(backends), notifier(), pool),
        std::invalid_argument);
}

TEST(GenerationExecutor, RejectsEmptyAndNullBackendConstruction) {
    ThreadPool pool(1);
    EXPECT_THROW(
        (void)GenerationExecutor(
            std::vector<std::unique_ptr<ModelBackend>>{}, notifier(), pool),
        std::invalid_argument);

    std::vector<std::unique_ptr<ModelBackend>> backends;
    backends.push_back(nullptr);
    EXPECT_THROW(
        (void)GenerationExecutor(std::move(backends), notifier(), pool),
        std::invalid_argument);
}

TEST(GenerationExecutor, RejectsInvalidBackendMetadataAtConstruction) {
    ThreadPool pool(1);
    EXPECT_THROW(
        (void)GenerationExecutor(
            test::one_backend(std::make_unique<RecordingBackend>(
                "valid-id", " Invalid name", "answer")),
            notifier(),
            pool),
        std::invalid_argument);
}

TEST(GenerationExecutor, RejectsDuplicateBackendMetadataAtConstruction) {
    ThreadPool pool(2);
    std::vector<std::unique_ptr<ModelBackend>> duplicate_ids;
    duplicate_ids.push_back(
        std::make_unique<RecordingBackend>("one-id", "One", ""));
    duplicate_ids.push_back(
        std::make_unique<RecordingBackend>("one-id", "Two", ""));
    EXPECT_THROW(
        (void)GenerationExecutor(std::move(duplicate_ids), notifier(), pool),
        std::invalid_argument);

    std::vector<std::unique_ptr<ModelBackend>> duplicate_names;
    duplicate_names.push_back(
        std::make_unique<RecordingBackend>("one-id", "One", ""));
    duplicate_names.push_back(
        std::make_unique<RecordingBackend>("two-id", "ONE", ""));
    EXPECT_THROW(
        (void)GenerationExecutor(std::move(duplicate_names), notifier(), pool),
        std::invalid_argument);
}

TEST(GenerationExecutor, ExposesPublicRuntimeInfoInBackendOrder) {
    ThreadPool pool(2);
    std::vector<std::unique_ptr<ModelBackend>> backends;
    backends.push_back(std::make_unique<RecordingBackend>("one-id", "One", ""));
    backends.push_back(std::make_unique<RecordingBackend>("two-id", "Two", ""));
    GenerationExecutor executor(std::move(backends), notifier(), pool);

    const std::vector<ModelBackendInfo>& info = executor.runtime_info();
    ASSERT_EQ(info.size(), 2U);
    EXPECT_EQ(info[0].character.id, "one-id");
    EXPECT_EQ(info[1].character.id, "two-id");
    EXPECT_EQ(info[0].api, "test://model");
    pool.stop();
}

TEST(GenerationExecutor, IdentifiesCharacterWhoseDefinitionStartupFails) {
    ThreadPool pool(1);
    CharacterDefinition definition{
        .character = {
            .id = "alpha-id",
            .display_name = "Alpha",
        },
        .backend = {
            .api_key_env = "__CHA_TEST_MISSING_AGENT_KEY__",
        },
        .system_prompt = "Prompt",
    };

    try {
        (void)GenerationExecutor(
            std::vector<CharacterDefinition>{std::move(definition)},
            notifier(),
            pool);
        FAIL() << "Expected startup failure";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("Character 'Alpha'"), std::string::npos);
        EXPECT_EQ(message.find("alpha-id"), std::string::npos);
    }
}

TEST(GenerationExecutor, RejectsEmptyBatchAndMissingHistory) {
    Transcript transcript;
    ThreadPool pool(1);
    GenerationExecutor executor(
        test::one_backend(std::make_unique<RecordingBackend>(
            "one-id", "One", "")),
        notifier(),
        pool);

    EXPECT_THROW(
        (void)executor.stage_batch({}),
        std::invalid_argument);

    GenerationRequest invalid = generation_request(transcript, 1, "one-id", "One");
    invalid.history.reset();
    EXPECT_THROW(
        (void)executor.stage_batch({std::move(invalid)}),
        std::invalid_argument);

    expect_one_completed_run(executor, transcript, 2, "one-id", "One");
    pool.stop();
}

TEST(GenerationExecutor, RejectsUnknownAndDuplicateTargets) {
    Transcript transcript;
    ThreadPool pool(1);
    GenerationExecutor executor(
        test::one_backend(std::make_unique<RecordingBackend>(
            "one-id", "One", "")),
        notifier(),
        pool);

    EXPECT_THROW(
        (void)executor.stage_batch(
            {generation_request(transcript, 1, "missing-id", "Missing")}),
        std::invalid_argument);
    EXPECT_THROW(
        (void)executor.stage_batch({
            generation_request(transcript, 2, "one-id", "One"),
            generation_request(transcript, 3, "one-id", "One"),
        }),
        std::invalid_argument);

    expect_one_completed_run(executor, transcript, 4, "one-id", "One");
    pool.stop();
}

TEST(GenerationExecutor, RejectsStagingIntoAStoppedPoolWithoutCallingABackend) {
    Transcript transcript;
    ThreadPool pool(1);
    auto backend = std::make_unique<RecordingBackend>("one-id", "One", "");
    RecordingBackend* view = backend.get();
    GenerationExecutor executor(
        test::one_backend(std::move(backend)), notifier(), pool);
    pool.stop();

    EXPECT_THROW(
        (void)executor.stage_batch(
            {generation_request(transcript, 1, "one-id", "One")}),
        std::runtime_error);
    EXPECT_FALSE(view->prepared.load(std::memory_order_acquire));
    EXPECT_FALSE(view->performed.load(std::memory_order_acquire));
    // The failed staging left no executor state that would change how the next
    // attempt fails.
    EXPECT_THROW(
        (void)executor.stage_batch(
            {generation_request(transcript, 2, "one-id", "One")}),
        std::runtime_error);
}

TEST(GenerationExecutor, RollsBackPartialSubmissionAndFreesEveryAcceptedWorker) {
    Transcript transcript;
    test::BarrierState state;
    ThreadPool pool(2);
    auto one = std::make_unique<test::BarrierBackend>("one-id", "One", state);
    auto two = std::make_unique<test::BarrierBackend>("two-id", "Two", state);
    test::BarrierBackend* one_view = one.get();
    test::BarrierBackend* two_view = two.get();
    std::vector<std::unique_ptr<ModelBackend>> backends;
    backends.push_back(std::move(one));
    backends.push_back(std::move(two));
    bool inject_failure = true;
    GenerationExecutor executor(
        std::move(backends),
        notifier(),
        pool,
        [&inject_failure](std::size_t submission_index) {
            if (submission_index == 1 && inject_failure) {
                inject_failure = false;
                throw std::runtime_error("injected submission failure");
            }
        });

    EXPECT_THROW(
        (void)executor.stage_batch({
            generation_request(transcript, 1, "one-id", "One"),
            generation_request(transcript, 2, "two-id", "Two"),
        }),
        std::runtime_error);
    EXPECT_FALSE(one_view->prepared.load(std::memory_order_acquire));
    EXPECT_FALSE(two_view->prepared.load(std::memory_order_acquire));

    // Both workers can be inside perform() at once again, which is possible
    // only because the one accepted gated task finished during the rollback.
    GenerationBatch batch = executor.stage_batch({
        generation_request(transcript, 3, "one-id", "One"),
        generation_request(transcript, 4, "two-id", "Two"),
    });
    batch.open();
    const bool both_entered = test::wait_until_entered(state, 2);
    state.release.store(true, std::memory_order_release);
    EXPECT_TRUE(both_entered);
    EXPECT_TRUE(
        std::holds_alternative<GenerationCompleted>(next_foreground_event(batch)));
    batch.advance_foreground();
    EXPECT_TRUE(
        std::holds_alternative<GenerationCompleted>(next_foreground_event(batch)));
}

// --- Backend replacement ----------------------------------------------------

CharacterDefinition recipe_definition(
    std::string id,
    std::string name,
    std::string model = "original-model") {
    return {
        .character = {
            .id = std::move(id),
            .display_name = std::move(name),
        },
        .backend = {
            .host = "127.0.0.1",
            .port = 1,
            .model = std::move(model),
        },
        .system_prompt = "Test prompt",
    };
}

// Shared observation of every backend one factory builds: the configs it was
// given, and one performed flag per construction so a test can tell which
// instance served a request even after a slot was swapped.
struct FactoryObservation {
    std::vector<ModelBackendConfig> configs;
    std::vector<std::shared_ptr<std::atomic_bool>> performed;
};

class FactoryBackend final : public ModelBackend {
public:
    FactoryBackend(
        CharacterDefinition definition,
        std::shared_ptr<std::atomic_bool> performed)
        : definition_(std::move(definition)), performed_(std::move(performed)) {
    }

    RequestPayload prepare(const GenerationRequest& input) override {
        return {.bytes = input.run.prompt_text};
    }

    GenerationResult perform(
        RequestPayload,
        const GenerationDeltaSink&,
        const std::atomic_bool&) override {
        performed_->store(true, std::memory_order_release);
        return {};
    }

    ModelBackendInfo info() const override {
        return {
            definition_.character,
            definition_.backend.model,
            "test://model",
            true,
        };
    }

private:
    CharacterDefinition definition_;
    std::shared_ptr<std::atomic_bool> performed_;
};

GenerationExecutor::BackendFactory recording_factory(
    const std::shared_ptr<FactoryObservation>& observation) {
    return [observation](CharacterDefinition definition) {
        observation->configs.push_back(definition.backend);
        auto performed = std::make_shared<std::atomic_bool>(false);
        observation->performed.push_back(performed);
        return std::unique_ptr<ModelBackend>(
            new FactoryBackend(std::move(definition), std::move(performed)));
    };
}

TEST(GenerationExecutor, BuildsBackendsThroughTheFactoryInOrder) {
    ThreadPool pool(2);
    auto observation = std::make_shared<FactoryObservation>();
    GenerationExecutor executor(
        std::vector<CharacterDefinition>{
            recipe_definition("one-id", "One", "model-one"),
            recipe_definition("two-id", "Two", "model-two"),
        },
        notifier(),
        pool,
        recording_factory(observation));

    ASSERT_EQ(observation->configs.size(), 2U);
    EXPECT_EQ(observation->configs[0].model, "model-one");
    EXPECT_EQ(observation->configs[1].model, "model-two");
    ASSERT_EQ(executor.runtime_info().size(), 2U);
    EXPECT_EQ(executor.runtime_info()[0].model, "model-one");
    pool.stop();
}

TEST(GenerationExecutor, ReplaceBackendSwapsTheSlotAndRefreshesRuntimeInfo) {
    Transcript transcript;
    ThreadPool pool(2);
    auto observation = std::make_shared<FactoryObservation>();
    GenerationExecutor executor(
        std::vector<CharacterDefinition>{
            recipe_definition("one-id", "One"),
            recipe_definition("two-id", "Two"),
        },
        notifier(),
        pool,
        recording_factory(observation));

    ModelBackendConfig override_config{
        .host = "127.0.0.1",
        .port = 2,
        .model = "override-model",
    };
    executor.replace_backend("one-id", override_config);

    ASSERT_EQ(observation->configs.size(), 3U);
    EXPECT_EQ(observation->configs[2].model, "override-model");
    EXPECT_EQ(observation->configs[2].port, 2);
    ASSERT_EQ(executor.runtime_info().size(), 2U);
    EXPECT_EQ(executor.runtime_info()[0].model, "override-model");
    EXPECT_EQ(executor.runtime_info()[0].character.id, "one-id");
    EXPECT_EQ(executor.runtime_info()[1].model, "original-model");

    // A staged request reaches the replacement; the replaced backend is gone
    // and the untouched slot still answers for its own character.
    expect_one_completed_run(executor, transcript, 1, "one-id", "One");
    EXPECT_TRUE(observation->performed[2]->load(std::memory_order_acquire));
    EXPECT_FALSE(observation->performed[0]->load(std::memory_order_acquire));
    expect_one_completed_run(executor, transcript, 2, "two-id", "Two");
    EXPECT_TRUE(observation->performed[1]->load(std::memory_order_acquire));
    pool.stop();
}

TEST(GenerationExecutor, ResetBackendRebuildsWithTheOriginalConfig) {
    ThreadPool pool(1);
    auto observation = std::make_shared<FactoryObservation>();
    GenerationExecutor executor(
        std::vector<CharacterDefinition>{
            recipe_definition("one-id", "One", "original-model"),
        },
        notifier(),
        pool,
        recording_factory(observation));

    ModelBackendConfig override_config{
        .host = "127.0.0.1",
        .port = 2,
        .model = "override-model",
    };
    executor.replace_backend("one-id", override_config);
    executor.reset_backend("one-id");

    ASSERT_EQ(observation->configs.size(), 3U);
    EXPECT_EQ(observation->configs[2].model, "original-model");
    EXPECT_EQ(observation->configs[2].port, 1);
    EXPECT_EQ(executor.runtime_info()[0].model, "original-model");
    pool.stop();
}

TEST(GenerationExecutor, ReplaceBackendRejectsUnknownAndMalformedResults) {
    ThreadPool pool(1);
    auto observation = std::make_shared<FactoryObservation>();
    GenerationExecutor executor(
        std::vector<CharacterDefinition>{
            recipe_definition("one-id", "One"),
        },
        notifier(),
        pool,
        recording_factory(observation));

    EXPECT_THROW(
        executor.replace_backend("missing-id", {}),
        std::invalid_argument);
    EXPECT_THROW(
        executor.reset_backend("missing-id"),
        std::invalid_argument);

    // Misbehaving factories are keyed on a marker model so construction
    // succeeds and only the replacement goes wrong.
    const ModelBackendConfig marker{
        .host = "127.0.0.1",
        .port = 1,
        .model = "misbehave",
    };

    GenerationExecutor null_factory_executor(
        std::vector<CharacterDefinition>{recipe_definition("one-id", "One")},
        notifier(),
        pool,
        [](CharacterDefinition definition) {
            if (definition.backend.model == "misbehave") {
                return std::unique_ptr<ModelBackend>();
            }
            return std::unique_ptr<ModelBackend>(
                new RecordingBackend("one-id", "One", ""));
        });
    EXPECT_THROW(
        null_factory_executor.replace_backend("one-id", marker),
        std::runtime_error);
    EXPECT_EQ(null_factory_executor.runtime_info()[0].character.id, "one-id");

    GenerationExecutor wrong_id_executor(
        std::vector<CharacterDefinition>{recipe_definition("one-id", "One")},
        notifier(),
        pool,
        [](CharacterDefinition definition) {
            if (definition.backend.model == "misbehave") {
                return std::unique_ptr<ModelBackend>(
                    new RecordingBackend("other-id", "Other", ""));
            }
            return std::unique_ptr<ModelBackend>(
                new RecordingBackend("one-id", "One", ""));
        });
    EXPECT_THROW(
        wrong_id_executor.replace_backend("one-id", marker),
        std::runtime_error);
    EXPECT_EQ(wrong_id_executor.runtime_info()[0].character.id, "one-id");
    pool.stop();
}

TEST(GenerationExecutor, AFactoryThrowLeavesTheExistingSlotInPlace) {
    Transcript transcript;
    ThreadPool pool(1);
    auto observation = std::make_shared<FactoryObservation>();
    auto failing_factory = [observation](CharacterDefinition definition) {
        if (definition.backend.model == "explode") {
            throw std::runtime_error("factory exploded");
        }
        return recording_factory(observation)(std::move(definition));
    };
    GenerationExecutor executor(
        std::vector<CharacterDefinition>{
            recipe_definition("one-id", "One"),
        },
        notifier(),
        pool,
        failing_factory);

    ModelBackendConfig broken{
        .host = "127.0.0.1",
        .port = 1,
        .model = "explode",
    };
    EXPECT_THROW(executor.replace_backend("one-id", broken), std::runtime_error);
    EXPECT_EQ(executor.runtime_info()[0].model, "original-model");

    expect_one_completed_run(executor, transcript, 1, "one-id", "One");
    EXPECT_TRUE(observation->performed[0]->load(std::memory_order_acquire));
    pool.stop();
}

TEST(GenerationExecutor, ReplacementIsUnavailableWithoutDefinitions) {
    ThreadPool pool(1);
    GenerationExecutor executor(
        test::one_backend(std::make_unique<RecordingBackend>(
            "one-id", "One", "")),
        notifier(),
        pool);

    EXPECT_THROW(executor.replace_backend("one-id", {}), std::logic_error);
    EXPECT_THROW(executor.reset_backend("one-id"), std::logic_error);
    pool.stop();
}

} // namespace
} // namespace cha
