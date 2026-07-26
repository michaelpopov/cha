#include "session/session_controller.h"
#include "agents/agent.h"
#include "agents/completion_backend.h"
#include "session/session_database.h"
#include "support/test_backends.h"
#include "support/test_notifier.h"
#include "util/utf8_path.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace cha {
namespace {

test::TestNotifier& notifier() {
    static test::TestNotifier instance;
    return instance;
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

// Returns scripted completion output while retaining prompt-only requests for assertions.
class ScriptedBackend final : public CompletionBackend {
public:
    ScriptedBackend(
        CompletionResult result = {},
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
                CompletionDeltaKind::answer,
                std::move(delta),
            });
        }
    }

    ScriptedBackend(
        std::vector<CompletionDelta> deltas,
        CompletionResult result = {},
        bool wait_for_cancellation = false,
        std::string id = "guide-id",
        std::string name = "Guide")
      : id_(std::move(id)),
        name_(std::move(name)),
        result_(std::move(result)),
        deltas_(std::move(deltas)),
        wait_for_cancellation_(wait_for_cancellation) {
    }

    RequestPayload prepare(
        const CompletionRequest& request,
        const TranscriptReadView& transcript) override {
        requests.push_back(request);
        latest_prompt = transcript.entries().back();
        model_contexts.push_back(project_agent_context(
            transcript.entries(),
            transcript.open_entry_id(),
            {},
            id_));
        return {.bytes = request.prompt.text};
    }

    CompletionResult perform(
        RequestPayload,
        const CompletionDeltaSink& on_delta,
        const std::atomic_bool& cancellation) override {
        for (const CompletionDelta& delta : deltas_) {
            on_delta(delta);
        }
        if (wait_for_cancellation_) {
            while (!cancellation.load(std::memory_order_acquire)) {
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
                .name = name_,
            },
            .model = "test-model",
            .api = "test://completion",
            .streaming = true,
        };
    }

    std::vector<CompletionRequest> requests;
    std::vector<std::vector<AgentMessage>> model_contexts;
    TranscriptEntry latest_prompt;

private:
    std::string id_{"guide-id"};
    std::string name_{"Guide"};
    CompletionResult result_;
    std::vector<CompletionDelta> deltas_;
    bool wait_for_cancellation_{};
};

SessionUpdate receive_until_idle(SessionController& controller) {
    SessionUpdate combined;
    while (controller.generation_status().active) {
        const std::size_t observed = notifier().wake_count();
        const SessionUpdate update = controller.receive();
        combined.render_needed =
            combined.render_needed || update.render_needed;
        combined.end_session =
            combined.end_session || update.end_session;
        combined.clear_input =
            combined.clear_input || update.clear_input;
        if (update.notice) {
            combined.notice = update.notice;
        }
        if (controller.generation_status().active
            && !notifier().wait_for_wake(observed)) {
            throw std::runtime_error(
                "Timed out waiting for controller event");
        }
    }
    return combined;
}

SessionUpdate receive_when_ready(SessionController& controller) {
    while (true) {
        const std::size_t observed = notifier().wake_count();
        SessionUpdate update = controller.receive();
        if (update.render_needed || update.end_session || update.notice) {
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

TEST(SessionController, OwnsACompleteIdentifiedTypedTurn) {
    TemporaryJournal temporary;
    const TranscriptEntry earlier =
        make_human_entry(10, "guide-id", "Guide", "Earlier");
    {
        SessionJournal journal(temporary.path);
        journal.append(earlier);
    }
    auto backend = std::make_unique<ScriptedBackend>(
        CompletionResult{},
        std::vector<std::string>{"Hello", " there"});
    ScriptedBackend* backend_view = backend.get();
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::move(backend)),
        temporary.path,
        notifier(),
        restore_with({earlier}, 17, 11));

    const SessionUpdate submitted =
        controller->submit_prompt("Current");
    EXPECT_TRUE(submitted.render_needed);
    const SessionUpdate completed =
        receive_until_idle(*controller);

    ASSERT_EQ(backend_view->requests.size(), 1U);
    const CompletionRequest& request =
        backend_view->requests.front();
    EXPECT_EQ(request.request_id, 17U);
    EXPECT_EQ(request.prompt.addressed_to, "guide-id");
    EXPECT_EQ(request.prompt.kind, EntryKind::human);
    EXPECT_EQ(request.prompt.text, "Current");
    EXPECT_EQ(backend_view->latest_prompt, request.prompt);
    EXPECT_EQ(
        backend_view->model_contexts.front(),
        (std::vector<AgentMessage>{
            {AgentRole::user, "Earlier"},
            {AgentRole::user, "Current"},
        }));
    EXPECT_TRUE(completed.render_needed);

    const auto entries = controller->transcript().entries();
    ASSERT_EQ(entries.size(), 3U);
    const TranscriptEntry& response = entries.back();
    EXPECT_EQ(response.kind, EntryKind::agent);
    EXPECT_EQ(response.participant_id, "guide-id");
    EXPECT_EQ(response.display_name, "Guide");
    EXPECT_EQ(response.text, "Hello there");
    EXPECT_EQ(response.status, EntryStatus::complete);
    EXPECT_EQ(load_transcript_entries(temporary.path), entries);
}

TEST(SessionController, PreparesTheSecondTurnFromTheSharedCompletedTranscript) {
    TemporaryJournal temporary;
    auto backend = std::make_unique<ScriptedBackend>(
        CompletionResult{}, std::vector<std::string>{"Answer"});
    ScriptedBackend* backend_view = backend.get();
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::move(backend)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("First");
    receive_until_idle(*controller);
    (void)controller->submit_prompt("Second");
    receive_until_idle(*controller);

    ASSERT_EQ(backend_view->model_contexts.size(), 2U);
    EXPECT_EQ(
        backend_view->model_contexts[1],
        (std::vector<AgentMessage>{
            {AgentRole::user, "First"},
            {AgentRole::assistant, "Answer"},
            {AgentRole::user, "Second"},
        }));
}

TEST(SessionController, ClearMakesTheNextRequestSeeOnlyPostClearContext) {
    TemporaryJournal temporary;
    auto backend = std::make_unique<ScriptedBackend>(
        CompletionResult{}, std::vector<std::string>{"Answer"});
    ScriptedBackend* backend_view = backend.get();
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::move(backend)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("First");
    receive_until_idle(*controller);
    (void)controller->clear_transcript();
    (void)controller->submit_prompt("Second");
    receive_until_idle(*controller);

    ASSERT_EQ(backend_view->model_contexts.size(), 2U);
    EXPECT_EQ(
        backend_view->model_contexts[1],
        (std::vector<AgentMessage>{{AgentRole::user, "Second"}}));
}

TEST(SessionController, ExcludesFailedTurnsFromTheFollowingModelContext) {
    TemporaryJournal temporary;
    auto backend = std::make_unique<ScriptedBackend>(
        CompletionResult{CompletionOutcome::transport_error, "unavailable"});
    ScriptedBackend* backend_view = backend.get();
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::move(backend)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("Failed");
    receive_until_idle(*controller);
    (void)controller->submit_prompt("Second");
    receive_until_idle(*controller);

    ASSERT_EQ(backend_view->model_contexts.size(), 2U);
    EXPECT_EQ(
        backend_view->model_contexts[1],
        (std::vector<AgentMessage>{{AgentRole::user, "Second"}}));
}

TEST(SessionController, ExcludesCancelledPartialOutputFromFollowingModelContext) {
    TemporaryJournal temporary;
    auto backend = std::make_unique<ScriptedBackend>(
        CompletionResult{CompletionOutcome::cancelled, {}},
        std::vector<std::string>{"Partial"});
    ScriptedBackend* backend_view = backend.get();
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::move(backend)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("First");
    receive_until_idle(*controller);
    (void)controller->submit_prompt("Second");
    receive_until_idle(*controller);

    ASSERT_EQ(backend_view->model_contexts.size(), 2U);
    EXPECT_EQ(
        backend_view->model_contexts[1],
        (std::vector<AgentMessage>{
            {AgentRole::user, "First"},
            {AgentRole::user, "Second"},
        }));
}

TEST(SessionController, PersistsAnIdentifiedCancelledResponse) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>(
            CompletionResult{CompletionOutcome::cancelled, {}},
            std::vector<std::string>{"Partial"})),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("Question");
    const SessionUpdate update =
        receive_until_idle(*controller);

    ASSERT_TRUE(update.notice);
    EXPECT_EQ(*update.notice, "Generation stopped");
    const auto restored =
        load_transcript_entries(temporary.path);
    ASSERT_EQ(restored.size(), 2U);
    EXPECT_EQ(restored.back().kind, EntryKind::agent);
    EXPECT_EQ(
        restored.back().status,
        EntryStatus::cancelled);
    EXPECT_EQ(restored.back().text, "Partial");
}

TEST(SessionController, RecordsCancellationWithoutAnEmptyAssistantEntry) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>(
            CompletionResult{CompletionOutcome::cancelled, {}})),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("Question");
    receive_until_idle(*controller);

    const auto entries = controller->transcript().entries();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().kind, EntryKind::human);
    EXPECT_EQ(load_transcript_entries(temporary.path), entries);
}

TEST(SessionController, KeepsReasoningEphemeralWhileAnswerEntersTranscript) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>(
            CompletionResult{},
            std::vector<std::string>{},
            true)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("Question");
    EXPECT_EQ(
        controller->generation_status().phase,
        ResponsePhase::waiting);

    (void)controller->handle_agent_event(AgentDelta{
        1,
        CompletionDeltaKind::reasoning,
        "PRIVATE_REASONING",
    });
    EXPECT_EQ(
        controller->generation_status().phase,
        ResponsePhase::reasoning);
    EXPECT_EQ(
        controller->generation_status().reasoning_text,
        "PRIVATE_REASONING");
    ASSERT_EQ(controller->transcript().entries().size(), 1U);

    (void)controller->handle_agent_event(AgentDelta{
        1,
        CompletionDeltaKind::answer,
        "Answer",
    });
    EXPECT_EQ(
        controller->generation_status().phase,
        ResponsePhase::answering);
    ASSERT_EQ(controller->transcript().entries().size(), 2U);
    EXPECT_EQ(controller->transcript().entries().back().text, "Answer");
    (void)controller->handle_agent_event(AgentDelta{
        1,
        CompletionDeltaKind::reasoning,
        " late",
    });
    EXPECT_EQ(
        controller->generation_status().phase,
        ResponsePhase::answering);
    EXPECT_EQ(
        controller->generation_status().reasoning_text,
        "PRIVATE_REASONING late");

    (void)controller->handle_agent_event(AgentCompleted{1});
    EXPECT_FALSE(controller->generation_status().active);
    EXPECT_TRUE(controller->generation_status().reasoning_text.empty());
    const std::vector<TranscriptEntry> live =
        controller->transcript().entries();
    ASSERT_EQ(live.size(), 2U);
    EXPECT_EQ(live.back().text, "Answer");
    EXPECT_EQ(live.back().status, EntryStatus::complete);

    const std::vector<TranscriptEntry> restored =
        load_transcript_entries(temporary.path);
    EXPECT_EQ(restored, live);
}

TEST(SessionController, ReasoningOnlyCancellationLeavesNoTranscriptEntry) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>(
            CompletionResult{},
            std::vector<std::string>{},
            true)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("Question");
    (void)controller->handle_agent_event(AgentDelta{
        1,
        CompletionDeltaKind::reasoning,
        "EPHEMERAL_REASONING_ONLY",
    });
    EXPECT_EQ(
        controller->generation_status().reasoning_text,
        "EPHEMERAL_REASONING_ONLY");
    ASSERT_EQ(controller->transcript().entries().size(), 1U);
    (void)controller->request_stop();
    receive_until_idle(*controller);
    EXPECT_TRUE(controller->generation_status().reasoning_text.empty());

    const std::vector<TranscriptEntry> live =
        controller->transcript().entries();
    ASSERT_EQ(live.size(), 1U);
    EXPECT_EQ(live.front().kind, EntryKind::human);

    const std::vector<TranscriptEntry> restored =
        load_transcript_entries(temporary.path);
    EXPECT_EQ(restored, live);
}

TEST(SessionController, RejectsCompletionWithoutResponseContent) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("Question");
    const SessionUpdate update =
        receive_until_idle(*controller);

    ASSERT_TRUE(update.notice);
    EXPECT_EQ(*update.notice, "Generation failed");
    const auto entries = controller->transcript().entries();
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries.back().kind, EntryKind::error);
    EXPECT_EQ(
        entries.back().text,
        "Agent completed without answer content");
    EXPECT_EQ(load_transcript_entries(temporary.path), entries);
}

TEST(SessionController, ReplacesPartialOutputWithATypedError) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>(
            CompletionResult{
                CompletionOutcome::transport_error,
                "network unavailable",
            },
            std::vector<std::string>{"Discard me"})),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("Question");
    receive_until_idle(*controller);

    const auto entries = controller->transcript().entries();
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
        make_notice_entry(1, "Existing");
    {
        SessionJournal journal(temporary.path);
        journal.append(existing);
    }
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        temporary.path,
        notifier(),
        restore_with({existing}, 1, 2));

    const SessionUpdate cleared =
        controller->clear_transcript();
    EXPECT_EQ(cleared.notice, "Transcript cleared");
    EXPECT_TRUE(controller->transcript().entries().empty());
    EXPECT_TRUE(load_transcript_entries(temporary.path).empty());

    const SessionUpdate info = controller->session_information();
    ASSERT_TRUE(info.notice);
    EXPECT_NE(
        info.notice->find("Transcript entries: 0"),
        std::string::npos);
    EXPECT_NE(
        info.notice->find("* @Guide  test-model  test://completion  streaming"),
        std::string::npos);
    EXPECT_EQ(info.notice->find("Model:"), std::string::npos);
    EXPECT_TRUE(controller->transcript().entries().empty());
    EXPECT_TRUE(load_transcript_entries(temporary.path).empty());

}

TEST(SessionController, PersistenceFailureIdentifiesTheRequestAndAgent) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>()),
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
        (void)controller->submit_prompt("Question");
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
}

TEST(SessionController, RejectsNewOperationsDuringGeneration) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>(
            CompletionResult{},
            std::vector<std::string>{},
            true)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("Question");
    const SessionUpdate blocked =
        controller->submit_prompt("Another");
    EXPECT_FALSE(blocked.clear_input);
    EXPECT_EQ(
        blocked.notice,
        "Generation in progress; use /stop, Esc, or Ctrl-C");
    const SessionUpdate stopping =
        controller->request_stop();
    EXPECT_FALSE(stopping.clear_input);
    EXPECT_EQ(stopping.notice, "Stopping generation...");
    receive_until_idle(*controller);
}

TEST(SessionController, IgnoresEventsForAnotherRequest) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>(
            CompletionResult{},
            std::vector<std::string>{},
            true)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("Question");
    const SessionUpdate delta =
        controller->handle_agent_event(
            AgentDelta{
                999,
                CompletionDeltaKind::answer,
                "Wrong response",
            });
    const SessionUpdate completed =
        controller->handle_agent_event(
            AgentCompleted{999});

    EXPECT_FALSE(delta.render_needed);
    EXPECT_FALSE(completed.render_needed);
    EXPECT_TRUE(controller->generation_status().active);
    const auto entries = controller->transcript().entries();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().kind, EntryKind::human);

    (void)controller->request_stop();
    receive_until_idle(*controller);
}

TEST(SessionController, AttributesDispatchFailuresToTheTargetAgent) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        temporary.path,
        notifier());
    controller->shutdown();

    const SessionUpdate update =
        controller->submit_prompt("Question");

    EXPECT_EQ(update.notice, "Request could not be dispatched");
    const auto restored =
        load_transcript_entries(temporary.path);
    ASSERT_EQ(restored.size(), 2U);
    EXPECT_EQ(restored.back().kind, EntryKind::error);
    EXPECT_EQ(restored.back().participant_id, "guide-id");
    EXPECT_EQ(restored.back().text, "Agent execution is unavailable");
}

TEST(SessionController, FinalizesInterruptedTurnsDuringRestore) {
    TemporaryJournal temporary;
    {
        SessionJournal journal(temporary.path);
        const TranscriptEntry prompt =
            make_human_entry(1, "guide-id", "Guide", "Interrupted", 5);
        journal.start_turn(5, prompt);
    }
    SessionRestore restored =
        load_session_state(temporary.path);
    ASSERT_EQ(restored.interrupted_turns.size(), 1U);

    auto controller = SessionController::from_backends_for_testing(
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

TEST(SessionController, RoutesStructuredPromptsAndDefaultChangesAcrossForumPersonas) {
    TemporaryJournal temporary;
    auto guide = std::make_unique<ScriptedBackend>(
        CompletionResult{}, std::vector<std::string>{"Guide answer"});
    auto ismael = std::make_unique<ScriptedBackend>(
        CompletionResult{}, std::vector<std::string>{"Ismael answer"}, false,
        "ismael-id", "Ismael");
    ScriptedBackend* guide_view = guide.get();
    ScriptedBackend* ismael_view = ismael.get();
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::move(guide));
    backends.push_back(std::move(ismael));
    auto controller = SessionController::from_backends_for_testing(
        std::move(backends),
        temporary.path,
        notifier());

    const SessionUpdate mentioned =
        controller->submit_prompt("hello", "Ism");
    EXPECT_TRUE(mentioned.clear_input);
    receive_until_idle(*controller);
    ASSERT_EQ(ismael_view->requests.size(), 1U);
    EXPECT_EQ(ismael_view->requests.front().prompt.addressed_to, "ismael-id");
    EXPECT_TRUE(guide_view->requests.empty());

    const SessionUpdate default_changed =
        controller->set_default_agent("Gui");
    EXPECT_TRUE(default_changed.clear_input);
    EXPECT_EQ(default_changed.notice, "Default agent is now Guide");
    (void)controller->submit_prompt("next");
    receive_until_idle(*controller);
    ASSERT_EQ(guide_view->requests.size(), 1U);
    EXPECT_EQ(guide_view->requests.front().prompt.addressed_to, "guide-id");

    const std::size_t entries_before_rejection = controller->transcript().entries().size();
    const SessionUpdate rejected =
        controller->submit_prompt("text", "nobody");
    EXPECT_FALSE(rejected.clear_input);
    EXPECT_NE(rejected.notice->find("@nobody"), std::string::npos);
    EXPECT_EQ(controller->transcript().entries().size(), entries_before_rejection);

    const std::vector<TranscriptEntry> entries_before_agents =
        controller->transcript().entries();
    const SessionUpdate agents = controller->agent_information();
    EXPECT_TRUE(agents.clear_input);
    ASSERT_TRUE(agents.notice);
    EXPECT_NE(
        agents.notice->find("Any unambiguous prefix works."),
        std::string::npos);
    EXPECT_EQ(agents.notice->find("Cheburashka"), std::string::npos);
    EXPECT_NE(agents.notice->find("@Ismael"), std::string::npos);
    EXPECT_EQ(controller->transcript().entries(), entries_before_agents);
    EXPECT_EQ(
        load_transcript_entries(temporary.path),
        entries_before_agents);
}

// Foreign-history addressing is a transcript concern; covered in user_session/transcript tests.

TEST(SessionController, ShutdownCancelsAndPersistsAnActiveTurn) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>(
            CompletionResult{},
            std::vector<std::string>{"Partial"},
            true)),
        temporary.path,
        notifier());

    (void)controller->submit_prompt("Question");
    const SessionUpdate partial = receive_when_ready(*controller);
    EXPECT_TRUE(partial.render_needed);
    EXPECT_TRUE(controller->generation_status().active);
    controller->shutdown();

    EXPECT_FALSE(controller->generation_status().active);
    const auto entries =
        load_transcript_entries(temporary.path);
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(
        entries.back().status,
        EntryStatus::cancelled);
    EXPECT_EQ(entries.back().text, "Partial");
}

} // namespace
} // namespace cha
