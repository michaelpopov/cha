#include "agent_protocol.h"
#include "chat_coordinator.h"
#include "conversation.h"
#include "conversation_file.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <variant>

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
        .id = "guide",
        .name = "Guide",
        .model = "test-model",
        .api = "http://example.test/v1/chat/completions",
        .streaming = true,
    };
}

TEST(ChatCoordinator, OwnsACompleteIdentifiedTurn) {
    TemporaryJournal temporary;
    ConversationJournal journal(temporary.path);
    Conversation conversation;
    journal.append({"You", "Earlier"});
    conversation.add_message("You", "Earlier");
    std::atomic_bool cancellation{false};
    ChatCoordinator coordinator(test_agent_info(), journal, cancellation, conversation, 17);
    CompletionRequestChannel requests;
    AgentEventChannel events;

    EXPECT_EQ(coordinator.submit("Current", requests), "");
    const std::optional<CompletionRequest> request = requests.get();
    ASSERT_TRUE(request);
    EXPECT_EQ(request->request_id, 17U);
    EXPECT_EQ(request->agent_id, "guide");
    EXPECT_EQ(request->history, (std::vector<ConversationMessage>{{"You", "Earlier"}}));
    EXPECT_EQ(request->prompt, "Current");

    ASSERT_TRUE(events.push(AgentDelta{17, "Hello"}));
    ASSERT_TRUE(events.push(AgentDelta{17, " there"}));
    ASSERT_TRUE(events.push(AgentCompleted{17}));
    const CoordinatorUpdate update = coordinator.receive(events);

    EXPECT_TRUE(update.render_needed);
    EXPECT_FALSE(coordinator.generating());
    EXPECT_EQ(
        conversation.messages(),
        (std::vector<ConversationMessage>{
            {"You", "Earlier"},
            {"You", "Current"},
            {"Guide", "Hello there"},
        }));
    EXPECT_EQ(load_conversation_file(temporary.path), conversation.messages());
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
    EXPECT_EQ(conversation.messages(), (std::vector<ConversationMessage>{{"You", "Question"}}));
}

TEST(ChatCoordinator, PersistsTheIdentifiedPartialResponseOnCancellation) {
    TemporaryJournal temporary;
    ConversationJournal journal(temporary.path);
    Conversation conversation;
    std::atomic_bool cancellation{false};
    ChatCoordinator coordinator(test_agent_info(), journal, cancellation, conversation);
    CompletionRequestChannel requests;
    AgentEventChannel events;

    ASSERT_TRUE(coordinator.submit("Question", requests).empty());
    coordinator.request_stop();
    EXPECT_TRUE(cancellation.load(std::memory_order_acquire));
    ASSERT_TRUE(events.push(AgentDelta{1, "Partial"}));
    ASSERT_TRUE(events.push(AgentCancelled{1}));
    const CoordinatorUpdate update = coordinator.receive(events);

    ASSERT_TRUE(update.notice);
    EXPECT_EQ(*update.notice, "Generation stopped");
    EXPECT_FALSE(coordinator.generating());
    EXPECT_EQ(
        load_conversation_file(temporary.path),
        (std::vector<ConversationMessage>{{"You", "Question"}, {"Guide", "Partial"}}));
}

TEST(ChatCoordinator, ReplacesPartialOutputWithAnIdentifiedFailure) {
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

    EXPECT_EQ(
        conversation.messages(),
        (std::vector<ConversationMessage>{
            {"You", "Question"},
            {"System", "Error: network unavailable"},
        }));
    EXPECT_EQ(load_conversation_file(temporary.path), conversation.messages());
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
    EXPECT_EQ(
        load_conversation_file(temporary.path),
        (std::vector<ConversationMessage>{
            {"You", "Question"},
            {"System", "Error: Request channel is closed"},
        }));
}

} // namespace
} // namespace cha
