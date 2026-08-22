#include "session/session_controller.h"
#include "agents/character.h"
#include "agents/model_backend.h"
#include "session/session_database.h"
#include "support/test_backends.h"
#include "support/test_controller.h"
#include "support/test_notifier.h"
#include "support/test_session_database.h"
#include "support/test_transcript.h"
#include "util/path_name.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <variant>

namespace cha {
namespace {

test::TestNotifier& notifier() {
    static test::TestNotifier instance;
    return instance;
}

ModelMessage operator_prompt(std::string_view text) {
    return {ModelRole::persona, "from Operator:\n" + std::string(text)};
}

std::vector<ModelMessage> context_without_timestamp_metadata(
    GenerationRequest input,
    std::string_view system_prompt) {
    input.run.created_at = 0;
    return project_model_context(input, system_prompt);
}

// Blocks the execution's final wake. This makes `execution_finished` true
// while the worker task is still live, so shutdown must join the pool rather
// than treating the batch's backend-safety barrier as full quiescence.
class FinalWakeBlockingNotifier final : public WakeNotifier {
public:
    void block_final_wake() noexcept {
        std::lock_guard lock(mutex_);
        block_final_wake_ = true;
    }

    void wake() noexcept override {
        std::unique_lock lock(mutex_);
        ++wake_count_;
        if (!block_final_wake_ || wake_count_ < 2) {
            return;
        }
        final_wake_blocked_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return release_final_wake_; });
    }

    bool wait_for_final_wake(
        std::chrono::milliseconds timeout = std::chrono::seconds(1)) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [this] {
            return final_wake_blocked_;
        });
    }

    void release_final_wake() noexcept {
        {
            std::lock_guard lock(mutex_);
            release_final_wake_ = true;
        }
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::size_t wake_count_{};
    bool block_final_wake_{};
    bool final_wake_blocked_{};
    bool release_final_wake_{};
};

std::vector<TranscriptEntry> copy_entries(TranscriptView transcript) {
    const std::span<const TranscriptEntry> entries = transcript.entries;
    return {entries.begin(), entries.end()};
}

void wait_until_next_unix_second() {
    const auto started = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch());
    while (std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
           == started) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// Removes one temporary session database when a controller test leaves scope.
class TemporaryJournal {
public:
    TemporaryJournal()
      : path(std::filesystem::temp_directory_path()
             / ("cha_controller_"
                + std::to_string(
                    std::chrono::steady_clock::now()
                        .time_since_epoch()
                        .count())
                + ".sqlite3")) {
        if (!create_session_database(
                path,
                {
                    .id = "controller-test",
                    .forum = "test-forum",
                    .label = "Controller test",
                })) {
            throw std::runtime_error("Failed to create controller test database");
        }
    }

    ~TemporaryJournal() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    std::filesystem::path path;
};

// Returns scripted generation output while retaining immutable inputs for assertions.
class ScriptedBackend final : public test::DescribedModelBackend {
public:
    ScriptedBackend(
        GenerationResult result = {},
        std::vector<std::string> deltas = {},
        bool wait_for_cancellation = false,
        std::string id = "guide-id",
        std::string name = "Guide")
      : id_(std::move(id)),
        name_(std::move(name)),
        result_(std::move(result)),
        wait_for_cancellation_(wait_for_cancellation) {
        for (std::string& delta : deltas) {
            deltas_.push_back({
                GenerationDeltaKind::answer,
                std::move(delta),
            });
        }
    }

    ScriptedBackend(
        std::vector<GenerationDelta> deltas,
        GenerationResult result = {},
        bool wait_for_cancellation = false,
        std::string id = "guide-id",
        std::string name = "Guide")
      : id_(std::move(id)),
        name_(std::move(name)),
        result_(std::move(result)),
        deltas_(std::move(deltas)),
        wait_for_cancellation_(wait_for_cancellation) {
    }

    RequestPayload prepare(const GenerationRequest& input) override {
        std::lock_guard lock(observations_mutex_);
        inputs.push_back(input);
        model_contexts.push_back(project_model_context(input, system_prompt));
        return {.bytes = input.run.prompt_text};
    }

    GenerationResult perform(
        RequestPayload,
        const GenerationDeltaSink& on_delta,
        const std::atomic_bool& cancellation) override {
        for (const GenerationDelta& delta : deltas_) {
            on_delta(delta);
        }
        if (hold_after_deltas != nullptr) {
            while (!hold_after_deltas->load(std::memory_order_acquire)
                   && !cancellation.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        }
        if (wait_for_cancellation_) {
            while (!cancellation.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return {GenerationOutcome::cancelled, {}};
        }
        return result_;
    }

    CharacterRuntimeInfo info() const override {
        return {
            .character = {
                .id = id_,
                .display_name = name_,
            },
            .model = "test-model",
            .api = "test://model",
            .streaming = true,
        };
    }

    std::vector<GenerationRequest> inputs;
    std::vector<std::vector<ModelMessage>> model_contexts;
    std::string system_prompt;
    std::atomic_bool* hold_after_deltas = nullptr;

private:
    std::mutex observations_mutex_;
    std::string id_{"guide-id"};
    std::string name_{"Guide"};
    GenerationResult result_;
    std::vector<GenerationDelta> deltas_;
    bool wait_for_cancellation_{};
};

class ConcurrentBackend final : public test::DescribedModelBackend {
public:
    ConcurrentBackend(
        std::string id,
        std::string name,
        std::string answer,
        const std::atomic_bool* release = nullptr)
        : id_(std::move(id)),
          name_(std::move(name)),
          answer_(std::move(answer)),
          release_(release) {
    }

    RequestPayload prepare(const GenerationRequest& input) override {
        std::lock_guard lock(observations_mutex_);
        inputs.push_back(input);
        return {.bytes = input.run.prompt_text};
    }

    GenerationResult perform(
        RequestPayload,
        const GenerationDeltaSink& on_delta,
        const std::atomic_bool& cancellation) override {
        entered.store(true, std::memory_order_release);
        while (release_ && !release_->load(std::memory_order_acquire)
               && !cancellation.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        if (cancellation.load(std::memory_order_acquire)) {
            finished.store(true, std::memory_order_release);
            return {GenerationOutcome::cancelled, {}};
        }
        on_delta({GenerationDeltaKind::answer, answer_});
        finished.store(true, std::memory_order_release);
        return {};
    }

    CharacterRuntimeInfo info() const override {
        return {
            .character = {.id = id_, .display_name = name_},
            .model = "test-model",
            .api = "test://model",
            .streaming = true,
        };
    }

    std::vector<GenerationRequest> inputs;
    std::atomic_bool entered{false};
    std::atomic_bool finished{false};

private:
    std::mutex observations_mutex_;
    std::string id_;
    std::string name_;
    std::string answer_;
    const std::atomic_bool* release_{};
};

class CancellationBlockingBackend final : public test::DescribedModelBackend {
public:
    CancellationBlockingBackend(
        std::string id,
        std::string name,
        std::atomic_bool& release,
        GenerationResult result = {GenerationOutcome::cancelled, {}})
        : id_(std::move(id)), name_(std::move(name)), release_(release),
          result_(std::move(result)) {
    }

    RequestPayload prepare(const GenerationRequest& input) override {
        return {.bytes = input.run.prompt_text};
    }

    GenerationResult perform(
        RequestPayload,
        const GenerationDeltaSink&,
        const std::atomic_bool& cancellation) override {
        entered.store(true, std::memory_order_release);
        while (!cancellation.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        while (!release_.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return result_;
    }

    CharacterRuntimeInfo info() const override {
        return {
            .character = {.id = id_, .display_name = name_},
            .model = "test-model",
            .api = "test://model",
            .streaming = true,
        };
    }

    std::atomic_bool entered{};

private:
    std::string id_;
    std::string name_;
    std::atomic_bool& release_;
    GenerationResult result_;
};

class OverlappingBackend final : public test::DescribedModelBackend {
public:
    explicit OverlappingBackend(std::atomic_bool& release_first)
        : release_first_(release_first) {}

    RequestPayload prepare(const GenerationRequest& input) override {
        return {.bytes = input.run.prompt_text};
    }

    GenerationResult perform(
        RequestPayload,
        const GenerationDeltaSink& on_delta,
        const std::atomic_bool& cancellation) override {
        const int call = calls.fetch_add(1, std::memory_order_acq_rel);
        if (call == 0) {
            while (!cancellation.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            first_cancelled.store(true, std::memory_order_release);
            while (!release_first_.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return {GenerationOutcome::cancelled, {}};
        }
        on_delta({GenerationDeltaKind::answer, "Second answer"});
        return {};
    }

    CharacterRuntimeInfo info() const override {
        return {
            .character = {.id = "two-id", .display_name = "Two"},
            .model = "test-model",
            .api = "test://model",
            .streaming = true,
        };
    }

    std::atomic_int calls{};
    std::atomic_bool first_cancelled{};

private:
    std::atomic_bool& release_first_;
};

class ThrowingPrepareBackend final : public test::DescribedModelBackend {
public:
    RequestPayload prepare(const GenerationRequest&) override {
        throw std::runtime_error("preparation failed");
    }

    GenerationResult perform(
        RequestPayload,
        const GenerationDeltaSink&,
        const std::atomic_bool&) override {
        performed = true;
        return {};
    }

    CharacterRuntimeInfo info() const override {
        return {
            .character = {
                .id = "guide-id",
                .display_name = "Guide",
            },
            .model = "test-model",
            .api = "test://model",
            .streaming = true,
        };
    }

    bool performed{};
};

ControllerUpdate receive_until_idle(SessionController& controller) {
    ControllerUpdate combined;
    while (controller.is_generating()) {
        const std::size_t observed = notifier().wake_count();
        merge(combined, test::receive_all_events(controller));
        if (controller.is_generating()
            && !notifier().wait_for_wake(observed)) {
            throw std::runtime_error(
                "Timed out waiting for controller event");
        }
    }
    return combined;
}

TEST(SessionController, BuildsStablePerCharacterPromptCacheKeys) {
    TemporaryJournal short_journal;
    auto short_backend = std::make_unique<ScriptedBackend>();
    ScriptedBackend* short_view = short_backend.get();
    auto short_controller = test::from_test_backends(
        test::one_backend(std::move(short_backend)),
        short_journal.path,
        notifier(),
        {},
        {},
        std::nullopt,
        {"forum", "session"});

    (void)short_controller->submit_prompt("operator", "First");
    (void)receive_until_idle(*short_controller);
    (void)short_controller->submit_prompt("operator", "Second");
    (void)receive_until_idle(*short_controller);
    ASSERT_EQ(short_view->inputs.size(), 2U);
    EXPECT_EQ(short_view->inputs[0].run.prompt_cache_key, "forum/session/guide-id");
    EXPECT_EQ(
        short_view->inputs[1].run.prompt_cache_key,
        short_view->inputs[0].run.prompt_cache_key);

    TemporaryJournal long_journal;
    auto long_backend = std::make_unique<ScriptedBackend>();
    ScriptedBackend* long_view = long_backend.get();
    auto long_controller = test::from_test_backends(
        test::one_backend(std::move(long_backend)),
        long_journal.path,
        notifier(),
        {},
        {},
        std::nullopt,
        {std::string(30, 'f'), std::string(30, 's')});
    (void)long_controller->submit_prompt("operator", "Long");
    (void)receive_until_idle(*long_controller);
    ASSERT_EQ(long_view->inputs.size(), 1U);
    const std::string& digest = long_view->inputs.front().run.prompt_cache_key;
    EXPECT_EQ(digest, "d9cb4005d6e1505e03f996791763ef57f5ab07bef766af2edd8f2f05bde795bc");
}

void receive_until_entry_count(
    SessionController& controller, std::size_t count) {
    while (controller.view().transcript.entries.size() < count) {
        const std::size_t observed = notifier().wake_count();
        (void)test::receive_all_events(controller);
        if (controller.view().transcript.entries.size() >= count) {
            return;
        }
        if (!notifier().wait_for_wake(observed)) {
            throw std::runtime_error("Timed out waiting for transcript entry");
        }
    }
}

ControllerUpdate receive_when_ready(SessionController& controller) {
    while (true) {
        const std::size_t observed = notifier().wake_count();
        ControllerUpdate update = test::receive_all_events(controller);
        if (has_state_update(update) || update.session_ended || update.notice) {
            return update;
        }
        if (!notifier().wait_for_wake(observed)) {
            throw std::runtime_error(
                "Timed out waiting for controller event");
        }
    }
}

SessionRestore restore_with(
    std::vector<TranscriptEntry> entries,
    RequestId next_request_id,
    EntryId next_entry_id) {
    return {
        .entries = std::move(entries),
        .next_request_id = next_request_id,
        .next_entry_id = next_entry_id,
    };
}

TEST(SessionController, RejectsEmptyCharacterConfiguration) {
    TemporaryJournal temporary;

    try {
        (void)test::from_definitions_for_testing(
            {}, temporary.path, notifier());
        FAIL() << "Expected empty-character configuration rejection";
    } catch (const std::invalid_argument& error) {
        EXPECT_EQ(
            error.what(),
            std::string("Generation requires at least one character"));
    }
}

TEST(SessionController, RejectsAnEmptyPersonaRoster) {
    TemporaryJournal temporary;

    try {
        (void)test::from_test_backends(
            test::one_backend(std::make_unique<ScriptedBackend>()),
            PersonaRoster{},
            temporary.path,
            notifier());
        FAIL() << "Expected empty-persona-roster rejection";
    } catch (const std::invalid_argument& error) {
        EXPECT_EQ(
            error.what(),
            std::string("Session controller requires at least one persona"));
    }
}

TEST(SessionController, RejectsUnknownInitialDefaultCharacterID) {
    TemporaryJournal temporary;

    try {
        (void)test::from_test_backends(
            test::one_backend(std::make_unique<ScriptedBackend>()),
            test::operator_roster(),
            temporary.path,
            notifier(),
            {},
            {},
            "unknown-id");
        FAIL() << "Expected unknown initial default rejection";
    } catch (const std::invalid_argument& error) {
        EXPECT_EQ(
            error.what(),
            std::string("Initial default character ID is not in the forum roster"));
    }
}

TEST(SessionController, OwnsACompleteIdentifiedTypedTurn) {
    TemporaryJournal temporary;
    const TranscriptEntry earlier =
        test::human_entry(10, {"operator", "You"}, {"guide-id", "Guide"}, "Earlier", 16);
    {
        SessionJournal journal(temporary.path);
        journal.start_turn(16, earlier);
        journal.cancel_turn(16, std::nullopt);
    }
    auto backend = std::make_unique<ScriptedBackend>(
        GenerationResult{},
        std::vector<std::string>{"Hello", " there"});
    ScriptedBackend* backend_view = backend.get();
    auto controller = test::from_test_backends(
        test::one_backend(std::move(backend)),
        temporary.path,
        notifier(),
        restore_with({earlier}, 17, 11));

    const ControllerUpdate submitted =
        controller->submit_prompt("operator", "Current");
    EXPECT_TRUE(has_state_update(submitted));
    const ControllerUpdate completed =
        receive_until_idle(*controller);
    EXPECT_FALSE(completed.session_ended);

    ASSERT_EQ(backend_view->inputs.size(), 1U);
    const GenerationRequest& request =
        backend_view->inputs.front();
    EXPECT_EQ(request.run.request_id, 17U);
    EXPECT_EQ(request.run.target.id, "guide-id");
    EXPECT_EQ(request.run.prompt_text, "Current");
    EXPECT_NE(request.run.created_at, 0);
    ASSERT_EQ(request.history->entries.size(), 1U);
    EXPECT_EQ(request.history->entries.front(), earlier);
    const SharedModelHistory captured_history = request.history;
    EXPECT_EQ(
        context_without_timestamp_metadata(request, backend_view->system_prompt),
        (std::vector<ModelMessage>{
            {ModelRole::persona, "from You:\nEarlier"},
            operator_prompt("Current"),
        }));
    EXPECT_TRUE(has_state_update(completed));

    const auto entries = copy_entries(controller->view().transcript);
    ASSERT_EQ(entries.size(), 3U);
    EXPECT_EQ(entries[1].created_at, request.run.created_at);
    const TranscriptEntry& response = entries.back();
    EXPECT_EQ(response.kind, EntryKind::character);
    EXPECT_EQ(response.participant_id, "guide-id");
    EXPECT_EQ(response.display_name, "Guide");
    EXPECT_EQ(response.text, "Hello there");
    EXPECT_EQ(response.status, EntryStatus::complete);
    EXPECT_EQ(captured_history->entries, (std::vector<TranscriptEntry>{earlier}));
    EXPECT_EQ(load_transcript_entries(temporary.path), entries);
}

TEST(SessionController, ResolvesAndStampsTheAuthorForEveryBatchRun) {
    TemporaryJournal temporary;
    auto first = std::make_unique<ScriptedBackend>(
        GenerationResult{}, std::vector<std::string>{"First"}, false,
        "one-id", "One");
    auto second = std::make_unique<ScriptedBackend>(
        GenerationResult{}, std::vector<std::string>{"Second"}, false,
        "two-id", "Two");
    ScriptedBackend* const first_view = first.get();
    ScriptedBackend* const second_view = second.get();
    std::vector<std::unique_ptr<test::DescribedModelBackend>> backends;
    backends.push_back(std::move(first));
    backends.push_back(std::move(second));
    auto controller = test::from_test_backends(
        std::move(backends),
        PersonaRoster{{.id = "engineer", .display_name = "Engineer"}},
        temporary.path,
        notifier());

    (void)controller->start_multicast(
        "engineer", "Question", {"One", "Two"});
    receive_until_idle(*controller);

    ASSERT_EQ(first_view->inputs.size(), 1U);
    ASSERT_EQ(second_view->inputs.size(), 1U);
    EXPECT_EQ(first_view->inputs.front().run.author.id, "engineer");
    EXPECT_EQ(first_view->inputs.front().run.author.display_name, "Engineer");
    EXPECT_EQ(second_view->inputs.front().run.author.id, "engineer");
    const std::vector<TranscriptEntry> entries = copy_entries(controller->view().transcript);
    ASSERT_EQ(entries.size(), 4U);
    EXPECT_EQ(entries[0].participant_id, "engineer");
    EXPECT_EQ(entries[0].display_name, "Engineer");
    EXPECT_EQ(entries[0].text, "Question");
    EXPECT_EQ(entries[2].participant_id, "engineer");
    EXPECT_EQ(entries[2].display_name, "Engineer");
    EXPECT_EQ(entries[2].text, "Question");
}

TEST(SessionController, RejectsUnknownAuthorBeforeOrdinaryOrMulticastBatches) {
    TemporaryJournal temporary;
    auto backend = std::make_unique<ScriptedBackend>();
    ScriptedBackend* const backend_view = backend.get();
    auto controller = test::from_test_backends(
        test::one_backend(std::move(backend)),
        PersonaRoster{{.id = "engineer", .display_name = "Engineer"}},
        temporary.path,
        notifier());

    const ControllerUpdate unknown_ordinary =
        controller->submit_prompt("unknown", "Question");
    EXPECT_FALSE(unknown_ordinary.input_consumed);
    EXPECT_EQ(unknown_ordinary.notice, "Unknown persona ID 'unknown'");
    EXPECT_EQ(
        controller->start_multicast(
            "unknown", "Question", {"Guide"}).notice,
        "Unknown persona ID 'unknown'");
    EXPECT_TRUE(controller->view().transcript.entries.empty());
    EXPECT_TRUE(backend_view->inputs.empty());
}

TEST(SessionController, BoundsGenerationEventDrains) {
    TemporaryJournal temporary;
    auto backend = std::make_unique<ScriptedBackend>(
        GenerationResult{},
        std::vector<std::string>{"one", "two", "three"});
    auto controller = test::from_test_backends(
        test::one_backend(std::move(backend)),
        temporary.path,
        notifier());

    const std::size_t observed_wakes = notifier().wake_count();
    (void)controller->submit_prompt("operator", "Question");
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (notifier().wake_count() < observed_wakes + 4
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ASSERT_GE(notifier().wake_count(), observed_wakes + 4);

    const ControllerEventBatch first = controller->receive_events(2);
    EXPECT_TRUE(first.full);
    EXPECT_TRUE(controller->is_generating());
    EXPECT_EQ(controller->view().transcript.entries.back().text, "onetwo");

    const ControllerEventBatch second = controller->receive_events(2);
    EXPECT_TRUE(second.full);
    EXPECT_FALSE(controller->is_generating());
    EXPECT_EQ(controller->view().transcript.entries.back().text, "onetwothree");

    EXPECT_FALSE(controller->receive_events(2).full);
}

TEST(SessionController, ClearMakesTheNextRequestSeeOnlyPostClearContext) {
    TemporaryJournal temporary;
    auto backend = std::make_unique<ScriptedBackend>(
        GenerationResult{}, std::vector<std::string>{"Answer"});
    ScriptedBackend* backend_view = backend.get();
    auto controller = test::from_test_backends(
        test::one_backend(std::move(backend)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("operator", "First");
    receive_until_idle(*controller);
    (void)controller->clear_transcript();
    (void)controller->submit_prompt("operator", "Second");
    receive_until_idle(*controller);

    ASSERT_EQ(backend_view->model_contexts.size(), 2U);
    EXPECT_EQ(
        context_without_timestamp_metadata(
            backend_view->inputs[1], backend_view->system_prompt),
        (std::vector<ModelMessage>{operator_prompt("Second")}));
}

TEST(SessionController, PersistsAnIdentifiedCancelledResponse) {
    TemporaryJournal temporary;
    auto controller = test::from_test_backends(
        test::one_backend(std::make_unique<ScriptedBackend>(
            GenerationResult{GenerationOutcome::cancelled, {}},
            std::vector<std::string>{"Partial"})),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("operator", "Question");
    const ControllerUpdate update =
        receive_until_idle(*controller);

    ASSERT_TRUE(update.notice);
    EXPECT_EQ(*update.notice, "Generation stopped");
    const auto restored =
        load_transcript_entries(temporary.path);
    ASSERT_EQ(restored.size(), 2U);
    EXPECT_EQ(restored.back().kind, EntryKind::character);
    EXPECT_EQ(
        restored.back().status,
        EntryStatus::cancelled);
    EXPECT_EQ(restored.back().text, "Partial");
}

TEST(SessionController, StoresTheEntryCreationTimeItDisplayed) {
    TemporaryJournal temporary;
    std::atomic_bool release{false};
    auto backend = std::make_unique<ScriptedBackend>(
        GenerationResult{}, std::vector<std::string>{"Hello"});
    backend->hold_after_deltas = &release;
    auto controller = test::from_test_backends(
        test::one_backend(std::move(backend)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("operator", "Question");
    receive_until_entry_count(*controller, 2);
    const std::int64_t opened_at =
        copy_entries(controller->view().transcript).back().created_at;
    ASSERT_NE(opened_at, 0);

    // Force the journal record onto the next second so a reused stamp is the
    // only way the stored value can still match the live entry.
    wait_until_next_unix_second();
    release.store(true, std::memory_order_release);
    (void)receive_until_idle(*controller);

    const auto live = copy_entries(controller->view().transcript);
    const std::vector<TranscriptEntry> stored =
        load_transcript_entries(temporary.path);
    ASSERT_EQ(stored.size(), 2U);
    ASSERT_EQ(live.size(), 2U);
    EXPECT_EQ(live.back().created_at, opened_at);
    EXPECT_EQ(stored.back().created_at, opened_at);
    EXPECT_EQ(stored.front().created_at, live.front().created_at);
}

TEST(SessionController, StoresTheDisplayedCreationTimeForACancelledResponse) {
    TemporaryJournal temporary;
    std::atomic_bool release{false};
    auto backend = std::make_unique<ScriptedBackend>(
        GenerationResult{GenerationOutcome::cancelled, {}},
        std::vector<std::string>{"Partial"});
    backend->hold_after_deltas = &release;
    auto controller = test::from_test_backends(
        test::one_backend(std::move(backend)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("operator", "Question");
    receive_until_entry_count(*controller, 2);
    const std::int64_t opened_at =
        copy_entries(controller->view().transcript).back().created_at;
    ASSERT_NE(opened_at, 0);

    wait_until_next_unix_second();
    release.store(true, std::memory_order_release);
    (void)receive_until_idle(*controller);

    const auto live = copy_entries(controller->view().transcript);
    const std::vector<TranscriptEntry> stored =
        load_transcript_entries(temporary.path);
    ASSERT_EQ(stored.size(), 2U);
    ASSERT_EQ(live.size(), 2U);
    EXPECT_EQ(live.back().status, EntryStatus::cancelled);
    EXPECT_EQ(live.back().created_at, opened_at);
    EXPECT_EQ(stored.back().created_at, opened_at);
}

TEST(SessionController, RecordsCancellationWithoutAnEmptyAssistantEntry) {
    TemporaryJournal temporary;
    auto controller = test::from_test_backends(
        test::one_backend(std::make_unique<ScriptedBackend>(
            GenerationResult{GenerationOutcome::cancelled, {}})),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("operator", "Question");
    receive_until_idle(*controller);

    const auto entries = copy_entries(controller->view().transcript);
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().kind, EntryKind::human);
    EXPECT_EQ(load_transcript_entries(temporary.path), entries);
}

TEST(SessionController, KeepsReasoningEphemeralWhileAnswerEntersTranscript) {
    TemporaryJournal temporary;
    auto controller = test::from_test_backends(
        test::one_backend(std::make_unique<ScriptedBackend>(
            GenerationResult{},
            std::vector<std::string>{},
            true)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("operator", "Question");
    EXPECT_EQ(
        controller->view().generation.phase,
        ResponsePhase::waiting);

    (void)controller->handle_generation_event(GenerationEventDelta{
        1,
        GenerationDeltaKind::reasoning,
        "PRIVATE_REASONING",
    });
    EXPECT_EQ(
        controller->view().generation.phase,
        ResponsePhase::reasoning);
    EXPECT_EQ(
        controller->view().generation.reasoning_text,
        "PRIVATE_REASONING");
    ASSERT_EQ(controller->view().transcript.entries.size(), 1U);

    (void)controller->handle_generation_event(GenerationEventDelta{
        1,
        GenerationDeltaKind::answer,
        "Answer",
    });
    EXPECT_EQ(
        controller->view().generation.phase,
        ResponsePhase::answering);
    ASSERT_EQ(controller->view().transcript.entries.size(), 2U);
    EXPECT_EQ(controller->view().transcript.entries.back().text, "Answer");
    (void)controller->handle_generation_event(GenerationEventDelta{
        1,
        GenerationDeltaKind::reasoning,
        " late",
    });
    EXPECT_EQ(
        controller->view().generation.phase,
        ResponsePhase::answering);
    EXPECT_EQ(
        controller->view().generation.reasoning_text,
        "PRIVATE_REASONING late");

    (void)controller->handle_generation_event(GenerationCompleted{1});
    EXPECT_FALSE(controller->is_generating());
    EXPECT_TRUE(controller->view().generation.reasoning_text.empty());
    const std::vector<TranscriptEntry> live =
        copy_entries(controller->view().transcript);
    ASSERT_EQ(live.size(), 2U);
    EXPECT_EQ(live.back().text, "Answer");
    EXPECT_EQ(live.back().status, EntryStatus::complete);

    const std::vector<TranscriptEntry> restored =
        load_transcript_entries(temporary.path);
    EXPECT_EQ(restored, live);
}

TEST(SessionController, ReasoningOnlyCancellationLeavesNoTranscriptEntry) {
    TemporaryJournal temporary;
    auto controller = test::from_test_backends(
        test::one_backend(std::make_unique<ScriptedBackend>(
            GenerationResult{},
            std::vector<std::string>{},
            true)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("operator", "Question");
    (void)controller->handle_generation_event(GenerationEventDelta{
        1,
        GenerationDeltaKind::reasoning,
        "EPHEMERAL_REASONING_ONLY",
    });
    EXPECT_EQ(
        controller->view().generation.reasoning_text,
        "EPHEMERAL_REASONING_ONLY");
    ASSERT_EQ(controller->view().transcript.entries.size(), 1U);
    (void)controller->request_stop();
    receive_until_idle(*controller);
    EXPECT_TRUE(controller->view().generation.reasoning_text.empty());

    const std::vector<TranscriptEntry> live =
        copy_entries(controller->view().transcript);
    ASSERT_EQ(live.size(), 1U);
    EXPECT_EQ(live.front().kind, EntryKind::human);

    const std::vector<TranscriptEntry> restored =
        load_transcript_entries(temporary.path);
    EXPECT_EQ(restored, live);
}

TEST(SessionController, RejectsGenerationWithoutResponseContent) {
    TemporaryJournal temporary;
    auto controller = test::from_test_backends(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("operator", "Question");
    const ControllerUpdate update =
        receive_until_idle(*controller);

    ASSERT_TRUE(update.notice);
    EXPECT_EQ(*update.notice, "Generation failed");
    const auto entries = copy_entries(controller->view().transcript);
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries.back().kind, EntryKind::error);
    EXPECT_EQ(
        entries.back().text,
        "Generation finished without answer content");
    EXPECT_EQ(load_transcript_entries(temporary.path), entries);
}

TEST(SessionController, PersistsPreparationFailureAsTheTurnOutcome) {
    TemporaryJournal temporary;
    auto backend = std::make_unique<ThrowingPrepareBackend>();
    ThrowingPrepareBackend* backend_view = backend.get();
    auto controller = test::from_test_backends(
        test::one_backend(std::move(backend)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("operator", "Question");
    const ControllerUpdate update = receive_until_idle(*controller);

    EXPECT_EQ(update.notice, "Generation failed");
    EXPECT_FALSE(backend_view->performed);
    const std::vector<TranscriptEntry> entries =
        copy_entries(controller->view().transcript);
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries.front().kind, EntryKind::human);
    EXPECT_EQ(entries.back().kind, EntryKind::error);
    EXPECT_EQ(entries.back().text, "preparation failed");
    EXPECT_EQ(load_transcript_entries(temporary.path), entries);
}

TEST(SessionController, ReplacesPartialOutputWithATypedError) {
    TemporaryJournal temporary;
    auto controller = test::from_test_backends(
        test::one_backend(std::make_unique<ScriptedBackend>(
            GenerationResult{
                GenerationOutcome::transport_error,
                "network unavailable",
            },
            std::vector<std::string>{"Discard me"})),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("operator", "Question");
    receive_until_idle(*controller);

    const auto entries = copy_entries(controller->view().transcript);
    ASSERT_EQ(entries.size(), 2U);
    const TranscriptEntry& error = entries.back();
    EXPECT_EQ(error.kind, EntryKind::error);
    EXPECT_EQ(error.display_name, "Error");
    EXPECT_EQ(error.participant_id, "guide-id");
    EXPECT_EQ(error.text, "network unavailable");
    EXPECT_EQ(load_transcript_entries(temporary.path), entries);
}

TEST(SessionController, OwnsClearAndInformationSemantics) {
    TemporaryJournal temporary;
    const TranscriptEntry existing =
        test::human_entry(1, {"operator", "You"}, {"guide-id", "Guide"}, "Existing", 1);
    {
        SessionJournal journal(temporary.path);
        journal.start_turn(1, existing);
        journal.cancel_turn(1, std::nullopt);
    }
    auto controller = test::from_test_backends(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        temporary.path,
        notifier(),
        restore_with({existing}, 2, 2));

    const ControllerUpdate cleared =
        controller->clear_transcript();
    EXPECT_EQ(cleared.notice, "Transcript cleared");
    EXPECT_TRUE(controller->view().transcript.entries.empty());
    EXPECT_TRUE(load_transcript_entries(temporary.path).empty());

    const ControllerUpdate info = controller->session_information();
    ASSERT_TRUE(info.notice);
    EXPECT_NE(
        info.notice->find("Transcript entries: 0"),
        std::string::npos);
    EXPECT_NE(
        info.notice->find(
            "* @Guide  test-model  http://127.0.0.1:1/v1/responses  streaming"),
        std::string::npos);
    EXPECT_EQ(info.notice->find("Model:"), std::string::npos);
    EXPECT_TRUE(controller->view().transcript.entries.empty());
    EXPECT_TRUE(load_transcript_entries(temporary.path).empty());

}

TEST(SessionController, KeepsOffrecordMarkersOutOfTheSessionDatabase) {
    TemporaryJournal temporary;
    auto controller = test::from_test_backends(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        temporary.path,
        notifier());

    const ControllerUpdate no_span = controller->extend_offrecord();
    EXPECT_EQ(no_span.notice, "No off-record span to extend");
    EXPECT_TRUE(no_span.input_consumed);
    const ControllerUpdate opened = controller->open_offrecord();
    EXPECT_TRUE(has_state_update(opened));
    EXPECT_TRUE(opened.input_consumed);
    EXPECT_FALSE(opened.notice);
    const ControllerUpdate already_open = controller->open_offrecord();
    EXPECT_EQ(already_open.notice,
              "Already off the record; use /hide-off first");
    EXPECT_TRUE(already_open.input_consumed);
    EXPECT_TRUE(has_state_update(controller->extend_offrecord()));
    EXPECT_TRUE(has_state_update(controller->restore_offrecord()));
    const ControllerUpdate nothing_to_restore = controller->restore_offrecord();
    EXPECT_EQ(nothing_to_restore.notice, "Nothing to restore");
    EXPECT_TRUE(nothing_to_restore.input_consumed);

    EXPECT_EQ(
        copy_entries(controller->view().transcript),
        (std::vector<TranscriptEntry>{
            make_hide_on_marker(1),
            make_hide_marker(2),
            make_hide_off_marker(3),
        }));
    EXPECT_TRUE(load_transcript_entries(temporary.path).empty());
}

TEST(SessionController, ExcludesAHiddenTurnFromTheNextRequestAndRestoresItLater) {
    TemporaryJournal temporary;
    auto backend = std::make_unique<ScriptedBackend>(
        GenerationResult{}, std::vector<std::string>{"Answer"});
    ScriptedBackend* backend_view = backend.get();
    auto controller = test::from_test_backends(
        test::one_backend(std::move(backend)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("operator", "Visible");
    receive_until_idle(*controller);
    (void)controller->open_offrecord();
    (void)controller->submit_prompt("operator", "Hidden");
    receive_until_idle(*controller);
    (void)controller->extend_offrecord();
    (void)controller->submit_prompt("operator", "Current");
    receive_until_idle(*controller);

    ASSERT_EQ(backend_view->model_contexts.size(), 3U);
    EXPECT_EQ(
        context_without_timestamp_metadata(
            backend_view->inputs[2], backend_view->system_prompt),
        (std::vector<ModelMessage>{
            operator_prompt("Visible"),
            {ModelRole::assistant, "Answer"},
            operator_prompt("Current"),
        }));

    (void)controller->restore_offrecord();
    (void)controller->submit_prompt("operator", "Restored");
    receive_until_idle(*controller);

    ASSERT_EQ(backend_view->model_contexts.size(), 4U);
    EXPECT_EQ(
        context_without_timestamp_metadata(
            backend_view->inputs[3], backend_view->system_prompt),
        (std::vector<ModelMessage>{
            operator_prompt("Visible"),
            {ModelRole::assistant, "Answer"},
            operator_prompt("Hidden"),
            {ModelRole::assistant, "Answer"},
            operator_prompt("Current"),
            {ModelRole::assistant, "Answer"},
            operator_prompt("Restored"),
        }));
}

TEST(SessionController, RejectsOffrecordCommandsWhileActiveAndClearResetsTheSpan) {
    TemporaryJournal busy_temporary;
    auto busy_controller = test::from_test_backends(
        test::one_backend(std::make_unique<ScriptedBackend>(
            GenerationResult{}, std::vector<std::string>{}, true)),
        busy_temporary.path,
        notifier());
    (void)busy_controller->submit_prompt("operator", "Question");

    EXPECT_EQ(busy_controller->open_offrecord().notice, generation_in_progress_notice);
    EXPECT_EQ(busy_controller->extend_offrecord().notice, generation_in_progress_notice);
    EXPECT_EQ(busy_controller->restore_offrecord().notice, generation_in_progress_notice);
    busy_controller->shutdown();

    TemporaryJournal clear_temporary;
    auto clear_controller = test::from_test_backends(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        clear_temporary.path,
        notifier());
    EXPECT_TRUE(has_state_update(clear_controller->open_offrecord()));
    EXPECT_EQ(clear_controller->clear_transcript().notice, "Transcript cleared");
    EXPECT_EQ(clear_controller->extend_offrecord().notice, "No off-record span to extend");
    EXPECT_TRUE(clear_controller->view().transcript.entries.empty());
}

TEST(SessionController, MulticastCommitsTargetsInOrderWithIsolatedContexts) {
    TemporaryJournal temporary;
    auto one = std::make_unique<ScriptedBackend>(
        GenerationResult{}, std::vector<std::string>{"One answer"}, false,
        "one-id", "One");
    auto two = std::make_unique<ScriptedBackend>(
        GenerationResult{}, std::vector<std::string>{"Two answer"}, false,
        "two-id", "Two");
    ScriptedBackend* one_view = one.get();
    ScriptedBackend* two_view = two.get();
    std::vector<std::unique_ptr<test::DescribedModelBackend>> backends;
    backends.push_back(std::move(one));
    backends.push_back(std::move(two));
    auto controller = test::from_test_backends(
        std::move(backends), temporary.path, notifier());

    const ControllerUpdate started = controller->start_multicast("operator",
        "What time is it?",
        {"One", "Two"});
    EXPECT_TRUE(started.input_consumed);
    EXPECT_TRUE(controller->is_generating());

    const ControllerUpdate finished = receive_until_idle(*controller);
    EXPECT_TRUE(has_state_update(finished));
    EXPECT_FALSE(finished.session_ended);
    EXPECT_FALSE(controller->is_generating());

    ASSERT_EQ(one_view->inputs.size(), 1U);
    ASSERT_EQ(two_view->inputs.size(), 1U);
    EXPECT_EQ(one_view->inputs.front().run.target.id, "one-id");
    EXPECT_EQ(two_view->inputs.front().run.target.id, "two-id");
    EXPECT_EQ(one_view->inputs.front().history, two_view->inputs.front().history);
    EXPECT_EQ(
        context_without_timestamp_metadata(
            one_view->inputs.front(), one_view->system_prompt),
        (std::vector<ModelMessage>{operator_prompt("What time is it?")}));
    EXPECT_EQ(
        context_without_timestamp_metadata(
            two_view->inputs.front(), two_view->system_prompt),
        (std::vector<ModelMessage>{operator_prompt("What time is it?")}));

    const std::vector<TranscriptEntry> multicast_entries =
        copy_entries(controller->view().transcript);
    ASSERT_EQ(multicast_entries.size(), 4U);
    EXPECT_EQ(multicast_entries[0].participant_id, "operator");
    EXPECT_EQ(multicast_entries[0].display_name, "Operator");
    EXPECT_EQ(multicast_entries[0].addressed_to, "one-id");
    EXPECT_EQ(multicast_entries[0].text, "What time is it?");
    EXPECT_EQ(multicast_entries[1].text, "One answer");
    EXPECT_EQ(multicast_entries[2].addressed_to, "two-id");
    EXPECT_EQ(multicast_entries[2].text, "What time is it?");
    EXPECT_EQ(multicast_entries[3].text, "Two answer");

    (void)controller->submit_prompt("operator", "Follow-up");
    receive_until_idle(*controller);
    ASSERT_EQ(one_view->inputs.size(), 2U);
    EXPECT_EQ(
        one_view->inputs.back().history->entries,
        multicast_entries);
    EXPECT_EQ(
        load_transcript_entries(temporary.path),
        copy_entries(controller->view().transcript));
}

TEST(SessionController, ResolvesMulticastHandlesAndTreatsAnEmptyListAsAllCharacters) {
    TemporaryJournal temporary;
    auto one = std::make_unique<ScriptedBackend>(
        GenerationResult{}, std::vector<std::string>{"One answer"}, false,
        "one-id", "One");
    auto two = std::make_unique<ScriptedBackend>(
        GenerationResult{}, std::vector<std::string>{"Two answer"}, false,
        "two-id", "Two");
    ScriptedBackend* const one_view = one.get();
    ScriptedBackend* const two_view = two.get();
    std::vector<std::unique_ptr<test::DescribedModelBackend>> backends;
    backends.push_back(std::move(one));
    backends.push_back(std::move(two));
    auto controller = test::from_test_backends(
        std::move(backends), temporary.path, notifier());

    EXPECT_EQ(
        controller->start_multicast("operator", "Question", {"missing"}).notice,
        "Unknown character @missing. Characters in this forum: @One, @Two");
    EXPECT_EQ(
        controller->start_multicast("operator", "Question", {"One", "One"}).notice,
        "Multicast target @One is duplicated");
    EXPECT_TRUE(controller->view().transcript.entries.empty());

    const ControllerUpdate started =
        controller->start_multicast("operator", "Question", {});
    EXPECT_TRUE(started.input_consumed);
    receive_until_idle(*controller);
    EXPECT_EQ(one_view->inputs.size(), 1U);
    EXPECT_EQ(two_view->inputs.size(), 1U);
}

TEST(SessionController, MulticastRefusesOffrecordAndStopPreventsNextActivation) {
    TemporaryJournal span_temporary;
    auto span_controller = test::from_test_backends(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        span_temporary.path,
        notifier());
    (void)span_controller->open_offrecord();
    EXPECT_EQ(
        span_controller->start_multicast("operator", "Question", {"Guide"}).notice,
        "Cannot start multicast while an off-record span is active");

    TemporaryJournal stop_temporary;
    auto first = std::make_unique<ScriptedBackend>(
        GenerationResult{}, std::vector<std::string>{}, true, "one-id", "One");
    auto second = std::make_unique<ScriptedBackend>(
        GenerationResult{}, std::vector<std::string>{"Two answer"}, false,
        "two-id", "Two");
    ScriptedBackend* second_view = second.get();
    std::vector<std::unique_ptr<test::DescribedModelBackend>> backends;
    backends.push_back(std::move(first));
    backends.push_back(std::move(second));
    auto stop_controller = test::from_test_backends(
        std::move(backends), stop_temporary.path, notifier());

    (void)stop_controller->start_multicast("operator",
        "Question", {"One", "Two"});
    EXPECT_EQ(stop_controller->submit_prompt("operator", "Another").notice,
              generation_in_progress_notice);
    EXPECT_EQ(stop_controller->clear_transcript().notice,
              generation_in_progress_notice);
    EXPECT_EQ(stop_controller->open_offrecord().notice,
              generation_in_progress_notice);
    EXPECT_EQ(stop_controller->extend_offrecord().notice,
              generation_in_progress_notice);
    EXPECT_EQ(stop_controller->restore_offrecord().notice,
              generation_in_progress_notice);
    EXPECT_EQ(
        stop_controller->start_multicast("operator", "Again", {"One"}).notice,
        generation_in_progress_notice);
    EXPECT_EQ(stop_controller->request_stop().notice, "Stopping generation...");
    const ControllerGenerationView stopping =
        stop_controller->view().generation;
    EXPECT_TRUE(stopping.active);
    EXPECT_EQ(stopping.character_display_name, "One");
    EXPECT_EQ(stopping.phase, ResponsePhase::stopping);
    const ControllerUpdate stopped = receive_until_idle(*stop_controller);
    EXPECT_EQ(stopped.notice, "Generation stopped");
    EXPECT_FALSE(stop_controller->is_generating());
    // Cancellation may win before the background worker enters prepare(), or
    // it may race with already-started provider work. Neither case activates
    // the child's durable turn.
    EXPECT_LE(second_view->inputs.size(), 1U);
    const std::vector<TranscriptEntry> entries =
        copy_entries(stop_controller->view().transcript);
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().addressed_to, "one-id");
    stop_controller->shutdown();
}

TEST(SessionController, CompletedForegroundWinsTheStopRace) {
    TemporaryJournal temporary;
    auto first = std::make_unique<ConcurrentBackend>(
        "one-id", "One", "One answer");
    auto second = std::make_unique<ConcurrentBackend>(
        "two-id", "Two", "Two answer");
    ConcurrentBackend* const first_view = first.get();
    std::vector<std::unique_ptr<test::DescribedModelBackend>> backends;
    backends.push_back(std::move(first));
    backends.push_back(std::move(second));
    auto controller = test::from_test_backends(
        std::move(backends), temporary.path, notifier());

    (void)controller->start_multicast("operator", "Question", {"One", "Two"});
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!first_view->finished.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ASSERT_TRUE(first_view->finished.load(std::memory_order_acquire));

    EXPECT_EQ(controller->request_stop().notice, "Stopping generation...");
    const ControllerUpdate stopped = receive_until_idle(*controller);

    EXPECT_EQ(stopped.notice, "Generation stopped");
    const std::vector<TranscriptEntry> entries =
        copy_entries(controller->view().transcript);
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].addressed_to, "one-id");
    EXPECT_EQ(entries[1].text, "One answer");
    EXPECT_EQ(entries[1].status, EntryStatus::complete);
    EXPECT_EQ(load_transcript_entries(temporary.path), entries);
}

TEST(SessionController, StopDoesNotWaitForCancelledBackgroundExecution) {
    TemporaryJournal temporary;
    std::atomic_bool release_background{};
    auto foreground = std::make_unique<ConcurrentBackend>(
        "one-id", "One", "unused");
    auto background = std::make_unique<CancellationBlockingBackend>(
        "two-id", "Two", release_background);
    ConcurrentBackend* foreground_view = foreground.get();
    CancellationBlockingBackend* background_view = background.get();
    std::vector<std::unique_ptr<test::DescribedModelBackend>> backends;
    backends.push_back(std::move(foreground));
    backends.push_back(std::move(background));
    auto controller = test::from_test_backends(
        std::move(backends), temporary.path, notifier());

    (void)controller->start_multicast("operator", "Question", {"One", "Two"});
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while ((!foreground_view->entered.load(std::memory_order_acquire)
            || !background_view->entered.load(std::memory_order_acquire))
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const bool both_entered =
        foreground_view->entered.load(std::memory_order_acquire)
        && background_view->entered.load(std::memory_order_acquire);

    const auto started = std::chrono::steady_clock::now();
    const ControllerUpdate stopping = controller->request_stop();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_TRUE(both_entered);
    EXPECT_LT(elapsed, std::chrono::milliseconds(50));
    EXPECT_EQ(stopping.notice, "Stopping generation...");
    receive_until_idle(*controller);

    // The cancelled second target is still registered and intentionally held
    // in its transport tail, but it no longer occupies this session's visible
    // generation state. A new foreground request may start immediately.
    const ControllerUpdate restarted =
        controller->submit_prompt("operator", "New question");
    EXPECT_TRUE(restarted.input_consumed);
    receive_until_idle(*controller);
    release_background.store(true, std::memory_order_release);
}

TEST(SessionController, NewRequestOverlapsDiscardedWorkerForTheSameCharacter) {
    TemporaryJournal temporary;
    std::atomic_bool release_first{};
    auto first = std::make_unique<ConcurrentBackend>(
        "one-id", "One", "First answer");
    auto second = std::make_unique<OverlappingBackend>(release_first);
    OverlappingBackend* second_view = second.get();
    std::vector<std::unique_ptr<test::DescribedModelBackend>> backends;
    backends.push_back(std::move(first));
    backends.push_back(std::move(second));
    auto controller = test::from_test_backends(
        std::move(backends), temporary.path, notifier(), {}, {},
        std::optional<ParticipantId>{"two-id"});
    struct ReleaseOnExit {
        std::atomic_bool& flag;
        ~ReleaseOnExit() { flag.store(true, std::memory_order_release); }
    } release_on_exit{release_first};

    (void)controller->start_multicast("operator", "Question", {"One", "Two"});
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (second_view->calls.load(std::memory_order_acquire) != 1
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ASSERT_EQ(second_view->calls.load(std::memory_order_acquire), 1);

    (void)controller->request_stop();
    receive_until_idle(*controller);
    ASSERT_TRUE(second_view->first_cancelled.load(std::memory_order_acquire));

    const ControllerUpdate restarted =
        controller->submit_prompt("operator", "New question");
    EXPECT_TRUE(restarted.input_consumed);
    receive_until_idle(*controller);
    EXPECT_EQ(second_view->calls.load(std::memory_order_acquire), 2);

    release_first.store(true, std::memory_order_release);
    controller->shutdown();
}

TEST(SessionController, StopAcknowledgesAForegroundFailure) {
    TemporaryJournal temporary;
    std::atomic_bool release{};
    auto backend = std::make_unique<CancellationBlockingBackend>(
        "guide-id", "Guide", release,
        GenerationResult{GenerationOutcome::transport_error, "Unavailable"});
    CancellationBlockingBackend* const backend_view = backend.get();
    auto controller = test::from_test_backends(
        test::one_backend(std::move(backend)), temporary.path, notifier());

    (void)controller->submit_prompt("operator", "Question");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!backend_view->entered.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ASSERT_TRUE(backend_view->entered.load(std::memory_order_acquire));

    EXPECT_EQ(controller->request_stop().notice, "Stopping generation...");
    release.store(true, std::memory_order_release);
    EXPECT_EQ(
        receive_until_idle(*controller).notice,
        "Generation failed\nGeneration stopped");
}

TEST(SessionController, MulticastContinuesAfterChildFailuresAndRetainsNotices) {
    TemporaryJournal temporary;
    auto failed = std::make_unique<ScriptedBackend>(
        GenerationResult{GenerationOutcome::transport_error, "Unavailable"},
        std::vector<std::string>{}, false, "one-id", "One");
    auto cancelled = std::make_unique<ScriptedBackend>(
        GenerationResult{GenerationOutcome::cancelled, {}},
        std::vector<std::string>{"Partial"}, false, "two-id", "Two");
    auto complete = std::make_unique<ScriptedBackend>(
        GenerationResult{}, std::vector<std::string>{"Three answer"}, false,
        "three-id", "Three");
    ScriptedBackend* failed_view = failed.get();
    ScriptedBackend* cancelled_view = cancelled.get();
    ScriptedBackend* complete_view = complete.get();
    std::vector<std::unique_ptr<test::DescribedModelBackend>> backends;
    backends.push_back(std::move(failed));
    backends.push_back(std::move(cancelled));
    backends.push_back(std::move(complete));
    auto controller = test::from_test_backends(
        std::move(backends), temporary.path, notifier());

    (void)controller->start_multicast("operator",
        "Question", {"One", "Two", "Three"});
    const ControllerUpdate finished = receive_until_idle(*controller);

    EXPECT_EQ(finished.notice, "Generation failed\nGeneration stopped");
    ASSERT_EQ(failed_view->inputs.size(), 1U);
    ASSERT_EQ(cancelled_view->inputs.size(), 1U);
    ASSERT_EQ(complete_view->inputs.size(), 1U);
    EXPECT_EQ(
        context_without_timestamp_metadata(
            complete_view->inputs.front(), complete_view->system_prompt),
        (std::vector<ModelMessage>{operator_prompt("Question")}));
    EXPECT_FALSE(controller->is_generating());
}

// The batch owns both the run and the queue for each slot, so a child that
// finishes early cannot have its output paired with another child's prompt.
TEST(SessionController, PairsEveryChildWithItsOwnSlotDespiteReversedGenerationOrder) {
    TemporaryJournal temporary;
    std::atomic_bool release_first{false};
    std::atomic_bool release_second{false};
    auto first = std::make_unique<ConcurrentBackend>(
        "one-id", "One", "One answer", &release_first);
    auto second = std::make_unique<ConcurrentBackend>(
        "two-id", "Two", "Two answer", &release_second);
    auto third = std::make_unique<ConcurrentBackend>(
        "three-id", "Three", "Three answer");
    ConcurrentBackend* const first_view = first.get();
    ConcurrentBackend* const second_view = second.get();
    ConcurrentBackend* const third_view = third.get();
    std::vector<std::unique_ptr<test::DescribedModelBackend>> backends;
    backends.push_back(std::move(first));
    backends.push_back(std::move(second));
    backends.push_back(std::move(third));
    auto controller = test::from_test_backends(
        std::move(backends), temporary.path, notifier());

    (void)controller->start_multicast(
        "operator", "Question", {"One", "Two", "Three"});

    // Generation order is the exact reverse of the selected foreground order.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    const auto wait_for_finish = [&deadline](const ConcurrentBackend& backend) {
        while (!backend.finished.load(std::memory_order_acquire)
               && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        return backend.finished.load(std::memory_order_acquire);
    };
    ASSERT_TRUE(wait_for_finish(*third_view));
    release_second.store(true, std::memory_order_release);
    ASSERT_TRUE(wait_for_finish(*second_view));
    // Nothing from the two finished children is visible yet.
    ASSERT_EQ(controller->view().transcript.entries.size(), 1U);
    release_first.store(true, std::memory_order_release);
    ASSERT_TRUE(wait_for_finish(*first_view));
    receive_until_idle(*controller);

    const std::vector<TranscriptEntry> entries =
        copy_entries(controller->view().transcript);
    ASSERT_EQ(entries.size(), 6U);
    const std::vector<std::string> expected_ids{"one-id", "two-id", "three-id"};
    const std::vector<std::string> expected_names{"One", "Two", "Three"};
    for (std::size_t child = 0; child < expected_ids.size(); ++child) {
        const TranscriptEntry& prompt = entries[child * 2];
        const TranscriptEntry& answer = entries[child * 2 + 1];
        const RequestId request_id = static_cast<RequestId>(child + 1);
        EXPECT_EQ(prompt.kind, EntryKind::human);
        EXPECT_EQ(prompt.addressed_to, expected_ids[child]);
        EXPECT_EQ(prompt.text, "Question");
        EXPECT_EQ(prompt.request_id, request_id);
        EXPECT_EQ(answer.kind, EntryKind::character);
        EXPECT_EQ(answer.participant_id, expected_ids[child]);
        EXPECT_EQ(answer.display_name, expected_names[child]);
        EXPECT_EQ(answer.text, expected_names[child] + " answer");
        EXPECT_EQ(answer.status, EntryStatus::complete);
        EXPECT_EQ(answer.request_id, request_id);
    }

    // Each backend saw exactly its own slot's request, whatever order the
    // provider work finished in.
    ASSERT_EQ(first_view->inputs.size(), 1U);
    ASSERT_EQ(second_view->inputs.size(), 1U);
    ASSERT_EQ(third_view->inputs.size(), 1U);
    EXPECT_EQ(first_view->inputs.front().run.request_id, 1U);
    EXPECT_EQ(second_view->inputs.front().run.request_id, 2U);
    EXPECT_EQ(third_view->inputs.front().run.request_id, 3U);
    EXPECT_EQ(load_transcript_entries(temporary.path), entries);
}

TEST(SessionController, PersistenceFailureIdentifiesTheRequestAndCharacter) {
    TemporaryJournal temporary;
    auto backend = std::make_unique<ScriptedBackend>();
    ScriptedBackend* backend_view = backend.get();
    auto controller = test::from_test_backends(
        test::one_backend(std::move(backend)),
        temporary.path,
        notifier());
    sqlite3* raw_blocker = nullptr;
    ASSERT_EQ(
        sqlite3_open_v2(
            utf8_path(temporary.path).c_str(),
            &raw_blocker,
            SQLITE_OPEN_READWRITE,
            nullptr),
        SQLITE_OK);
    const std::unique_ptr<sqlite3, decltype(&sqlite3_close_v2)> blocker(
        raw_blocker, &sqlite3_close_v2);
    ASSERT_EQ(
        sqlite3_exec(
            blocker.get(), "BEGIN EXCLUSIVE", nullptr, nullptr, nullptr),
        SQLITE_OK);

    std::string message;
    try {
        (void)controller->submit_prompt("operator", "Question");
    } catch (const std::runtime_error& error) {
        message = error.what();
    }

    EXPECT_EQ(
        sqlite3_exec(blocker.get(), "ROLLBACK", nullptr, nullptr, nullptr),
        SQLITE_OK);
    controller->shutdown();
    ASSERT_FALSE(message.empty());
    EXPECT_NE(
        message.find("Failed to persist start of request 1 for @Guide"),
        std::string::npos)
        << message;
    EXPECT_NE(message.find("Session database"), std::string::npos)
        << message;
    EXPECT_TRUE(backend_view->inputs.empty());
}

TEST(SessionController, FirstActivationFailureTearsDownEveryGatedExecution) {
    TemporaryJournal temporary;
    auto first = std::make_unique<ScriptedBackend>(
        GenerationResult{}, std::vector<std::string>{"One answer"}, false,
        "one-id", "One");
    auto second = std::make_unique<ScriptedBackend>(
        GenerationResult{}, std::vector<std::string>{"Two answer"}, false,
        "two-id", "Two");
    ScriptedBackend* const first_view = first.get();
    ScriptedBackend* const second_view = second.get();
    std::vector<std::unique_ptr<test::DescribedModelBackend>> backends;
    backends.push_back(std::move(first));
    backends.push_back(std::move(second));
    bool fail_first_activation = true;
    auto controller = test::from_test_backends(
        std::move(backends),
        temporary.path,
        notifier(),
        {},
        [&fail_first_activation](std::size_t index) {
            if (index == 0 && fail_first_activation) {
                fail_first_activation = false;
                throw std::runtime_error("injected first activation failure");
            }
        });

    EXPECT_THROW(
        (void)controller->start_multicast("operator",
            "Question", {"One", "Two"}),
        std::runtime_error);
    EXPECT_FALSE(controller->is_generating());
    EXPECT_TRUE(controller->view().transcript.entries.empty());
    EXPECT_TRUE(first_view->inputs.empty());
    EXPECT_TRUE(second_view->inputs.empty());

    const ControllerUpdate restarted = controller->start_multicast("operator",
        "Retry", {"One", "Two"});
    EXPECT_TRUE(restarted.input_consumed);
    receive_until_idle(*controller);
    EXPECT_EQ(first_view->inputs.size(), 1U);
    EXPECT_EQ(second_view->inputs.size(), 1U);
}

TEST(SessionController, LaterActivationFailureCancelsAndReleasesEveryExecution) {
    TemporaryJournal temporary;
    auto first = std::make_unique<ScriptedBackend>(
        GenerationResult{}, std::vector<std::string>{"One answer"}, false,
        "one-id", "One");
    auto second = std::make_unique<ScriptedBackend>(
        GenerationResult{}, std::vector<std::string>{"Two answer"}, false,
        "two-id", "Two");
    ScriptedBackend* const first_view = first.get();
    ScriptedBackend* const second_view = second.get();
    std::vector<std::unique_ptr<test::DescribedModelBackend>> backends;
    backends.push_back(std::move(first));
    backends.push_back(std::move(second));
    bool fail_second_activation = true;
    auto controller = test::from_test_backends(
        std::move(backends),
        temporary.path,
        notifier(),
        {},
        [&fail_second_activation](std::size_t index) {
            if (index == 1 && fail_second_activation) {
                fail_second_activation = false;
                throw std::runtime_error("injected later activation failure");
            }
        });

    (void)controller->start_multicast("operator", "Question", {"One", "Two"});
    EXPECT_THROW(
        (void)receive_until_idle(*controller),
        std::runtime_error);
    EXPECT_FALSE(controller->is_generating());
    const std::vector<TranscriptEntry> entries =
        copy_entries(controller->view().transcript);
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].addressed_to, "one-id");
    EXPECT_EQ(entries[1].text, "One answer");

    const ControllerUpdate restarted = controller->start_multicast("operator",
        "Retry", {"One", "Two"});
    EXPECT_TRUE(restarted.input_consumed);
    receive_until_idle(*controller);
    EXPECT_GE(first_view->inputs.size(), 2U);
    EXPECT_GE(second_view->inputs.size(), 1U);
}

TEST(SessionController, RecordsAnInlineNullAgentMessageWithoutGeneration) {
    TemporaryJournal temporary;
    auto backend = std::make_unique<ScriptedBackend>();
    ScriptedBackend* const backend_view = backend.get();
    auto controller = test::from_test_backends(
        test::one_backend(std::move(backend)),
        temporary.path,
        notifier());

    const ControllerUpdate recorded =
        controller->submit_prompt("operator", "Thinking out loud", "-");
    EXPECT_TRUE(recorded.input_consumed);
    EXPECT_TRUE(requires_snapshot(recorded));
    ASSERT_TRUE(recorded.notice);
    EXPECT_EQ(*recorded.notice, "");
    EXPECT_TRUE(backend_view->inputs.empty());
    EXPECT_FALSE(controller->is_generating());

    const auto entries = copy_entries(controller->view().transcript);
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().kind, EntryKind::human);
    EXPECT_EQ(entries.front().participant_id, "operator");
    EXPECT_EQ(entries.front().display_name, "Operator");
    EXPECT_EQ(entries.front().addressed_to, null_agent_handle);
    EXPECT_EQ(entries.front().addressed_to_name, null_agent_name);
    EXPECT_EQ(entries.front().text, "Thinking out loud");
    EXPECT_FALSE(entries.front().request_id.has_value());
    EXPECT_EQ(load_transcript_entries(temporary.path), entries);
}

TEST(SessionController, RecordsPlainMessagesInSessionLocalRecordingMode) {
    TemporaryJournal temporary;
    auto backend = std::make_unique<ScriptedBackend>();
    ScriptedBackend* const backend_view = backend.get();
    auto controller = test::from_test_backends(
        test::one_backend(std::move(backend)),
        temporary.path,
        notifier());

    const ControllerUpdate mode = controller->set_default_character("-");
    EXPECT_TRUE(mode.input_consumed);
    EXPECT_TRUE(requires_snapshot(mode));
    ASSERT_TRUE(mode.notice);
    EXPECT_NE(mode.notice->find("Recording"), std::string::npos);
    EXPECT_EQ(controller->view().default_character_id, null_agent_handle);

    const ControllerUpdate first =
        controller->submit_prompt("operator", "First thought");
    const ControllerUpdate second =
        controller->submit_prompt("operator", "Second thought");
    EXPECT_TRUE(first.input_consumed);
    EXPECT_TRUE(second.input_consumed);
    EXPECT_TRUE(backend_view->inputs.empty());
    EXPECT_FALSE(controller->is_generating());

    const auto entries = copy_entries(controller->view().transcript);
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries.front().text, "First thought");
    EXPECT_EQ(entries.back().text, "Second thought");
    for (const TranscriptEntry& entry : entries) {
        EXPECT_EQ(entry.addressed_to, null_agent_handle);
        EXPECT_FALSE(entry.request_id.has_value());
    }
    EXPECT_EQ(load_transcript_entries(temporary.path), entries);

    // Switching back to a real character resumes normal dispatch.
    const ControllerUpdate resumed = controller->set_default_character("Guide");
    EXPECT_EQ(resumed.notice, "Default character is now Guide");
    EXPECT_EQ(controller->view().default_character_id, "guide-id");
    const ControllerUpdate answered =
        controller->submit_prompt("operator", "Question");
    EXPECT_TRUE(answered.input_consumed);
    receive_until_idle(*controller);
    ASSERT_EQ(backend_view->inputs.size(), 1U);
    EXPECT_EQ(backend_view->inputs.front().run.prompt_text, "Question");
}

TEST(SessionController, RejectsEmptyNullAgentMessagesWithoutRecording) {
    TemporaryJournal temporary;
    auto controller = test::from_test_backends(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        temporary.path,
        notifier());

    const ControllerUpdate empty_inline =
        controller->submit_prompt("operator", "", "-");
    EXPECT_EQ(empty_inline.notice, "Message to @- is empty");
    EXPECT_FALSE(empty_inline.input_consumed);
    EXPECT_TRUE(controller->view().transcript.entries.empty());

    // An empty plain submission in recording mode is a silent no-op, exactly
    // as in normal mode.
    (void)controller->set_default_character("-");
    const ControllerUpdate empty_plain =
        controller->submit_prompt("operator", "");
    EXPECT_FALSE(empty_plain.input_consumed);
    EXPECT_FALSE(empty_plain.notice.has_value());
    EXPECT_FALSE(has_state_update(empty_plain));
    EXPECT_TRUE(controller->view().transcript.entries.empty());
}

TEST(SessionController, RejectsNullAgentRecordingDuringGeneration) {
    TemporaryJournal temporary;
    auto controller = test::from_test_backends(
        test::one_backend(std::make_unique<ScriptedBackend>(
            GenerationResult{},
            std::vector<std::string>{},
            true)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("operator", "Question");
    const ControllerUpdate blocked =
        controller->submit_prompt("operator", "Thinking out loud", "-");
    EXPECT_FALSE(blocked.input_consumed);
    EXPECT_EQ(
        blocked.notice,
        "Generation in progress; use /stop, Esc, or Ctrl-C");
    EXPECT_TRUE(controller->view().transcript.entries.size() == 1U);

    (void)controller->request_stop();
    receive_until_idle(*controller);
}

TEST(SessionController, RejectsNewOperationsDuringGeneration) {
    TemporaryJournal temporary;
    auto controller = test::from_test_backends(
        test::one_backend(std::make_unique<ScriptedBackend>(
            GenerationResult{},
            std::vector<std::string>{},
            true)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("operator", "Question");
    const ControllerUpdate blocked =
        controller->submit_prompt("operator", "Another");
    EXPECT_FALSE(blocked.input_consumed);
    EXPECT_EQ(
        blocked.notice,
        "Generation in progress; use /stop, Esc, or Ctrl-C");
    const ControllerUpdate stopping =
        controller->request_stop();
    EXPECT_FALSE(stopping.input_consumed);
    EXPECT_EQ(stopping.notice, "Stopping generation...");
    receive_until_idle(*controller);
}

TEST(SessionController, IgnoresEventsForAnotherRequest) {
    TemporaryJournal temporary;
    auto controller = test::from_test_backends(
        test::one_backend(std::make_unique<ScriptedBackend>(
            GenerationResult{},
            std::vector<std::string>{},
            true)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("operator", "Question");
    const ControllerUpdate delta =
        controller->handle_generation_event(
            GenerationEventDelta{
                999,
                GenerationDeltaKind::answer,
                "Wrong response",
            });
    const ControllerUpdate completed =
        controller->handle_generation_event(
            GenerationCompleted{999});

    EXPECT_FALSE(has_state_update(delta));
    EXPECT_FALSE(has_state_update(completed));
    EXPECT_TRUE(controller->is_generating());
    const auto entries = copy_entries(controller->view().transcript);
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().kind, EntryKind::human);

    (void)controller->request_stop();
    receive_until_idle(*controller);
}

TEST(SessionController, StagingFailureLeavesNoDurableTurn) {
    TemporaryJournal temporary;
    auto controller = test::from_test_backends(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        temporary.path,
        notifier());
    controller->shutdown();

    const ControllerUpdate update =
        controller->submit_prompt("operator", "Question");

    EXPECT_EQ(update.notice, "Request could not be dispatched");
    EXPECT_FALSE(update.input_consumed);
    const auto restored =
        load_transcript_entries(temporary.path);
    EXPECT_TRUE(restored.empty());
    EXPECT_TRUE(controller->view().transcript.entries.empty());
}

TEST(SessionController, FinalizesInterruptedTurnsDuringRestore) {
    TemporaryJournal temporary;
    {
        SessionJournal journal(temporary.path);
        const TranscriptEntry prompt =
            test::human_entry(1, {"operator", "You"}, {"guide-id", "Guide"}, "Interrupted", 5);
        journal.start_turn(5, prompt);
    }
    SessionRestore restored =
        load_session_state(temporary.path);
    ASSERT_EQ(restored.interrupted_turns.size(), 1U);

    auto controller = test::from_test_backends(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        temporary.path,
        notifier(),
        std::move(restored));

    const SessionRestore repaired =
        load_session_state(temporary.path);
    EXPECT_TRUE(repaired.interrupted_turns.empty());
    ASSERT_EQ(repaired.entries.size(), 2U);
    EXPECT_EQ(repaired.entries.back().kind, EntryKind::error);
    EXPECT_NE(
        repaired.entries.back().text.find("interrupted"),
        std::string::npos);
}

TEST(SessionController, HonorsNonFirstInitialDefaultWithoutReorderingForumCharacters) {
    TemporaryJournal temporary;
    auto guide = std::make_unique<ScriptedBackend>(
        GenerationResult{}, std::vector<std::string>{"Guide answer"});
    auto ismael = std::make_unique<ScriptedBackend>(
        GenerationResult{}, std::vector<std::string>{"Ismael answer"}, false,
        "ismael-id", "Ismael");
    ScriptedBackend* guide_view = guide.get();
    ScriptedBackend* ismael_view = ismael.get();
    std::vector<std::unique_ptr<test::DescribedModelBackend>> backends;
    backends.push_back(std::move(guide));
    backends.push_back(std::move(ismael));
    auto controller = test::from_test_backends(
        std::move(backends),
        temporary.path,
        notifier(),
        {},
        {},
        "ismael-id",
        {"forum", "session"});

    EXPECT_EQ(controller->view().default_character_id, "ismael-id");
    ASSERT_EQ(controller->view().characters.size(), 2U);
    EXPECT_EQ(controller->view().characters[0].id, "guide-id");
    EXPECT_EQ(controller->view().characters[1].id, "ismael-id");

    (void)controller->submit_prompt("operator", "initial default");
    receive_until_idle(*controller);
    ASSERT_EQ(ismael_view->inputs.size(), 1U);
    EXPECT_EQ(ismael_view->inputs.back().run.target.id, "ismael-id");
    EXPECT_TRUE(guide_view->inputs.empty());

    (void)controller->start_multicast("operator", "all agents", {});
    receive_until_idle(*controller);
    ASSERT_EQ(guide_view->inputs.size(), 1U);
    ASSERT_EQ(ismael_view->inputs.size(), 2U);
    EXPECT_EQ(guide_view->inputs.back().run.target.id, "guide-id");
    EXPECT_EQ(ismael_view->inputs.back().run.target.id, "ismael-id");
    EXPECT_EQ(guide_view->inputs.back().run.prompt_cache_key, "forum/session/guide-id");
    EXPECT_EQ(ismael_view->inputs.back().run.prompt_cache_key, "forum/session/ismael-id");
    const std::vector<TranscriptEntry> entries_after_multicast =
        copy_entries(controller->view().transcript);
    ASSERT_EQ(entries_after_multicast.size(), 6U);
    EXPECT_EQ(entries_after_multicast[2].addressed_to, "guide-id");
    EXPECT_EQ(entries_after_multicast[4].addressed_to, "ismael-id");

    const ControllerUpdate mentioned =
        controller->submit_prompt("operator", "hello", "Ism");
    EXPECT_TRUE(mentioned.input_consumed);
    receive_until_idle(*controller);
    ASSERT_EQ(ismael_view->inputs.size(), 3U);
    EXPECT_EQ(ismael_view->inputs.back().run.target.id, "ismael-id");
    EXPECT_EQ(guide_view->inputs.size(), 1U);

    const ControllerUpdate default_changed =
        controller->set_default_character("Gui");
    EXPECT_TRUE(default_changed.input_consumed);
    EXPECT_EQ(default_changed.notice, "Default character is now Guide");
    (void)controller->submit_prompt("operator", "next");
    receive_until_idle(*controller);
    ASSERT_EQ(guide_view->inputs.size(), 2U);
    EXPECT_EQ(guide_view->inputs.back().run.target.id, "guide-id");

    const ControllerUpdate stable_id_changed =
        controller->set_default_character_by_id("ismael-id");
    EXPECT_FALSE(stable_id_changed.input_consumed);
    EXPECT_EQ(stable_id_changed.notice, "Default character is now Ismael");
    (void)controller->submit_prompt("operator", "by stable ID");
    receive_until_idle(*controller);
    ASSERT_EQ(ismael_view->inputs.size(), 4U);
    EXPECT_EQ(ismael_view->inputs.back().run.target.id, "ismael-id");

    const ControllerUpdate unknown_id =
        controller->set_default_character_by_id("unknown-id");
    EXPECT_FALSE(unknown_id.input_consumed);
    EXPECT_EQ(unknown_id.notice, "Unknown character");

    const std::size_t entries_before_rejection = controller->view().transcript.entries.size();
    const ControllerUpdate rejected =
        controller->submit_prompt("operator", "text", "nobody");
    EXPECT_FALSE(rejected.input_consumed);
    EXPECT_NE(rejected.notice->find("@nobody"), std::string::npos);
    EXPECT_EQ(controller->view().transcript.entries.size(), entries_before_rejection);

    const std::vector<TranscriptEntry> entries_before_agents =
        copy_entries(controller->view().transcript);
    const ControllerUpdate characters = controller->character_information();
    EXPECT_TRUE(characters.input_consumed);
    ASSERT_TRUE(characters.notice);
    EXPECT_NE(
        characters.notice->find("Any unambiguous prefix works."),
        std::string::npos);
    EXPECT_EQ(characters.notice->find("Cheburashka"), std::string::npos);
    EXPECT_NE(characters.notice->find("@Ismael"), std::string::npos);
    EXPECT_LT(characters.notice->find("@Guide"), characters.notice->find("@Ismael"));
    EXPECT_LT(characters.notice->find("* @Ismael"), characters.notice->find("@Ismael"));
    const ControllerUpdate info = controller->session_information();
    ASSERT_TRUE(info.notice);
    EXPECT_NE(info.notice->find("* @Ismael"), std::string::npos);
    EXPECT_EQ(
        copy_entries(controller->view().transcript),
        entries_before_agents);
    EXPECT_EQ(
        load_transcript_entries(temporary.path),
        entries_before_agents);
}

TEST(SessionController, ResolvesCurrentPersonaByIdOrDisplayNamePrefix) {
    TemporaryJournal temporary;
    auto controller = test::from_test_backends(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        PersonaRoster{
            {.id = "michael", .display_name = "Michael"},
            {.id = "michelle", .display_name = "Michelle"},
            {.id = "reader", .display_name = "Reader"},
        },
        temporary.path,
        notifier());

    const ControllerUpdate by_name = controller->set_default_persona("Rea");
    EXPECT_TRUE(by_name.input_consumed);
    EXPECT_TRUE(requires_snapshot(by_name));
    EXPECT_EQ(by_name.notice, "Current persona is now Reader");
    EXPECT_EQ(controller->view().default_persona_id, "reader");
    EXPECT_EQ(controller->view().default_persona_display_name, "Reader");

    const ControllerUpdate by_id = controller->set_default_persona("MICHAEL");
    EXPECT_EQ(by_id.notice, "Current persona is now Michael");
    EXPECT_EQ(controller->view().default_persona_id, "michael");

    // Re-selecting the current persona confirms it but requests no snapshot, so
    // the input route neither re-persists nor reloads for a non-change.
    const ControllerUpdate unchanged = controller->set_default_persona("michael");
    EXPECT_EQ(unchanged.notice, "Current persona is now Michael");
    EXPECT_FALSE(requires_snapshot(unchanged));
    EXPECT_EQ(controller->view().default_persona_id, "michael");

    EXPECT_EQ(
        controller->set_default_persona("mic").notice,
        "Ambiguous persona !mic: matches !Michael, !Michelle. Type more of the name.");
    EXPECT_EQ(
        controller->set_default_persona("nobody").notice,
        "Unknown persona !nobody. Personas in this workspace: !Michael, !Michelle, !Reader");
}

// Foreign-history addressing is a transcript concern; covered in persona_session/transcript tests.

TEST(SessionController, ShutdownCancelsAndPersistsAnActiveTurn) {
    TemporaryJournal temporary;
    auto controller = test::from_test_backends(
        test::one_backend(std::make_unique<ScriptedBackend>(
            GenerationResult{},
            std::vector<std::string>{"Partial"},
            true)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("operator", "Question");
    const ControllerUpdate partial = receive_when_ready(*controller);
    EXPECT_TRUE(has_state_update(partial));
    EXPECT_TRUE(controller->is_generating());
    controller->shutdown();

    EXPECT_FALSE(controller->is_generating());
    EXPECT_TRUE(test::receive_all_events(*controller).session_ended);
    const auto entries =
        load_transcript_entries(temporary.path);
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(
        entries.back().status,
        EntryStatus::cancelled);
    EXPECT_EQ(entries.back().text, "Partial");
}

TEST(SessionController, ClassifiesSuccessiveReasoningAndAnswerSuffixes) {
    TemporaryJournal temporary;
    auto controller = test::from_test_backends(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        temporary.path,
        notifier());

    // Starting a generation inserts the prompt and opens a request.
    EXPECT_TRUE(requires_snapshot(
        controller->submit_prompt("operator", "Question")));

    // The first reasoning chunk establishes visible request state.
    EXPECT_TRUE(requires_snapshot(controller->handle_generation_event(GenerationEventDelta{
        1, GenerationDeltaKind::reasoning, "Think",
    })));
    const ControllerUpdate more_reasoning =
        controller->handle_generation_event(GenerationEventDelta{
            1, GenerationDeltaKind::reasoning, " more",
        });
    ASSERT_NE(text_append(more_reasoning), nullptr);
    EXPECT_EQ(
        *text_append(more_reasoning),
        (TextAppend{ReasoningTextTarget{1}, " more"}));

    const ControllerUpdate careful_reasoning =
        controller->handle_generation_event(GenerationEventDelta{
            1, GenerationDeltaKind::reasoning, " carefully",
        });
    ASSERT_NE(text_append(careful_reasoning), nullptr);
    EXPECT_EQ(text_append(careful_reasoning)->text, " carefully");

    // The first answer chunk changes phase and opens the response entry.
    EXPECT_TRUE(requires_snapshot(controller->handle_generation_event(GenerationEventDelta{
        1, GenerationDeltaKind::answer, "Answer",
    })));
    const ControllerUpdate more_answer =
        controller->handle_generation_event(GenerationEventDelta{
            1, GenerationDeltaKind::answer, " again",
        });
    ASSERT_NE(text_append(more_answer), nullptr);
    EXPECT_EQ(
        *text_append(more_answer),
        (TextAppend{EntryTextTarget{2}, " again"}));

    // Reasoning arriving after the answer began is still a pure append; the
    // transport decides whether that target switch needs a snapshot.
    const ControllerUpdate late_reasoning =
        controller->handle_generation_event(GenerationEventDelta{
            1, GenerationDeltaKind::reasoning, " late",
        });
    ASSERT_NE(text_append(late_reasoning), nullptr);
    EXPECT_EQ(
        *text_append(late_reasoning),
        (TextAppend{ReasoningTextTarget{1}, " late"}));

    // Generation finalization closes the entry even though it also carries text.
    EXPECT_TRUE(
        requires_snapshot(controller->handle_generation_event(GenerationCompleted{1})));
}

TEST(SessionController, ClassifiesIgnoredAndAmbiguousDeltasConservatively) {
    TemporaryJournal temporary;
    auto controller = test::from_test_backends(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        temporary.path,
        notifier());

    // No request is active, so nothing is applied.
    EXPECT_FALSE(has_state_update(controller->handle_generation_event(GenerationEventDelta{
        1, GenerationDeltaKind::answer, "Orphan",
    })));

    (void)controller->submit_prompt("operator", "Question");
    // A mismatched request and an empty delta both leave state untouched.
    EXPECT_FALSE(has_state_update(controller->handle_generation_event(GenerationEventDelta{
        99, GenerationDeltaKind::answer, "Other request",
    })));
    EXPECT_FALSE(has_state_update(controller->handle_generation_event(GenerationEventDelta{
        1, GenerationDeltaKind::answer, "",
    })));

    // Cancellation and failure are terminal regardless of accumulated text.
    (void)controller->handle_generation_event(GenerationEventDelta{
        1, GenerationDeltaKind::answer, "Partial",
    });
    EXPECT_TRUE(
        requires_snapshot(controller->handle_generation_event(GenerationCancelled{1})));

    (void)controller->submit_prompt("operator", "Another question");
    EXPECT_TRUE(requires_snapshot(
        controller->handle_generation_event(GenerationFailed{2, "boom"})));
}

TEST(SessionController, CommandsRequestSnapshotsAndNoticesAloneDoNot) {
    TemporaryJournal temporary;
    auto controller = test::from_test_backends(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        temporary.path,
        notifier());

    // Informational commands change no retained state.
    const ControllerUpdate information = controller->session_information();
    EXPECT_FALSE(has_state_update(information));
    EXPECT_TRUE(information.input_consumed);
    ASSERT_TRUE(information.notice);
    EXPECT_FALSE(information.notice->empty());

    // A rejected default change is a notice without a state effect.
    const ControllerUpdate unknown = controller->set_default_character("nobody");
    EXPECT_FALSE(has_state_update(unknown));

    // An accepted one is structural.
    EXPECT_TRUE(requires_snapshot(controller->set_default_character("Guide")));
    EXPECT_TRUE(requires_snapshot(controller->set_default_character_by_id("guide-id")));
    EXPECT_TRUE(requires_snapshot(controller->clear_transcript()));
    EXPECT_TRUE(requires_snapshot(controller->open_offrecord()));
    EXPECT_TRUE(requires_snapshot(controller->extend_offrecord()));
    EXPECT_TRUE(requires_snapshot(controller->restore_offrecord()));

    // Nothing to stop is a notice only.
    EXPECT_FALSE(has_state_update(controller->request_stop()));
}

TEST(SessionController, ShutdownReturnsBeforeCancelledProviderUnregisters) {
    TemporaryJournal temporary;
    FinalWakeBlockingNotifier shutdown_notifier;
    // The backend stays inside perform() until shutdown cancels it, so the
    // notifier is still silent when the test arms the block below. The
    // execution's two wakes are then exactly its terminal event and its
    // generation finalization, and the blocked one is the generation wake.
    const std::atomic_bool never_released{false};
    auto backend = std::make_unique<ConcurrentBackend>(
        "guide-id", "Guide", "unused", &never_released);
    ConcurrentBackend* backend_view = backend.get();
    std::vector<std::unique_ptr<test::DescribedModelBackend>> backends;
    backends.push_back(std::move(backend));
    auto controller = test::from_test_backends(
        std::move(backends), temporary.path, shutdown_notifier);

    (void)controller->submit_prompt("operator", "Question");
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!backend_view->entered.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ASSERT_TRUE(backend_view->entered.load(std::memory_order_acquire));

    shutdown_notifier.block_final_wake();
    auto shutdown = std::async(std::launch::async, [&controller] {
        controller->shutdown();
    });

    const bool final_wake_blocked = shutdown_notifier.wait_for_final_wake();
    const std::future_status status = final_wake_blocked
        ? shutdown.wait_for(std::chrono::milliseconds(50))
        : std::future_status::deferred;
    shutdown_notifier.release_final_wake();

    ASSERT_TRUE(final_wake_blocked);
    EXPECT_EQ(status, std::future_status::ready);
    shutdown.get();
}

TEST(SessionController, ReportsSemanticStateAndInputConsumptionIndependently) {
    TemporaryJournal temporary;
    auto controller = test::from_test_backends(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        temporary.path,
        notifier());

    const ControllerUpdate unknown =
        controller->submit_prompt("unknown", "Question");
    EXPECT_FALSE(has_state_update(unknown));
    EXPECT_FALSE(unknown.input_consumed);

    const ControllerUpdate information = controller->session_information();
    EXPECT_FALSE(has_state_update(information));
    EXPECT_TRUE(information.input_consumed);
    ASSERT_TRUE(information.notice);

    const ControllerUpdate empty_handle = controller->set_default_character({});
    EXPECT_FALSE(has_state_update(empty_handle));
    EXPECT_TRUE(empty_handle.input_consumed);

    const ControllerUpdate typed_default =
        controller->set_default_character_by_id("guide-id");
    EXPECT_TRUE(has_state_update(typed_default));
    EXPECT_FALSE(typed_default.input_consumed);

    const ControllerUpdate submitted =
        controller->submit_prompt("operator", "Question");
    EXPECT_TRUE(has_state_update(submitted));
    EXPECT_TRUE(submitted.input_consumed);

    const ControllerEventBatch events = controller->receive_events(1);
    EXPECT_FALSE(events.update.input_consumed);

    controller->shutdown();
    const ControllerUpdate terminal = test::receive_all_events(*controller);
    EXPECT_TRUE(terminal.session_ended);
    EXPECT_FALSE(terminal.input_consumed);

    const ControllerUpdate undispatchable =
        controller->submit_prompt("operator", "Another question");
    EXPECT_FALSE(has_state_update(undispatchable));
    EXPECT_FALSE(undispatchable.input_consumed);
    EXPECT_EQ(undispatchable.notice, "Request could not be dispatched");
}

// --- Runtime style override --------------------------------------------------

CharacterDefinition provider_test_definition(
    std::string id,
    std::string name,
    std::string model = "configured-model") {
    return {
        .character = {
            .id = std::move(id),
            .display_name = std::move(name),
        },
        .provider = {.id = "test", .config = {
            .host = "127.0.0.1",
            .port = 1,
            .model = std::move(model),
        }},
        .system_prompt = "Test prompt",
    };
}

TEST(SessionController, ThreadLaunchFailureClosesTheCommittedTurn) {
    TemporaryJournal temporary;
    auto controller = test::from_definitions_for_testing(
        {provider_test_definition("guide-id", "Guide")},
        temporary.path,
        notifier(),
        {},
        std::nullopt,
        [](std::function<void()>) {
            throw std::runtime_error("thread creation failed");
        });

    const ControllerUpdate started = controller->submit_prompt("operator", "Question");
    EXPECT_TRUE(started.input_consumed);
    EXPECT_EQ(receive_until_idle(*controller).notice, "Generation failed");
    const std::vector<TranscriptEntry> entries = load_transcript_entries(temporary.path);
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries.front().text, "Question");
    EXPECT_EQ(entries.back().status, EntryStatus::failed);
}

// Shared observation of every backend one factory builds: the configs it was
// given, plus optional gates for the busy and factory-failure tests.
struct ProviderFactoryObservation {
    std::vector<ModelBackendConfig> configs;
    std::atomic_bool hold_perform{};
    std::atomic_bool entered_perform{};
    std::atomic_bool fail_next{};
};

// Answers with its configured model name, so the transcript says which
// backend served a prompt.
class ProviderFactoryBackend final : public ModelBackend {
public:
    ProviderFactoryBackend(
        SharedCharacterDefinition definition,
        std::shared_ptr<ProviderFactoryObservation> observation)
        : definition_(std::move(definition)), observation_(std::move(observation)) {
    }

    RequestPayload prepare(const GenerationRequest& input) override {
        return {.bytes = input.run.prompt_text};
    }

    GenerationResult perform(
        RequestPayload,
        const GenerationDeltaSink& on_delta,
        const std::atomic_bool& cancellation) override {
        observation_->entered_perform.store(true, std::memory_order_release);
        while (observation_->hold_perform.load(std::memory_order_acquire)
               && !cancellation.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        on_delta({GenerationDeltaKind::answer, "answer-" + definition_->provider.config.model});
        return {};
    }

private:
    SharedCharacterDefinition definition_;
    std::shared_ptr<ProviderFactoryObservation> observation_;
};

ProviderClientFactory provider_recording_factory(
    const std::shared_ptr<ProviderFactoryObservation>& observation) {
    return [observation](SharedCharacterDefinition definition) {
        if (observation->fail_next.exchange(false)) {
            throw std::runtime_error("backend construction failed");
        }
        observation->configs.push_back(definition->provider.config);
        return std::unique_ptr<ModelBackend>(
            new ProviderFactoryBackend(std::move(definition), observation));
    };
}

CharacterAppearance bold_appearance() {
    return {CharacterFont::sans, CharacterSlant::normal, CharacterWeight::bold,
            CharacterScale::normal};
}

// The distinct configured appearance every style_test_definition carries, so a
// reset is observable as a return to it rather than to the plain default.
CharacterAppearance style_configured_appearance() {
    return {CharacterFont::mono, CharacterSlant::italic, CharacterWeight::normal,
            CharacterScale::small};
}

SessionController::StyleResolver style_stub_resolver() {
    return [](std::string_view name) -> CharacterAppearance {
        if (name == "bold") return bold_appearance();
        throw std::invalid_argument(
            "Style '" + std::string(name)
            + "' is not usable: no style config is installed under this name. "
              "Available styles: bold");
    };
}

CharacterDefinition style_test_definition(std::string id, std::string name) {
    CharacterDefinition definition =
        provider_test_definition(std::move(id), std::move(name));
    definition.character.appearance = style_configured_appearance();
    return definition;
}

test::TestController style_test_controller(
    const std::filesystem::path& database_path,
    std::vector<CharacterDefinition> definitions,
    const std::shared_ptr<ProviderFactoryObservation>& observation) {
    return test::TestController(
        std::move(definitions),
        test::operator_roster(),
        "guide-id",
        database_path,
        std::shared_ptr<WakeNotifier>(&notifier(), [](WakeNotifier*) {}),
        {},
        provider_recording_factory(observation),
        {},
        {},
        style_stub_resolver());
}

CharacterAppearance appearance_in_view(
    SessionController& controller, std::string_view id) {
    for (const CharacterMetadata& character : controller.view().characters) {
        if (character.id == id) return character.appearance;
    }
    ADD_FAILURE() << "character not in view: " << id;
    return {};
}

TEST(SessionController, StyleOverrideSetsTheAppearanceForOneCharacterAndSnapshots) {
    TemporaryJournal temporary;
    auto observation = std::make_shared<ProviderFactoryObservation>();
    auto controller = style_test_controller(
        temporary.path,
        {
            style_test_definition("guide-id", "Guide"),
            style_test_definition("other-id", "Other"),
        },
        observation);

    const ControllerUpdate update = controller->set_session_style("bold");
    EXPECT_TRUE(update.input_consumed);
    EXPECT_TRUE(requires_snapshot(update));
    EXPECT_EQ(update.notice, "Guide now uses style 'bold' for this session.");
    EXPECT_EQ(appearance_in_view(*controller, "guide-id"), bold_appearance());
    EXPECT_EQ(appearance_in_view(*controller, "other-id"), style_configured_appearance())
        << "only the default character's appearance changes";
}

TEST(SessionController, StyleOverrideReportsAndResets) {
    TemporaryJournal temporary;
    auto observation = std::make_shared<ProviderFactoryObservation>();
    auto controller = style_test_controller(
        temporary.path,
        {style_test_definition("guide-id", "Guide")},
        observation);

    const ControllerUpdate report = controller->set_session_style("");
    EXPECT_FALSE(requires_snapshot(report)) << "the report form is notice-only";
    EXPECT_EQ(report.notice, "Guide is using its configured style for this session.");

    (void)controller->set_session_style("bold");
    EXPECT_EQ(
        controller->set_session_style("").notice,
        "Guide's style override for this session is 'bold'.");

    const ControllerUpdate reset = controller->set_session_style("default");
    EXPECT_TRUE(requires_snapshot(reset));
    EXPECT_EQ(reset.notice, "Guide is back to its configured style for this session.");
    EXPECT_EQ(appearance_in_view(*controller, "guide-id"), style_configured_appearance());
    EXPECT_EQ(
        controller->set_session_style("").notice,
        "Guide is using its configured style for this session.");
}

TEST(SessionController, StyleOverrideFollowsTheCharacterNotTheDefaultSlot) {
    TemporaryJournal temporary;
    auto observation = std::make_shared<ProviderFactoryObservation>();
    auto controller = style_test_controller(
        temporary.path,
        {
            style_test_definition("guide-id", "Guide"),
            style_test_definition("other-id", "Other"),
        },
        observation);

    (void)controller->set_session_style("bold");
    (void)controller->set_default_character("Other");
    EXPECT_EQ(
        controller->set_session_style("").notice,
        "Other is using its configured style for this session.");

    (void)controller->set_default_character("Guide");
    EXPECT_EQ(
        controller->set_session_style("").notice,
        "Guide's style override for this session is 'bold'.");
    EXPECT_EQ(appearance_in_view(*controller, "guide-id"), bold_appearance());
}

TEST(SessionController, StyleCommandsStayIdleWhileRecording) {
    TemporaryJournal temporary;
    auto observation = std::make_shared<ProviderFactoryObservation>();
    auto controller = style_test_controller(
        temporary.path,
        {style_test_definition("guide-id", "Guide")},
        observation);

    (void)controller->set_default_character("-");
    const char* const recording_notice =
        "No character is selected while recording. Use /@<name> to resume.";
    for (const char* name : {"", "default", "bold"}) {
        const ControllerUpdate update = controller->set_session_style(name);
        EXPECT_TRUE(update.input_consumed);
        EXPECT_FALSE(requires_snapshot(update));
        EXPECT_EQ(update.notice, recording_notice);
    }
    EXPECT_EQ(appearance_in_view(*controller, "guide-id"), style_configured_appearance());

    (void)controller->set_default_character("Guide");
    EXPECT_EQ(
        controller->set_session_style("").notice,
        "Guide is using its configured style for this session.");
    EXPECT_EQ(appearance_in_view(*controller, "guide-id"), style_configured_appearance());
}

TEST(SessionController, StyleResolverFailureLeavesTheAppearanceAlone) {
    TemporaryJournal temporary;
    auto observation = std::make_shared<ProviderFactoryObservation>();
    auto controller = style_test_controller(
        temporary.path,
        {style_test_definition("guide-id", "Guide")},
        observation);

    const ControllerUpdate unknown = controller->set_session_style("nope");
    EXPECT_FALSE(requires_snapshot(unknown));
    EXPECT_EQ(
        unknown.notice,
        "Style 'nope' is not usable: no style config is installed under this name. "
        "Available styles: bold");
    EXPECT_EQ(appearance_in_view(*controller, "guide-id"), style_configured_appearance());
    EXPECT_EQ(
        controller->set_session_style("").notice,
        "Guide is using its configured style for this session.");

    // The resolver reads the filesystem, so it can fail in ways its own contract
    // does not name. One mistyped style must still not fail the session.
    test::TestController throwing(
        std::vector<CharacterDefinition>{style_test_definition("guide-id", "Guide")},
        test::operator_roster(),
        "guide-id",
        temporary.path,
        std::shared_ptr<WakeNotifier>(&notifier(), [](WakeNotifier*) {}),
        {},
        provider_recording_factory(observation),
        {},
        {},
        [](std::string_view) -> CharacterAppearance {
            throw std::runtime_error("styles directory is unreadable");
        });
    EXPECT_EQ(
        throwing->set_session_style("bold").notice,
        "styles directory is unreadable");
    throwing->shutdown();
}

TEST(SessionController, StyleOverrideNeedsAResolver) {
    TemporaryJournal temporary;
    auto controller = test::from_test_backends(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        temporary.path,
        notifier());

    const ControllerUpdate update = controller->set_session_style("bold");
    EXPECT_TRUE(update.input_consumed);
    EXPECT_EQ(update.notice, "Style override is not available in this session.");
}

TEST(SessionController, ClearingTheTranscriptKeepsTheStyleOverride) {
    TemporaryJournal temporary;
    auto observation = std::make_shared<ProviderFactoryObservation>();
    auto controller = style_test_controller(
        temporary.path,
        {style_test_definition("guide-id", "Guide")},
        observation);

    (void)controller->set_session_style("bold");
    (void)controller->clear_transcript();
    EXPECT_EQ(appearance_in_view(*controller, "guide-id"), bold_appearance());
    EXPECT_EQ(
        controller->set_session_style("").notice,
        "Guide's style override for this session is 'bold'.");
}

TEST(SessionController, StyleOverrideSucceedsWhileGenerating) {
    TemporaryJournal temporary;
    auto observation = std::make_shared<ProviderFactoryObservation>();
    observation->hold_perform.store(true);
    auto controller = style_test_controller(
        temporary.path,
        {style_test_definition("guide-id", "Guide")},
        observation);

    (void)controller->submit_prompt("operator", "Question");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (!observation->entered_perform.load(std::memory_order_acquire)
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    ASSERT_TRUE(observation->entered_perform.load(std::memory_order_acquire));

    // No busy guard here: the web grammar's gate is what rejects the command
    // mid-generation. The typed action itself succeeds.
    const ControllerUpdate update = controller->set_session_style("bold");
    EXPECT_TRUE(requires_snapshot(update));
    EXPECT_EQ(update.notice, "Guide now uses style 'bold' for this session.");
    EXPECT_EQ(appearance_in_view(*controller, "guide-id"), bold_appearance());

    observation->hold_perform.store(false);
    (void)receive_until_idle(*controller);
}

} // namespace
} // namespace cha
