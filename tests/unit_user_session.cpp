#include "chat_coordinator.h"
#include "completion_backend.h"
#include "session_database.h"
#include "input_editor.h"
#include "session_view.h"
#include "user_session.h"

#include <gtest/gtest.h>

#include <poll.h>

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace cha {
namespace {

// Removes one temporary session database when a controller test leaves scope.
class TemporarySessionJournal {
public:
    TemporarySessionJournal()
      : path(std::filesystem::temp_directory_path()
             / ("cha_user_session_"
                + std::to_string(
                    std::chrono::steady_clock::now()
                        .time_since_epoch()
                        .count())
                + ".sqlite3")) {
        if (!create_session_database(
                path,
                {
                    .id = "user-session-test",
                    .room = "test-room",
                    .persona = "test-persona",
                    .label = "User session test",
                })) {
            throw std::runtime_error("Failed to create user-session test database");
        }
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

// Echoes or blocks a completion so UI-to-coordinator behavior is deterministic.
class SessionBackend final : public CompletionBackend {
public:
    explicit SessionBackend(bool wait_for_cancellation = false)
      : wait_for_cancellation_(wait_for_cancellation) {
    }

    RequestPayload prepare(
        const CompletionRequest& request,
        const ConversationReadView&) override {
        return {.bytes = request.prompt.text};
    }

    CompletionResult perform(
        RequestPayload payload,
        const CompletionDeltaSink& on_delta,
        const std::atomic_bool& cancellation) override {
        if (wait_for_cancellation_) {
            while (!cancellation.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            return {CompletionOutcome::cancelled, {}};
        }
        on_delta("Answer to " + payload.bytes);
        return {};
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

private:
    std::string id_{"guide-id"};
    bool wait_for_cancellation_{};
};

void enter(FakeSessionView& view, std::string_view text) {
    view.type(text);
    view.push(SessionInputKind::enter);
}

void receive_when_ready(
    ChatCoordinator& coordinator,
    UserSession& session) {
    pollfd descriptor{
        coordinator.notification_fd(),
        POLLIN,
        0,
    };
    if (::poll(&descriptor, 1, 1000) != 1) {
        throw std::runtime_error(
            "Timed out waiting for session response");
    }
    session.receive_responses();
}

TEST(UserSession, SubmitsEditedInputThroughTheCoordinator) {
    TemporarySessionJournal temporary;
    ChatCoordinator coordinator(
        std::make_unique<SessionBackend>(),
        temporary.path);
    FakeSessionView view;
    UserSession session(view, coordinator);

    enter(view, "Question");
    session.receive_terminal_input();
    receive_when_ready(coordinator, session);

    const auto entries = coordinator.conversation().entries();
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries.front().text, "Question");
    EXPECT_EQ(entries.back().text, "Answer to Question");
    EXPECT_EQ(load_conversation_entries(temporary.path), entries);
}

TEST(UserSession, DelegatesClearAndInfoCommandsToTheCoordinator) {
    TemporarySessionJournal temporary;
    ChatCoordinator coordinator(
        std::make_unique<SessionBackend>(),
        temporary.path);
    FakeSessionView view;
    UserSession session(view, coordinator);

    enter(view, "/info");
    session.receive_terminal_input();
    ASSERT_EQ(coordinator.conversation().entries().size(), 1U);
    EXPECT_EQ(
        coordinator.conversation().entries().front().kind,
        EntryKind::notice);

    enter(view, "/clear");
    session.receive_terminal_input();
    session.render_if_needed();

    EXPECT_TRUE(coordinator.conversation().entries().empty());
    EXPECT_TRUE(load_conversation_entries(temporary.path).empty());
    EXPECT_EQ(view.rendered_notice, "Conversation cleared");
}

TEST(UserSession, StopInputDrivesCoordinatorCancellation) {
    TemporarySessionJournal temporary;
    ChatCoordinator coordinator(
        std::make_unique<SessionBackend>(true),
        temporary.path);
    FakeSessionView view;
    UserSession session(view, coordinator);

    enter(view, "Question");
    session.receive_terminal_input();
    view.push(SessionInputKind::escape);
    session.receive_terminal_input();
    session.render_if_needed();

    EXPECT_EQ(view.rendered_notice, "Stopping generation...");
    receive_when_ready(coordinator, session);
    EXPECT_FALSE(coordinator.generating());
    EXPECT_EQ(
        load_conversation_entries(temporary.path),
        coordinator.conversation().entries());
}

TEST(UserSession, PreservesADraftRejectedDuringGeneration) {
    TemporarySessionJournal temporary;
    ChatCoordinator coordinator(
        std::make_unique<SessionBackend>(true),
        temporary.path);
    FakeSessionView view;
    UserSession session(view, coordinator);

    enter(view, "Question");
    session.receive_terminal_input();
    enter(view, "Keep this draft");
    session.receive_terminal_input();
    session.render_if_needed();

    EXPECT_EQ(view.rendered_input, "Keep this draft");
    EXPECT_EQ(
        view.rendered_notice,
        "Generation in progress; use /stop, Esc, or Ctrl-C");
    session.shutdown();
}

TEST(UserSession, ConsumesStopCommandDuringGeneration) {
    TemporarySessionJournal temporary;
    ChatCoordinator coordinator(
        std::make_unique<SessionBackend>(true),
        temporary.path);
    FakeSessionView view;
    UserSession session(view, coordinator);

    enter(view, "Question");
    session.receive_terminal_input();
    enter(view, "/stop");
    session.receive_terminal_input();
    session.render_if_needed();

    EXPECT_TRUE(view.rendered_input.empty());
    EXPECT_EQ(view.rendered_notice, "Stopping generation...");
    receive_when_ready(coordinator, session);
}

TEST(UserSession, ExitCommandStopsTheSession) {
    TemporarySessionJournal temporary;
    ChatCoordinator coordinator(
        std::make_unique<SessionBackend>(),
        temporary.path);
    FakeSessionView view;
    UserSession session(view, coordinator);

    enter(view, "/exit");
    session.receive_terminal_input();

    EXPECT_FALSE(session.running());
}

TEST(UserSession, ClosedWorkerStopsTheSession) {
    TemporarySessionJournal temporary;
    ChatCoordinator coordinator(
        std::make_unique<SessionBackend>(),
        temporary.path);
    FakeSessionView view;
    UserSession session(view, coordinator);
    coordinator.shutdown();

    session.receive_responses();

    EXPECT_FALSE(session.running());
}

TEST(UserSession, ClosedTerminalInputStopsTheSession) {
    TemporarySessionJournal temporary;
    ChatCoordinator coordinator(
        std::make_unique<SessionBackend>(),
        temporary.path);
    FakeSessionView view;
    view.input_is_closed = true;
    UserSession session(view, coordinator);

    session.receive_terminal_input();

    EXPECT_FALSE(session.running());
}

TEST(UserSession, TerminalFailureStopsAndRendersItsNotice) {
    TemporarySessionJournal temporary;
    ChatCoordinator coordinator(
        std::make_unique<SessionBackend>(),
        temporary.path);
    FakeSessionView view;
    UserSession session(view, coordinator);

    session.report_terminal_failure();
    session.render_if_needed();

    EXPECT_FALSE(session.running());
    EXPECT_EQ(view.render_count, 1);
    EXPECT_EQ(view.rendered_notice, "Terminal input failed.");
}

TEST(UserSession, ShutdownPersistsCancellationOfAnActiveTurn) {
    TemporarySessionJournal temporary;
    ChatCoordinator coordinator(
        std::make_unique<SessionBackend>(true),
        temporary.path);
    FakeSessionView view;
    UserSession session(view, coordinator);

    enter(view, "Question");
    session.receive_terminal_input();
    session.shutdown();

    EXPECT_FALSE(coordinator.generating());
    EXPECT_EQ(
        load_conversation_entries(temporary.path),
        coordinator.conversation().entries());
}

} // namespace
} // namespace cha
