#include "chat_coordinator.h"
#include "agent_context.h"
#include "completion_backend.h"
#include "session_database.h"

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

struct ContextMessage {
    AgentRole role{};
    std::string content;

    bool operator==(const ContextMessage&) const = default;
};

class ContextRecorder final : public AgentContextWriter {
public:
    void begin_message(AgentRole role) override {
        current_ = {.role = role};
    }
    void append_content(std::string_view text) override { current_.content += text; }
    void end_message() override { messages.push_back(std::move(current_)); }

    std::vector<ContextMessage> messages;

private:
    ContextMessage current_;
};

// Removes one temporary session database when a coordinator test leaves scope.
class TemporaryJournal {
public:
    TemporaryJournal()
      : path(std::filesystem::temp_directory_path()
             / ("cha_coordinator_"
                + std::to_string(
                    std::chrono::steady_clock::now()
                        .time_since_epoch()
                        .count())
                + ".sqlite3")) {
        if (!create_session_database(
                path,
                {
                    .id = "coordinator-test",
                    .room = "test-room",
                    .persona = "test-persona",
                    .label = "Coordinator test",
                })) {
            throw std::runtime_error("Failed to create coordinator test database");
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
        bool wait_for_cancellation = false)
      : result_(std::move(result)),
        deltas_(std::move(deltas)),
        wait_for_cancellation_(wait_for_cancellation) {
    }

    RequestPayload prepare(
        const CompletionRequest& request,
        const ConversationReadView& conversation) override {
        requests.push_back(request);
        latest_prompt = conversation.entries().back();
        ContextRecorder writer;
        write_agent_context(
            conversation.entries(),
            conversation.open_entry_id(),
            {},
            id_,
            writer);
        model_contexts.push_back(std::move(writer.messages));
        return {.bytes = request.prompt.text};
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
            return {CompletionOutcome::cancelled, {}};
        }
        return result_;
    }

    AgentInfo info() const override {
        return {
            .id = id_,
            .name = "Guide",
            .model = "test-model",
            .api = "test://completion",
            .streaming = true,
        };
    }

    const std::string& agent_id() const override {
        return id_;
    }

    std::vector<CompletionRequest> requests;
    std::vector<std::vector<ContextMessage>> model_contexts;
    ConversationEntry latest_prompt;

private:
    std::string id_{"guide-id"};
    CompletionResult result_;
    std::vector<std::string> deltas_;
    bool wait_for_cancellation_{};
};

CoordinatorUpdate receive_until_idle(ChatCoordinator& coordinator) {
    CoordinatorUpdate combined;
    while (coordinator.generating()) {
        pollfd descriptor{
            coordinator.notification_fd(),
            POLLIN,
            0,
        };
        if (::poll(&descriptor, 1, 1000) != 1) {
            throw std::runtime_error(
                "Timed out waiting for coordinator event");
        }
        const CoordinatorUpdate update = coordinator.receive();
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

TEST(ChatCoordinator, OwnsACompleteIdentifiedTypedTurn) {
    TemporaryJournal temporary;
    const ConversationEntry earlier =
        make_human_entry(10, "Earlier");
    {
        ConversationJournal journal(temporary.path);
        journal.append(earlier);
    }
    auto backend = std::make_unique<ScriptedBackend>(
        CompletionResult{},
        std::vector<std::string>{"Hello", " there"});
    ScriptedBackend* backend_view = backend.get();
    ChatCoordinator coordinator(
        std::move(backend),
        temporary.path,
        restore_with({earlier}, 17, 11));

    const CoordinatorUpdate submitted =
        coordinator.handle_input("Current");
    EXPECT_TRUE(submitted.render_needed);
    const CoordinatorUpdate completed =
        receive_until_idle(coordinator);

    ASSERT_EQ(backend_view->requests.size(), 1U);
    const CompletionRequest& request =
        backend_view->requests.front();
    EXPECT_EQ(request.request_id, 17U);
    EXPECT_EQ(request.agent_id, "guide-id");
    EXPECT_EQ(request.conversation_revision, 2U);
    EXPECT_EQ(request.prompt.kind, EntryKind::human);
    EXPECT_EQ(request.prompt.text, "Current");
    EXPECT_EQ(backend_view->latest_prompt, request.prompt);
    EXPECT_EQ(
        backend_view->model_contexts.front(),
        (std::vector<ContextMessage>{
            {AgentRole::user, "Earlier"},
            {AgentRole::user, "Current"},
        }));
    EXPECT_TRUE(completed.render_needed);

    const auto entries = coordinator.conversation().entries();
    ASSERT_EQ(entries.size(), 3U);
    const ConversationEntry& response = entries.back();
    EXPECT_EQ(response.kind, EntryKind::agent);
    EXPECT_EQ(response.participant_id, "guide-id");
    EXPECT_EQ(response.display_name, "Guide");
    EXPECT_EQ(response.text, "Hello there");
    EXPECT_EQ(response.status, CompletionStatus::complete);
    EXPECT_EQ(load_conversation_entries(temporary.path), entries);
}

TEST(ChatCoordinator, PreparesTheSecondTurnFromTheSharedCompletedConversation) {
    TemporaryJournal temporary;
    auto backend = std::make_unique<ScriptedBackend>(
        CompletionResult{}, std::vector<std::string>{"Answer"});
    ScriptedBackend* backend_view = backend.get();
    ChatCoordinator coordinator(std::move(backend), temporary.path);

    (void)coordinator.handle_input("First");
    receive_until_idle(coordinator);
    (void)coordinator.handle_input("Second");
    receive_until_idle(coordinator);

    ASSERT_EQ(backend_view->model_contexts.size(), 2U);
    EXPECT_EQ(
        backend_view->model_contexts[1],
        (std::vector<ContextMessage>{
            {AgentRole::user, "First"},
            {AgentRole::assistant, "Answer"},
            {AgentRole::user, "Second"},
        }));
}

TEST(ChatCoordinator, ClearMakesTheNextRequestSeeOnlyPostClearContext) {
    TemporaryJournal temporary;
    auto backend = std::make_unique<ScriptedBackend>(
        CompletionResult{}, std::vector<std::string>{"Answer"});
    ScriptedBackend* backend_view = backend.get();
    ChatCoordinator coordinator(std::move(backend), temporary.path);

    (void)coordinator.handle_input("First");
    receive_until_idle(coordinator);
    (void)coordinator.handle_input("/clear");
    (void)coordinator.handle_input("Second");
    receive_until_idle(coordinator);

    ASSERT_EQ(backend_view->model_contexts.size(), 2U);
    EXPECT_EQ(
        backend_view->model_contexts[1],
        (std::vector<ContextMessage>{{AgentRole::user, "Second"}}));
}

TEST(ChatCoordinator, ExcludesFailedTurnsFromTheFollowingModelContext) {
    TemporaryJournal temporary;
    auto backend = std::make_unique<ScriptedBackend>(
        CompletionResult{CompletionOutcome::transport_error, "unavailable"});
    ScriptedBackend* backend_view = backend.get();
    ChatCoordinator coordinator(std::move(backend), temporary.path);

    (void)coordinator.handle_input("Failed");
    receive_until_idle(coordinator);
    (void)coordinator.handle_input("Second");
    receive_until_idle(coordinator);

    ASSERT_EQ(backend_view->model_contexts.size(), 2U);
    EXPECT_EQ(
        backend_view->model_contexts[1],
        (std::vector<ContextMessage>{{AgentRole::user, "Second"}}));
}

TEST(ChatCoordinator, ExcludesCancelledPartialOutputFromFollowingModelContext) {
    TemporaryJournal temporary;
    auto backend = std::make_unique<ScriptedBackend>(
        CompletionResult{CompletionOutcome::cancelled, {}},
        std::vector<std::string>{"Partial"});
    ScriptedBackend* backend_view = backend.get();
    ChatCoordinator coordinator(std::move(backend), temporary.path);

    (void)coordinator.handle_input("First");
    receive_until_idle(coordinator);
    (void)coordinator.handle_input("Second");
    receive_until_idle(coordinator);

    ASSERT_EQ(backend_view->model_contexts.size(), 2U);
    EXPECT_EQ(
        backend_view->model_contexts[1],
        (std::vector<ContextMessage>{
            {AgentRole::user, "First"},
            {AgentRole::user, "Second"},
        }));
}

TEST(ChatCoordinator, PersistsAnIdentifiedCancelledResponse) {
    TemporaryJournal temporary;
    ChatCoordinator coordinator(
        std::make_unique<ScriptedBackend>(
            CompletionResult{CompletionOutcome::cancelled, {}},
            std::vector<std::string>{"Partial"}),
        temporary.path);

    (void)coordinator.handle_input("Question");
    const CoordinatorUpdate update =
        receive_until_idle(coordinator);

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

TEST(ChatCoordinator, RecordsCancellationWithoutAnEmptyAssistantEntry) {
    TemporaryJournal temporary;
    ChatCoordinator coordinator(
        std::make_unique<ScriptedBackend>(
            CompletionResult{CompletionOutcome::cancelled, {}}),
        temporary.path);

    (void)coordinator.handle_input("Question");
    receive_until_idle(coordinator);

    const auto entries = coordinator.conversation().entries();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().kind, EntryKind::human);
    EXPECT_EQ(load_conversation_entries(temporary.path), entries);
}

TEST(ChatCoordinator, RejectsCompletionWithoutResponseContent) {
    TemporaryJournal temporary;
    ChatCoordinator coordinator(
        std::make_unique<ScriptedBackend>(),
        temporary.path);

    (void)coordinator.handle_input("Question");
    const CoordinatorUpdate update =
        receive_until_idle(coordinator);

    ASSERT_TRUE(update.notice);
    EXPECT_EQ(*update.notice, "Generation failed");
    const auto entries = coordinator.conversation().entries();
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries.back().kind, EntryKind::error);
    EXPECT_EQ(
        entries.back().text,
        "Agent completed without text content");
    EXPECT_EQ(load_conversation_entries(temporary.path), entries);
}

TEST(ChatCoordinator, ReplacesPartialOutputWithATypedError) {
    TemporaryJournal temporary;
    ChatCoordinator coordinator(
        std::make_unique<ScriptedBackend>(
            CompletionResult{
                CompletionOutcome::transport_error,
                "network unavailable",
            },
            std::vector<std::string>{"Discard me"}),
        temporary.path);

    (void)coordinator.handle_input("Question");
    receive_until_idle(coordinator);

    const auto entries = coordinator.conversation().entries();
    ASSERT_EQ(entries.size(), 2U);
    const ConversationEntry& error = entries.back();
    EXPECT_EQ(error.kind, EntryKind::error);
    EXPECT_EQ(error.display_name, "Error");
    EXPECT_EQ(error.participant_id, "guide-id");
    EXPECT_EQ(error.text, "network unavailable");
    EXPECT_EQ(load_conversation_entries(temporary.path), entries);
}

TEST(ChatCoordinator, OwnsClearInfoAndExitCommandSemantics) {
    TemporaryJournal temporary;
    const ConversationEntry existing =
        make_notice_entry(1, "Existing");
    {
        ConversationJournal journal(temporary.path);
        journal.append(existing);
    }
    ChatCoordinator coordinator(
        std::make_unique<ScriptedBackend>(),
        temporary.path,
        restore_with({existing}, 1, 2));

    const CoordinatorUpdate cleared =
        coordinator.handle_input("/clear");
    EXPECT_EQ(cleared.notice, "Conversation cleared");
    EXPECT_TRUE(coordinator.conversation().entries().empty());
    EXPECT_TRUE(load_conversation_entries(temporary.path).empty());

    (void)coordinator.handle_input("/info");
    const auto entries = coordinator.conversation().entries();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().kind, EntryKind::notice);
    EXPECT_NE(
        entries.front().text.find("Model: test-model"),
        std::string::npos);
    EXPECT_NE(
        entries.front().text.find("Transcript entries: 0"),
        std::string::npos);
    EXPECT_EQ(load_conversation_entries(temporary.path), entries);

    EXPECT_TRUE(
        coordinator.handle_input("/exit").end_session);
}

TEST(ChatCoordinator, RejectsCommandsAndNewPromptsDuringGeneration) {
    TemporaryJournal temporary;
    ChatCoordinator coordinator(
        std::make_unique<ScriptedBackend>(
            CompletionResult{},
            std::vector<std::string>{},
            true),
        temporary.path);

    (void)coordinator.handle_input("Question");
    const CoordinatorUpdate blocked =
        coordinator.handle_input("Another");
    EXPECT_FALSE(blocked.clear_input);
    EXPECT_EQ(
        blocked.notice,
        "Generation in progress; use /stop, Esc, or Ctrl-C");
    const CoordinatorUpdate stopping =
        coordinator.handle_input("/stop");
    EXPECT_TRUE(stopping.clear_input);
    EXPECT_EQ(stopping.notice, "Stopping generation...");
    receive_until_idle(coordinator);
}

TEST(ChatCoordinator, IgnoresEventsForAnotherRequest) {
    TemporaryJournal temporary;
    ChatCoordinator coordinator(
        std::make_unique<ScriptedBackend>(
            CompletionResult{},
            std::vector<std::string>{},
            true),
        temporary.path);

    (void)coordinator.handle_input("Question");
    const CoordinatorUpdate delta =
        coordinator.handle_agent_event(
            AgentDelta{999, "Wrong response"});
    const CoordinatorUpdate completed =
        coordinator.handle_agent_event(
            AgentCompleted{999});

    EXPECT_FALSE(delta.render_needed);
    EXPECT_FALSE(completed.render_needed);
    EXPECT_TRUE(coordinator.generating());
    const auto entries = coordinator.conversation().entries();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().kind, EntryKind::human);

    (void)coordinator.request_stop();
    receive_until_idle(coordinator);
}

TEST(ChatCoordinator, DoesNotAttributeLocalDispatchFailuresToTheAgent) {
    TemporaryJournal temporary;
    ChatCoordinator coordinator(
        std::make_unique<ScriptedBackend>(),
        temporary.path);
    coordinator.shutdown();

    const CoordinatorUpdate update =
        coordinator.handle_input("Question");

    EXPECT_EQ(update.notice, "Request could not be dispatched");
    const auto restored =
        load_conversation_entries(temporary.path);
    ASSERT_EQ(restored.size(), 2U);
    EXPECT_EQ(restored.back().kind, EntryKind::error);
    EXPECT_TRUE(restored.back().participant_id.empty());
    EXPECT_EQ(restored.back().text, "Agent worker is closed");
}

TEST(ChatCoordinator, FinalizesInterruptedTurnsDuringRestore) {
    TemporaryJournal temporary;
    {
        ConversationJournal journal(temporary.path);
        const ConversationEntry prompt =
            make_human_entry(1, "Interrupted", 5);
        journal.start_turn(5, "guide-id", prompt);
    }
    ConversationRestore restored =
        load_conversation_state(temporary.path);
    ASSERT_EQ(restored.interrupted_turns.size(), 1U);

    ChatCoordinator coordinator(
        std::make_unique<ScriptedBackend>(),
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

TEST(ChatCoordinator, ShutdownCancelsAndPersistsAnActiveTurn) {
    TemporaryJournal temporary;
    ChatCoordinator coordinator(
        std::make_unique<ScriptedBackend>(
            CompletionResult{},
            std::vector<std::string>{"Partial"},
            true),
        temporary.path);

    (void)coordinator.handle_input("Question");
    pollfd descriptor{
        coordinator.notification_fd(),
        POLLIN,
        0,
    };
    ASSERT_EQ(::poll(&descriptor, 1, 1000), 1);
    const CoordinatorUpdate partial = coordinator.receive();
    EXPECT_TRUE(partial.render_needed);
    EXPECT_TRUE(coordinator.generating());
    coordinator.shutdown();

    EXPECT_FALSE(coordinator.generating());
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
