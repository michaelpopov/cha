#include "agent_protocol.h"
#include "chat_coordinator.h"
#include "conversation.h"
#include "conversation_file.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>

namespace cha {
namespace {

// Removes one temporary journal when a coordinator test leaves scope.
class TemporaryJournal {
public:
    TemporaryJournal()
      : path(std::filesystem::temp_directory_path()
             / ("cha_coordinator_"
                + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                + ".data")) {
    }

    ~TemporaryJournal() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    std::filesystem::path path;
};

AgentInfo test_agent_info() {
    return {
        .id = "guide-id",
        .name = "Guide",
        .model = "test-model",
        .api = "http://example.test/v1/chat/completions",
        .streaming = true,
    };
}

TEST(ChatCoordinator, OwnsACompleteIdentifiedTypedTurn) {
    TemporaryJournal temporary;
    ConversationJournal journal(temporary.path);
    Conversation conversation;
    const ConversationEntry earlier = make_human_entry(10, "Earlier");
    journal.append(earlier);
    conversation.add_entry(earlier);
    std::atomic_bool cancellation{false};
    ChatCoordinator coordinator(test_agent_info(), journal, cancellation, conversation, 17, 11);
    CompletionRequestChannel requests;
    AgentEventChannel events;

    EXPECT_EQ(coordinator.submit("Current", requests), "");
    const std::optional<CompletionRequest> request = requests.get();
    ASSERT_TRUE(request);
    EXPECT_EQ(request->request_id, 17U);
    EXPECT_EQ(request->agent_id, "guide-id");
    EXPECT_EQ(request->history, (std::vector<ConversationEntry>{earlier}));
    EXPECT_EQ(request->prompt.kind, EntryKind::human);
    EXPECT_EQ(request->prompt.text, "Current");

    ASSERT_TRUE(events.push(AgentDelta{17, "Hello"}));
    ASSERT_TRUE(events.push(AgentDelta{17, " there"}));
    ASSERT_TRUE(events.push(AgentCompleted{17}));
    const CoordinatorUpdate update = coordinator.receive(events);

    EXPECT_TRUE(update.render_needed);
    EXPECT_FALSE(coordinator.generating());
    const auto completed_entries = conversation.entries();
    ASSERT_EQ(completed_entries.size(), 3U);
    const ConversationEntry& response = completed_entries.back();
    EXPECT_EQ(response.kind, EntryKind::agent);
    EXPECT_EQ(response.participant_id, "guide-id");
    EXPECT_EQ(response.display_name, "Guide");
    EXPECT_EQ(response.text, "Hello there");
    EXPECT_EQ(response.status, CompletionStatus::complete);
    EXPECT_EQ(load_conversation_file(temporary.path), conversation.entries());
}

TEST(ChatCoordinator, IgnoresEventsForAnotherRequest) {
    TemporaryJournal temporary;
    ConversationJournal journal(temporary.path);
    Conversation conversation;
    std::atomic_bool cancellation{false};
    ChatCoordinator coordinator(test_agent_info(), journal, cancellation, conversation);
    CompletionRequestChannel requests;
    AgentEventChannel events;

    ASSERT_TRUE(coordinator.submit("Question", requests).empty());
    ASSERT_TRUE(events.push(AgentDelta{999, "wrong"}));
    ASSERT_TRUE(events.push(AgentCompleted{999}));
    coordinator.receive(events);

    EXPECT_TRUE(coordinator.generating());
    ASSERT_EQ(conversation.entries().size(), 1U);
    EXPECT_EQ(conversation.entries().front().kind, EntryKind::human);
}

TEST(ChatCoordinator, PersistsAnIdentifiedCancelledResponse) {
    TemporaryJournal temporary;
    ConversationJournal journal(temporary.path);
    Conversation conversation;
    std::atomic_bool cancellation{false};
    ChatCoordinator coordinator(test_agent_info(), journal, cancellation, conversation);
    CompletionRequestChannel requests;
    AgentEventChannel events;

    ASSERT_TRUE(coordinator.submit("Question", requests).empty());
    coordinator.request_stop();
    ASSERT_TRUE(events.push(AgentDelta{1, "Partial"}));
    ASSERT_TRUE(events.push(AgentCancelled{1}));
    const CoordinatorUpdate update = coordinator.receive(events);

    ASSERT_TRUE(update.notice);
    EXPECT_EQ(*update.notice, "Generation stopped");
    const auto restored = load_conversation_file(temporary.path);
    ASSERT_EQ(restored.size(), 2U);
    EXPECT_EQ(restored.back().kind, EntryKind::agent);
    EXPECT_EQ(restored.back().status, CompletionStatus::cancelled);
    EXPECT_EQ(restored.back().text, "Partial");
}

TEST(ChatCoordinator, RecordsCancellationWithoutAnEmptyAssistantEntry) {
    TemporaryJournal temporary;
    ConversationJournal journal(temporary.path);
    Conversation conversation;
    std::atomic_bool cancellation{false};
    ChatCoordinator coordinator(test_agent_info(), journal, cancellation, conversation);
    CompletionRequestChannel requests;
    AgentEventChannel events;

    ASSERT_TRUE(coordinator.submit("Question", requests).empty());
    coordinator.request_stop();
    ASSERT_TRUE(events.push(AgentCancelled{1}));
    coordinator.receive(events);

    EXPECT_FALSE(coordinator.generating());
    ASSERT_EQ(conversation.entries().size(), 1U);
    EXPECT_EQ(conversation.entries().front().kind, EntryKind::human);
    EXPECT_EQ(load_conversation_file(temporary.path), conversation.entries());
}

TEST(ChatCoordinator, RejectsCompletionWithoutResponseContent) {
    TemporaryJournal temporary;
    ConversationJournal journal(temporary.path);
    Conversation conversation;
    std::atomic_bool cancellation{false};
    ChatCoordinator coordinator(test_agent_info(), journal, cancellation, conversation);
    CompletionRequestChannel requests;
    AgentEventChannel events;

    ASSERT_TRUE(coordinator.submit("Question", requests).empty());
    ASSERT_TRUE(events.push(AgentCompleted{1}));
    const CoordinatorUpdate update = coordinator.receive(events);

    EXPECT_FALSE(coordinator.generating());
    ASSERT_TRUE(update.notice);
    EXPECT_EQ(*update.notice, "Generation failed");
    const std::vector<ConversationEntry> entries = conversation.entries();
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries.back().kind, EntryKind::error);
    EXPECT_EQ(entries.back().text, "Agent completed without text content");
    EXPECT_EQ(load_conversation_file(temporary.path), entries);
}

TEST(ChatCoordinator, ReplacesPartialOutputWithATypedError) {
    TemporaryJournal temporary;
    ConversationJournal journal(temporary.path);
    Conversation conversation;
    std::atomic_bool cancellation{false};
    ChatCoordinator coordinator(test_agent_info(), journal, cancellation, conversation);
    CompletionRequestChannel requests;
    AgentEventChannel events;

    ASSERT_TRUE(coordinator.submit("Question", requests).empty());
    ASSERT_TRUE(events.push(AgentDelta{1, "Discard me"}));
    ASSERT_TRUE(events.push(AgentFailed{1, "network unavailable"}));
    coordinator.receive(events);

    const auto failed_entries = conversation.entries();
    ASSERT_EQ(failed_entries.size(), 2U);
    const ConversationEntry& error = failed_entries.back();
    EXPECT_EQ(error.kind, EntryKind::error);
    EXPECT_EQ(error.display_name, "Error");
    EXPECT_EQ(error.participant_id, "guide-id");
    EXPECT_EQ(error.text, "network unavailable");
    EXPECT_EQ(load_conversation_file(temporary.path), conversation.entries());
}

TEST(ChatCoordinator, RecordsDispatchFailureAsATerminalTurn) {
    TemporaryJournal temporary;
    ConversationJournal journal(temporary.path);
    Conversation conversation;
    std::atomic_bool cancellation{false};
    ChatCoordinator coordinator(test_agent_info(), journal, cancellation, conversation);
    CompletionRequestChannel requests;
    requests.close();

    EXPECT_EQ(coordinator.submit("Question", requests), "Request could not be dispatched");
    EXPECT_FALSE(coordinator.generating());
    const auto restored = load_conversation_file(temporary.path);
    ASSERT_EQ(restored.size(), 2U);
    EXPECT_EQ(restored.back().kind, EntryKind::error);
    EXPECT_TRUE(restored.back().participant_id.empty());
    EXPECT_EQ(restored.back().text, "Request channel is closed");
}

TEST(ChatCoordinator, DoesNotAttributePromptInsertionFailuresToTheAgent) {
    TemporaryJournal temporary;
    ConversationJournal journal(temporary.path);
    Conversation conversation;
    conversation.add_entry(make_notice_entry(2, "Existing"));
    std::atomic_bool cancellation{false};
    ChatCoordinator coordinator(test_agent_info(), journal, cancellation, conversation, 1, 1);
    CompletionRequestChannel requests;

    EXPECT_THROW((void)coordinator.submit("Question", requests), std::invalid_argument);
    const auto restored = load_conversation_file(temporary.path);
    ASSERT_EQ(restored.size(), 2U);
    EXPECT_EQ(restored.back().kind, EntryKind::error);
    EXPECT_TRUE(restored.back().participant_id.empty());
    EXPECT_EQ(restored.back().text, "Failed to add the submitted prompt to the conversation");
}

TEST(ChatCoordinator, PersistsClearAndSystemMessages) {
    TemporaryJournal temporary;
    ConversationJournal journal(temporary.path);
    Conversation conversation;
    const ConversationEntry existing = make_notice_entry(1, "Existing");
    journal.append(existing);
    conversation.add_entry(existing);
    std::atomic_bool cancellation{false};
    ChatCoordinator coordinator(test_agent_info(), journal, cancellation, conversation, 1, 2);

    coordinator.clear();
    EXPECT_TRUE(conversation.entries().empty());
    EXPECT_TRUE(load_conversation_file(temporary.path).empty());

    coordinator.add_system_message("Information");
    const std::vector<ConversationEntry> entries = conversation.entries();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().kind, EntryKind::notice);
    EXPECT_EQ(entries.front().text, "Information");
    EXPECT_EQ(load_conversation_file(temporary.path), entries);
}

TEST(ChatCoordinator, ReportsAClosedAgentEventChannel) {
    TemporaryJournal temporary;
    ConversationJournal journal(temporary.path);
    Conversation conversation;
    std::atomic_bool cancellation{false};
    ChatCoordinator coordinator(test_agent_info(), journal, cancellation, conversation);
    AgentEventChannel events;
    events.close();

    const CoordinatorUpdate update = coordinator.receive(events);

    EXPECT_TRUE(update.channel_closed);
}

TEST(ChatCoordinator, ShutdownCancelsAnActiveTurnAndClosesRequests) {
    TemporaryJournal temporary;
    ConversationJournal journal(temporary.path);
    Conversation conversation;
    std::atomic_bool cancellation{false};
    ChatCoordinator coordinator(test_agent_info(), journal, cancellation, conversation);
    CompletionRequestChannel requests;
    ASSERT_TRUE(coordinator.submit("Question", requests).empty());

    coordinator.shutdown(requests);

    EXPECT_TRUE(cancellation.load(std::memory_order_acquire));
    CompletionRequest request;
    EXPECT_EQ(requests.try_get(request), ChannelReadStatus::value);
    EXPECT_EQ(requests.try_get(request), ChannelReadStatus::closed);
    EXPECT_FALSE(requests.push({}));
}

} // namespace
} // namespace cha
