#include "agent_protocol.h"
#include "chat_coordinator.h"
#include "conversation.h"
#include "conversation_file.h"
#include "input_editor.h"
#include "session_view.h"
#include "user_session.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cha {
namespace {

// Removes one temporary session journal when a controller test leaves scope.
class TemporarySessionJournal {
public:
    TemporarySessionJournal()
      : path(std::filesystem::temp_directory_path()
             / ("cha_user_session_"
                + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
                + ".data")) {
    }

    ~TemporarySessionJournal() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    std::filesystem::path path;
};

// Supplies deterministic typed input and records renders without initializing curses.
class FakeSessionView final : public SessionView {
public:
    [[nodiscard]] std::optional<SessionInput> read_input() override {
        if (inputs.empty()) {
            return std::nullopt;
        }
        SessionInput input = inputs.front();
        inputs.pop_front();
        return input;
    }

    void render(
        const Conversation& conversation,
        const InputEditor& editor,
        bool generating,
        std::string_view notice) override {
        ++render_count;
        rendered_entries = conversation.entries();
        rendered_input = editor.value();
        rendered_generating = generating;
        rendered_notice = notice;
    }

    void scroll_up() override {
        ++scroll_up_count;
    }

    void scroll_down() override {
        ++scroll_down_count;
    }

    void resize() override {
        ++resize_count;
    }

    [[nodiscard]] bool input_closed() const override {
        return input_is_closed;
    }

    void type(std::string_view text) {
        for (const char character : text) {
            inputs.push_back({
                .kind = SessionInputKind::character,
                .character = static_cast<wchar_t>(character),
            });
        }
    }

    void push(SessionInputKind kind) {
        inputs.push_back({.kind = kind});
    }

    std::deque<SessionInput> inputs;
    std::vector<ConversationEntry> rendered_entries;
    std::string rendered_input;
    std::string rendered_notice;
    bool rendered_generating{};
    int render_count{};
    int scroll_up_count{};
    int scroll_down_count{};
    int resize_count{};
    bool input_is_closed{};
};

AgentInfo session_agent_info() {
    return {
        .id = "guide-id",
        .name = "Guide",
        .model = "test-model",
        .api = "http://example.test/v1/chat/completions",
        .streaming = true,
    };
}

void enter(FakeSessionView& view, std::string_view text) {
    view.type(text);
    view.push(SessionInputKind::enter);
}

TEST(UserSession, SubmitsTypedInputThroughTheCoordinatorInOrder) {
    TemporarySessionJournal temporary;
    ConversationJournal journal(temporary.path);
    Conversation conversation;
    std::atomic_bool cancellation{false};
    ChatCoordinator coordinator(session_agent_info(), journal, cancellation, conversation);
    CompletionRequestChannel requests;
    FakeSessionView view;
    UserSession session(view, coordinator);

    enter(view, "Question");
    session.receive_terminal_input(requests);

    const std::optional<CompletionRequest> request = requests.get();
    ASSERT_TRUE(request);
    EXPECT_EQ(request->prompt.text, "Question");
    EXPECT_TRUE(request->history.empty());
    ASSERT_EQ(conversation.entries().size(), 1U);
    EXPECT_EQ(conversation.entries().front(), request->prompt);
    const ConversationRestore restored = load_conversation_state(temporary.path);
    ASSERT_EQ(restored.entries.size(), 2U);
    EXPECT_EQ(restored.entries.front(), request->prompt);
    ASSERT_EQ(restored.interrupted_turns.size(), 1U);
    EXPECT_EQ(restored.interrupted_turns.front().request_id, request->request_id);
}

TEST(UserSession, ClearCommandClearsMemoryAndPersistsTheClearEvent) {
    TemporarySessionJournal temporary;
    ConversationJournal journal(temporary.path);
    Conversation conversation;
    const ConversationEntry existing = make_notice_entry(1, "Existing");
    journal.append(existing);
    conversation.add_entry(existing);
    std::atomic_bool cancellation{false};
    ChatCoordinator coordinator(session_agent_info(), journal, cancellation, conversation, 1, 2);
    CompletionRequestChannel requests;
    FakeSessionView view;
    UserSession session(view, coordinator);

    enter(view, "/clear");
    session.receive_terminal_input(requests);
    session.render_if_needed();

    EXPECT_TRUE(conversation.entries().empty());
    EXPECT_TRUE(load_conversation_file(temporary.path).empty());
    EXPECT_EQ(view.rendered_notice, "Conversation cleared");
}

TEST(UserSession, InfoCommandPersistsATypedNotice) {
    TemporarySessionJournal temporary;
    ConversationJournal journal(temporary.path);
    Conversation conversation;
    std::atomic_bool cancellation{false};
    ChatCoordinator coordinator(session_agent_info(), journal, cancellation, conversation);
    CompletionRequestChannel requests;
    FakeSessionView view;
    UserSession session(view, coordinator);

    enter(view, "/info");
    session.receive_terminal_input(requests);

    const std::vector<ConversationEntry> entries = conversation.entries();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().kind, EntryKind::notice);
    EXPECT_NE(entries.front().text.find("Model: test-model"), std::string::npos);
    EXPECT_NE(entries.front().text.find("Transcript entries: 0"), std::string::npos);
    EXPECT_EQ(load_conversation_file(temporary.path), entries);
}

TEST(UserSession, AppliesAndPersistsAgentResponses) {
    TemporarySessionJournal temporary;
    ConversationJournal journal(temporary.path);
    Conversation conversation;
    std::atomic_bool cancellation{false};
    ChatCoordinator coordinator(session_agent_info(), journal, cancellation, conversation);
    CompletionRequestChannel requests;
    AgentEventChannel events;
    FakeSessionView view;
    UserSession session(view, coordinator);

    enter(view, "Question");
    session.receive_terminal_input(requests);
    ASSERT_TRUE(requests.get());
    ASSERT_TRUE(events.push(AgentDelta{1, "Answer"}));
    ASSERT_TRUE(events.push(AgentCompleted{1}));
    session.receive_responses(events);

    ASSERT_EQ(conversation.entries().size(), 2U);
    EXPECT_EQ(conversation.entries().back().text, "Answer");
    EXPECT_EQ(conversation.entries().back().status, CompletionStatus::complete);
    EXPECT_EQ(load_conversation_file(temporary.path), conversation.entries());
}

TEST(UserSession, StopInputDrivesCancellationThroughItsTerminalEvent) {
    TemporarySessionJournal temporary;
    ConversationJournal journal(temporary.path);
    Conversation conversation;
    std::atomic_bool cancellation{false};
    ChatCoordinator coordinator(session_agent_info(), journal, cancellation, conversation);
    CompletionRequestChannel requests;
    AgentEventChannel events;
    FakeSessionView view;
    UserSession session(view, coordinator);

    enter(view, "Question");
    session.receive_terminal_input(requests);
    ASSERT_TRUE(requests.get());
    view.push(SessionInputKind::escape);
    session.receive_terminal_input(requests);
    session.render_if_needed();

    EXPECT_TRUE(cancellation.load(std::memory_order_acquire));
    EXPECT_EQ(view.rendered_notice, "Stopping generation...");

    ASSERT_TRUE(events.push(AgentCancelled{1}));
    session.receive_responses(events);
    EXPECT_FALSE(cancellation.load(std::memory_order_acquire));
    EXPECT_FALSE(coordinator.generating());
    EXPECT_EQ(load_conversation_file(temporary.path), conversation.entries());
}

TEST(UserSession, ClosedAgentChannelStopsTheSession) {
    TemporarySessionJournal temporary;
    ConversationJournal journal(temporary.path);
    Conversation conversation;
    std::atomic_bool cancellation{false};
    ChatCoordinator coordinator(session_agent_info(), journal, cancellation, conversation);
    AgentEventChannel events;
    FakeSessionView view;
    UserSession session(view, coordinator);
    events.close();

    session.receive_responses(events);

    EXPECT_FALSE(session.running());
}

TEST(UserSession, ClosedTerminalInputStopsTheSession) {
    TemporarySessionJournal temporary;
    ConversationJournal journal(temporary.path);
    Conversation conversation;
    std::atomic_bool cancellation{false};
    ChatCoordinator coordinator(session_agent_info(), journal, cancellation, conversation);
    CompletionRequestChannel requests;
    FakeSessionView view;
    view.input_is_closed = true;
    UserSession session(view, coordinator);

    session.receive_terminal_input(requests);

    EXPECT_FALSE(session.running());
}

TEST(UserSession, TerminalFailureStopsAndRendersItsNotice) {
    TemporarySessionJournal temporary;
    ConversationJournal journal(temporary.path);
    Conversation conversation;
    std::atomic_bool cancellation{false};
    ChatCoordinator coordinator(session_agent_info(), journal, cancellation, conversation);
    FakeSessionView view;
    UserSession session(view, coordinator);

    session.report_terminal_failure();
    session.render_if_needed();

    EXPECT_FALSE(session.running());
    EXPECT_EQ(view.render_count, 1);
    EXPECT_EQ(view.rendered_notice, "Terminal input failed.");
}

TEST(UserSession, ShutdownCancelsAnActiveTurnAndClosesRequests) {
    TemporarySessionJournal temporary;
    ConversationJournal journal(temporary.path);
    Conversation conversation;
    std::atomic_bool cancellation{false};
    ChatCoordinator coordinator(session_agent_info(), journal, cancellation, conversation);
    CompletionRequestChannel requests;
    FakeSessionView view;
    UserSession session(view, coordinator);

    enter(view, "Question");
    session.receive_terminal_input(requests);
    session.shutdown(requests);

    EXPECT_TRUE(cancellation.load(std::memory_order_acquire));
    CompletionRequest ignored;
    EXPECT_EQ(requests.try_get(ignored), ChannelReadStatus::value);
    EXPECT_EQ(requests.try_get(ignored), ChannelReadStatus::closed);
    EXPECT_FALSE(requests.push({}));
}

} // namespace
} // namespace cha
