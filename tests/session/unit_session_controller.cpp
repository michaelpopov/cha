#include "session/session_controller.h"
#include "agents/agent.h"
#include "agents/completion_backend.h"
#include "session/session_database.h"
#include "support/test_backends.h"

#include <gtest/gtest.h>

#include <poll.h>

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
                    .room = "test-room",
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
        const ConversationReadView& conversation) override {
        requests.push_back(request);
        latest_prompt = conversation.entries().back();
        model_contexts.push_back(project_agent_context(
            conversation.entries(),
            conversation.open_entry_id(),
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

    AgentInfo info() const override {
        return {
            .id = id_,
            .name = name_,
            .model = "test-model",
            .api = "test://completion",
            .streaming = true,
        };
    }

    const std::string& agent_id() const override {
        return id_;
    }

    std::vector<CompletionRequest> requests;
    std::vector<std::vector<AgentMessage>> model_contexts;
    ConversationEntry latest_prompt;

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
        pollfd descriptor{
            controller.notification_fd(),
            POLLIN,
            0,
        };
        if (::poll(&descriptor, 1, 1000) != 1) {
            throw std::runtime_error(
                "Timed out waiting for controller event");
        }
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
    }
    return combined;
}

ConversationRestore restore_with(
    std::vector<ConversationEntry> entries,
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
    const ConversationEntry earlier =
        make_human_entry(10, "guide-id", "Guide", "Earlier");
    {
        ConversationJournal journal(temporary.path);
        journal.append(earlier);
    }
    auto backend = std::make_unique<ScriptedBackend>(
        CompletionResult{},
        std::vector<std::string>{"Hello", " there"});
    ScriptedBackend* backend_view = backend.get();
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::move(backend)),
        temporary.path,
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

    const auto entries = controller->conversation().entries();
    ASSERT_EQ(entries.size(), 3U);
    const ConversationEntry& response = entries.back();
    EXPECT_EQ(response.kind, EntryKind::agent);
    EXPECT_EQ(response.participant_id, "guide-id");
    EXPECT_EQ(response.display_name, "Guide");
    EXPECT_EQ(response.text, "Hello there");
    EXPECT_EQ(response.status, CompletionStatus::complete);
    EXPECT_EQ(load_conversation_entries(temporary.path), entries);
}

TEST(SessionController, PreparesTheSecondTurnFromTheSharedCompletedConversation) {
    TemporaryJournal temporary;
    auto backend = std::make_unique<ScriptedBackend>(
        CompletionResult{}, std::vector<std::string>{"Answer"});
    ScriptedBackend* backend_view = backend.get();
    auto controller = SessionController::from_backends_for_testing(test::one_backend(std::move(backend)), temporary.path);

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
    auto controller = SessionController::from_backends_for_testing(test::one_backend(std::move(backend)), temporary.path);

    (void)controller->submit_prompt("First");
    receive_until_idle(*controller);
    (void)controller->clear_conversation();
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
    auto controller = SessionController::from_backends_for_testing(test::one_backend(std::move(backend)), temporary.path);

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
    auto controller = SessionController::from_backends_for_testing(test::one_backend(std::move(backend)), temporary.path);

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
        temporary.path);

    (void)controller->submit_prompt("Question");
    const SessionUpdate update =
        receive_until_idle(*controller);

    ASSERT_TRUE(update.notice);
    EXPECT_EQ(*update.notice, "Generation stopped");
    const auto restored =
        load_conversation_entries(temporary.path);
    ASSERT_EQ(restored.size(), 2U);
    EXPECT_EQ(restored.back().kind, EntryKind::agent);
    EXPECT_EQ(
        restored.back().status,
        CompletionStatus::cancelled);
    EXPECT_EQ(restored.back().text, "Partial");
}

TEST(SessionController, RecordsCancellationWithoutAnEmptyAssistantEntry) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>(
            CompletionResult{CompletionOutcome::cancelled, {}})),
        temporary.path);

    (void)controller->submit_prompt("Question");
    receive_until_idle(*controller);

    const auto entries = controller->conversation().entries();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().kind, EntryKind::human);
    EXPECT_EQ(load_conversation_entries(temporary.path), entries);
}

TEST(SessionController, TracksReasoningAnswerAndLateReasoningMonotonically) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>(
            CompletionResult{},
            std::vector<std::string>{},
            true)),
        temporary.path);

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
    ASSERT_EQ(controller->conversation().entries().size(), 2U);
    EXPECT_EQ(
        controller->conversation().entries().back().reasoning_text,
        "PRIVATE_REASONING");

    (void)controller->handle_agent_event(AgentDelta{
        1,
        CompletionDeltaKind::answer,
        "Answer",
    });
    EXPECT_EQ(
        controller->generation_status().phase,
        ResponsePhase::answering);
    (void)controller->handle_agent_event(AgentDelta{
        1,
        CompletionDeltaKind::reasoning,
        " late",
    });
    EXPECT_EQ(
        controller->generation_status().phase,
        ResponsePhase::answering);

    (void)controller->handle_agent_event(AgentCompleted{1});
    EXPECT_FALSE(controller->generation_status().active);
    const std::vector<ConversationEntry> live =
        controller->conversation().entries();
    ASSERT_EQ(live.size(), 2U);
    EXPECT_EQ(live.back().reasoning_text, "PRIVATE_REASONING late");
    EXPECT_EQ(live.back().text, "Answer");
    EXPECT_EQ(live.back().status, CompletionStatus::complete);

    const std::vector<ConversationEntry> restored =
        load_conversation_entries(temporary.path);
    ASSERT_EQ(restored.size(), 2U);
    EXPECT_TRUE(restored.back().reasoning_text.empty());
    EXPECT_EQ(restored.back().text, "Answer");
}

TEST(SessionController, ReasoningOnlyCancellationIsLiveButNotDurable) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>(
            CompletionResult{},
            std::vector<std::string>{},
            true)),
        temporary.path);

    (void)controller->submit_prompt("Question");
    (void)controller->handle_agent_event(AgentDelta{
        1,
        CompletionDeltaKind::reasoning,
        "EPHEMERAL_REASONING_ONLY",
    });
    (void)controller->request_stop();
    receive_until_idle(*controller);

    const std::vector<ConversationEntry> live =
        controller->conversation().entries();
    ASSERT_EQ(live.size(), 2U);
    EXPECT_EQ(live.back().status, CompletionStatus::cancelled);
    EXPECT_EQ(live.back().reasoning_text, "EPHEMERAL_REASONING_ONLY");
    EXPECT_TRUE(live.back().text.empty());

    const std::vector<ConversationEntry> restored =
        load_conversation_entries(temporary.path);
    ASSERT_EQ(restored.size(), 1U);
    EXPECT_EQ(restored.front().kind, EntryKind::human);
}

TEST(SessionController, RejectsCompletionWithoutResponseContent) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        temporary.path);

    (void)controller->submit_prompt("Question");
    const SessionUpdate update =
        receive_until_idle(*controller);

    ASSERT_TRUE(update.notice);
    EXPECT_EQ(*update.notice, "Generation failed");
    const auto entries = controller->conversation().entries();
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries.back().kind, EntryKind::error);
    EXPECT_EQ(
        entries.back().text,
        "Agent completed without answer content");
    EXPECT_EQ(load_conversation_entries(temporary.path), entries);
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
        temporary.path);

    (void)controller->submit_prompt("Question");
    receive_until_idle(*controller);

    const auto entries = controller->conversation().entries();
    ASSERT_EQ(entries.size(), 2U);
    const ConversationEntry& error = entries.back();
    EXPECT_EQ(error.kind, EntryKind::error);
    EXPECT_EQ(error.display_name, "Error");
    EXPECT_EQ(error.participant_id, "guide-id");
    EXPECT_EQ(error.text, "network unavailable");
    EXPECT_EQ(load_conversation_entries(temporary.path), entries);
}

TEST(SessionController, OwnsClearAndInformationSemantics) {
    TemporaryJournal temporary;
    const ConversationEntry existing =
        make_notice_entry(1, "Existing");
    {
        ConversationJournal journal(temporary.path);
        journal.append(existing);
    }
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        temporary.path,
        restore_with({existing}, 1, 2));

    const SessionUpdate cleared =
        controller->clear_conversation();
    EXPECT_EQ(cleared.notice, "Conversation cleared");
    EXPECT_TRUE(controller->conversation().entries().empty());
    EXPECT_TRUE(load_conversation_entries(temporary.path).empty());

    const SessionUpdate info = controller->session_information();
    ASSERT_TRUE(info.notice);
    EXPECT_NE(
        info.notice->find("Transcript entries: 0"),
        std::string::npos);
    EXPECT_NE(
        info.notice->find("* @Guide  test-model  test://completion  streaming"),
        std::string::npos);
    EXPECT_EQ(info.notice->find("Model:"), std::string::npos);
    EXPECT_TRUE(controller->conversation().entries().empty());
    EXPECT_TRUE(load_conversation_entries(temporary.path).empty());

}

TEST(SessionController, PersistenceFailureIdentifiesTheRequestAndAgent) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        temporary.path);
    const std::filesystem::path moved = temporary.path.string() + ".moved";
    std::filesystem::rename(temporary.path, moved);

    std::string message;
    try {
        (void)controller->submit_prompt("Question");
    } catch (const std::runtime_error& error) {
        message = error.what();
    }

    std::filesystem::rename(moved, temporary.path);
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
        temporary.path);

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
        temporary.path);

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
    const auto entries = controller->conversation().entries();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().kind, EntryKind::human);

    (void)controller->request_stop();
    receive_until_idle(*controller);
}

TEST(SessionController, AttributesDispatchFailuresToTheTargetAgent) {
    TemporaryJournal temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        temporary.path);
    controller->shutdown();

    const SessionUpdate update =
        controller->submit_prompt("Question");

    EXPECT_EQ(update.notice, "Request could not be dispatched");
    const auto restored =
        load_conversation_entries(temporary.path);
    ASSERT_EQ(restored.size(), 2U);
    EXPECT_EQ(restored.back().kind, EntryKind::error);
    EXPECT_EQ(restored.back().participant_id, "guide-id");
    EXPECT_EQ(restored.back().text, "Agent execution is unavailable");
}

TEST(SessionController, FinalizesInterruptedTurnsDuringRestore) {
    TemporaryJournal temporary;
    {
        ConversationJournal journal(temporary.path);
        const ConversationEntry prompt =
            make_human_entry(1, "guide-id", "Guide", "Interrupted", 5);
        journal.start_turn(5, prompt);
    }
    ConversationRestore restored =
        load_conversation_state(temporary.path);
    ASSERT_EQ(restored.interrupted_turns.size(), 1U);

    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<ScriptedBackend>()),
        temporary.path,
        std::move(restored));

    const ConversationRestore repaired =
        load_conversation_state(temporary.path);
    EXPECT_TRUE(repaired.interrupted_turns.empty());
    ASSERT_EQ(repaired.entries.size(), 2U);
    EXPECT_EQ(repaired.entries.back().kind, EntryKind::error);
    EXPECT_NE(
        repaired.entries.back().text.find("interrupted"),
        std::string::npos);
}

TEST(SessionController, RoutesStructuredPromptsAndDefaultChangesAcrossTheRoster) {
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
    auto controller = SessionController::from_backends_for_testing(std::move(backends), temporary.path);

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

    const std::size_t entries_before_rejection = controller->conversation().entries().size();
    const SessionUpdate rejected =
        controller->submit_prompt("text", "nobody");
    EXPECT_FALSE(rejected.clear_input);
    EXPECT_NE(rejected.notice->find("@nobody"), std::string::npos);
    EXPECT_EQ(controller->conversation().entries().size(), entries_before_rejection);

    const std::vector<ConversationEntry> entries_before_agents =
        controller->conversation().entries();
    const SessionUpdate agents = controller->agent_information();
    EXPECT_TRUE(agents.clear_input);
    ASSERT_TRUE(agents.notice);
    EXPECT_NE(
        agents.notice->find("Any unambiguous prefix works."),
        std::string::npos);
    EXPECT_EQ(agents.notice->find("Cheburashka"), std::string::npos);
    EXPECT_NE(agents.notice->find("@Ismael"), std::string::npos);
    EXPECT_EQ(controller->conversation().entries(), entries_before_agents);
    EXPECT_EQ(
        load_conversation_entries(temporary.path),
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
        temporary.path);

    (void)controller->submit_prompt("Question");
    pollfd descriptor{
        controller->notification_fd(),
        POLLIN,
        0,
    };
    ASSERT_EQ(::poll(&descriptor, 1, 1000), 1);
    const SessionUpdate partial = controller->receive();
    EXPECT_TRUE(partial.render_needed);
    EXPECT_TRUE(controller->generation_status().active);
    controller->shutdown();

    EXPECT_FALSE(controller->generation_status().active);
    const auto entries =
        load_conversation_entries(temporary.path);
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(
        entries.back().status,
        CompletionStatus::cancelled);
    EXPECT_EQ(entries.back().text, "Partial");
}

} // namespace
} // namespace cha
