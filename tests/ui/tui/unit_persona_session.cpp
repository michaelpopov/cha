#include "session/session_controller.h"
#include "application/chat_application.h"
#include "agents/completion_backend.h"
#include "session/session_database.h"
#include "support/test_notifier.h"
#include "support/test_controller.h"
#include "support/test_session_database.h"
#include "support/test_workspace.h"
#include "ui/tui/input_editor.h"
#include "ui/tui/session_view.h"
#include "support/test_backends.h"
#include "ui/tui/persona_session.h"

#include <gtest/gtest.h>

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

test::TestNotifier& notifier() {
    static test::TestNotifier instance;
    return instance;
}

std::string selected_author_id() {
    return "operator";
}

// Removes one temporary session database when a controller test leaves scope.
class TemporarySessionJournal {
public:
    TemporarySessionJournal()
      : path(std::filesystem::temp_directory_path()
             / ("cha_persona_session_"
                + std::to_string(
                    std::chrono::steady_clock::now()
                        .time_since_epoch()
                        .count())
                + ".sqlite3")) {
        if (!create_session_database(
                path,
                {
                    .id = "persona-session-test",
                    .forum = "test-forum",
                    .label = "Persona session test",
                })) {
            throw std::runtime_error("Failed to create persona-session test database");
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
        TranscriptView transcript,
        const InputEditor& editor,
        const GenerationStatus& status,
        bool show_addressing,
        std::string_view input_target_name,
        std::string_view notice,
        const ApplicationOverlay* overlay) override {
        ++render_count;
        rendered_entries.assign(
            transcript.entries.begin(),
            transcript.entries.end());
        rendered_input = editor.value();
        rendered_generating = status.active;
        rendered_agent_name = std::move(status.agent_name);
        rendered_phase = status.phase;
        rendered_show_addressing = show_addressing;
        rendered_input_target_name = input_target_name;
        rendered_notice = notice;
        rendered_overlay = overlay ? std::optional<ApplicationOverlay>(*overlay) : std::nullopt;
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

    void reset_session_view() override {
        ++reset_session_count;
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
    std::vector<TranscriptEntry> rendered_entries;
    std::string rendered_input;
    std::string rendered_notice;
    bool rendered_generating{};
    std::string rendered_agent_name;
    ResponsePhase rendered_phase{ResponsePhase::waiting};
    bool rendered_show_addressing{};
    std::string rendered_input_target_name;
    std::optional<ApplicationOverlay> rendered_overlay;
    int render_count{};
    int scroll_up_count{};
    int scroll_down_count{};
    int resize_count{};
    int reset_session_count{};
};

// Echoes or blocks a completion so UI-to-controller behavior is deterministic.
class SessionBackend final : public CompletionBackend {
public:
    explicit SessionBackend(
        bool wait_for_cancellation = false,
        std::string id = "guide-id",
        std::string name = "Guide")
      : id_(std::move(id)),
        name_(std::move(name)),
        wait_for_cancellation_(wait_for_cancellation) {
    }

    RequestPayload prepare(const CompletionInput& input) override {
        return {.bytes = input.run.prompt_text};
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
        on_delta({
            CompletionDeltaKind::answer,
            "Answer to " + payload.bytes,
        });
        return {};
    }

    AgentRuntimeInfo info() const override {
        return {
            .character = {
                .id = id_,
                .name = name_,
            },
            .model = "test-model",
            .api = "test://completion",
            .streaming = true,
        };
    }

private:
    std::string id_;
    std::string name_;
    bool wait_for_cancellation_{};
};

std::vector<std::unique_ptr<CompletionBackend>> two_agents() {
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.push_back(std::make_unique<SessionBackend>(false, "guide-id", "Guide"));
    backends.push_back(std::make_unique<SessionBackend>(false, "ismael", "Ismael"));
    return backends;
}

std::vector<TranscriptEntry> copy_entries(const Transcript& transcript) {
    const auto entries = transcript.entries();
    return {entries.begin(), entries.end()};
}

void enter(FakeSessionView& view, std::string_view text) {
    view.type(text);
    view.push(SessionInputKind::enter);
}

void receive_when_ready(
    SessionController& controller,
    PersonaSession& session) {
    while (controller.generation_status().active) {
        const std::size_t observed = notifier().wake_count();
        session.receive_responses();
        if (controller.generation_status().active
            && !notifier().wait_for_wake(observed)) {
            throw std::runtime_error(
                "Timed out waiting for session response");
        }
    }
}

TEST(PersonaSession, SubmitsEditedInputThroughTheController) {
    TemporarySessionJournal temporary;
    auto controller = test::from_backends_for_testing(
        test::one_backend(std::make_unique<SessionBackend>()),
        temporary.path,
        notifier());
    FakeSessionView view;
    PersonaSession session(view, *controller, selected_author_id());

    enter(view, "Question");
    session.receive_terminal_input();
    receive_when_ready(*controller, session);

    const auto entries = copy_entries(controller->transcript());
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries.front().text, "Question");
    EXPECT_EQ(entries.back().text, "Answer to Question");
    EXPECT_EQ(load_transcript_entries(temporary.path), entries);
}

TEST(PersonaSession, ApplicationCommandsUseOverlayAndResetOnlyAfterASwitch) {
    test::TestWorkspace fixture;
    Workspace workspace(fixture.root());
    test::TestNotifier application_notifier;
    ChatApplication application(workspace, application_notifier);
    FakeSessionView view;
    PersonaSession session(view, application);

    // Application construction enters the chat directly; no selector supplies
    // either the initial session or the input target.
    session.resize();
    session.render_if_needed();
    EXPECT_EQ(application.descriptor().forum_display_name, "Entrance");
    EXPECT_EQ(application.descriptor().session_label, "Welcome");
    EXPECT_EQ(view.rendered_input_target_name, "Assistant");

    enter(view, "/help");
    session.receive_terminal_input();
    session.render_if_needed();
    ASSERT_TRUE(view.rendered_overlay);
    EXPECT_EQ(view.rendered_overlay->title, "Commands");
    EXPECT_EQ(application.controller().transcript().entries().size(), 0U);

    for (int index = 0; index != 8; ++index) {
        view.push(SessionInputKind::page_down);
    }
    view.push(SessionInputKind::resize);
    session.receive_terminal_input();
    session.render_if_needed();
    ASSERT_TRUE(view.rendered_overlay);
    EXPECT_EQ(view.rendered_overlay->first_visible,
              view.rendered_overlay->rows.size() - 1);
    EXPECT_EQ(view.scroll_down_count, 0);
    EXPECT_EQ(view.resize_count, 2);

    view.push(SessionInputKind::escape);
    session.receive_terminal_input();
    session.render_if_needed();
    EXPECT_FALSE(view.rendered_overlay);

    enter(view, "/forums");
    session.receive_terminal_input();
    session.render_if_needed();
    ASSERT_TRUE(view.rendered_overlay);
    EXPECT_EQ(view.rendered_overlay->title, "Forums");
    EXPECT_EQ(view.rendered_overlay->rows, std::vector<std::string>{"The Lobby"});
    view.push(SessionInputKind::escape);

    enter(view, "/personas");
    session.receive_terminal_input();
    session.render_if_needed();
    ASSERT_TRUE(view.rendered_overlay);
    EXPECT_EQ(view.rendered_overlay->title, "Personas");
    EXPECT_EQ(view.rendered_overlay->rows, std::vector<std::string>{"Reader"});
    view.push(SessionInputKind::escape);

    enter(view, "/iam Reader");
    enter(view, "/create \"The Lobby\" \"TUI discussion\"");
    session.receive_terminal_input();
    session.render_if_needed();
    EXPECT_EQ(application.selected_persona(), "Reader");
    EXPECT_EQ(application.descriptor().forum_display_name, "The Lobby");
    EXPECT_EQ(application.descriptor().session_label, "TUI discussion");
    EXPECT_EQ(view.reset_session_count, 1);
    EXPECT_TRUE(view.rendered_input.empty());
    EXPECT_EQ(view.rendered_input_target_name, "Guide");

    enter(view, "/sessions \"The Lobby\"");
    session.receive_terminal_input();
    session.render_if_needed();
    ASSERT_TRUE(view.rendered_overlay);
    EXPECT_EQ(view.rendered_overlay->title, "Sessions in The Lobby");
    EXPECT_EQ(view.rendered_overlay->rows, std::vector<std::string>{"TUI discussion"});
    EXPECT_TRUE(view.rendered_entries.empty());
    view.push(SessionInputKind::escape);
    session.receive_terminal_input();

    SessionController* const before_failed_open = &application.controller();
    enter(view, "/open \"The Lobby\" \"Missing\"");
    session.receive_terminal_input();
    session.render_if_needed();
    EXPECT_EQ(&application.controller(), before_failed_open);
    EXPECT_EQ(view.reset_session_count, 1);
    EXPECT_EQ(view.rendered_input_target_name, "Guide");

    // The old chat remains usable after a recoverable switch failure.
    view.type("draft after failed open");
    session.receive_terminal_input();
    session.render_if_needed();
    EXPECT_EQ(view.rendered_input, "draft after failed open");

    // Stateful application commands are rejected immediately while generation
    // is active and remain available as a draft rather than being delayed.
    view.push(SessionInputKind::escape);
    session.receive_terminal_input();
    enter(view, "Keep working");
    session.receive_terminal_input();
    ASSERT_TRUE(application.controller().is_generating());
    enter(view, "/forums");
    session.receive_terminal_input();
    session.render_if_needed();
    EXPECT_EQ(view.rendered_input, "/forums");
    EXPECT_EQ(view.rendered_notice,
              "Generation in progress; use /stop, Esc, or Ctrl-C");
    EXPECT_FALSE(view.rendered_overlay);

    session.shutdown();
    session.report_terminal_failure();
    session.render_if_needed();
    EXPECT_FALSE(session.running());
}

TEST(PersonaSession, DelegatesClearAndInfoCommandsToTheController) {
    TemporarySessionJournal temporary;
    auto controller = test::from_backends_for_testing(
        test::one_backend(std::make_unique<SessionBackend>()),
        temporary.path,
        notifier());
    FakeSessionView view;
    PersonaSession session(view, *controller, selected_author_id());

    enter(view, "/info");
    session.receive_terminal_input();
    session.render_if_needed();
    EXPECT_TRUE(controller->transcript().entries().empty());
    EXPECT_TRUE(load_transcript_entries(temporary.path).empty());
    EXPECT_NE(
        view.rendered_notice.find("Transcript entries: 0"),
        std::string::npos);

    enter(view, "/clear");
    session.receive_terminal_input();
    session.render_if_needed();

    EXPECT_TRUE(controller->transcript().entries().empty());
    EXPECT_TRUE(load_transcript_entries(temporary.path).empty());
    EXPECT_EQ(view.rendered_notice, "Transcript cleared");
}

TEST(PersonaSession, StopInputDrivesControllerCancellation) {
    TemporarySessionJournal temporary;
    auto controller = test::from_backends_for_testing(
        test::one_backend(std::make_unique<SessionBackend>(true)),
        temporary.path,
        notifier());
    FakeSessionView view;
    PersonaSession session(view, *controller, selected_author_id());

    enter(view, "Question");
    session.receive_terminal_input();
    view.push(SessionInputKind::escape);
    session.receive_terminal_input();
    session.render_if_needed();

    EXPECT_EQ(view.rendered_notice, "Stopping generation...");
    EXPECT_TRUE(view.rendered_generating);
    EXPECT_EQ(view.rendered_agent_name, "Guide");
    EXPECT_EQ(view.rendered_phase, ResponsePhase::stopping);
    receive_when_ready(*controller, session);
    EXPECT_FALSE(controller->generation_status().active);
    EXPECT_EQ(
        load_transcript_entries(temporary.path),
        copy_entries(controller->transcript()));
}

TEST(PersonaSession, PreservesADraftRejectedDuringGeneration) {
    TemporarySessionJournal temporary;
    auto controller = test::from_backends_for_testing(
        test::one_backend(std::make_unique<SessionBackend>(true)),
        temporary.path,
        notifier());
    FakeSessionView view;
    PersonaSession session(view, *controller, selected_author_id());

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

TEST(PersonaSession, ConsumesStopCommandDuringGeneration) {
    TemporarySessionJournal temporary;
    auto controller = test::from_backends_for_testing(
        test::one_backend(std::make_unique<SessionBackend>(true)),
        temporary.path,
        notifier());
    FakeSessionView view;
    PersonaSession session(view, *controller, selected_author_id());

    enter(view, "Question");
    session.receive_terminal_input();
    enter(view, "/stop");
    session.receive_terminal_input();
    session.render_if_needed();

    EXPECT_TRUE(view.rendered_input.empty());
    EXPECT_EQ(view.rendered_notice, "Stopping generation...");
    receive_when_ready(*controller, session);
}

TEST(PersonaSession, ExitCommandStopsTheSession) {
    TemporarySessionJournal temporary;
    auto controller = test::from_backends_for_testing(
        test::one_backend(std::make_unique<SessionBackend>()),
        temporary.path,
        notifier());
    FakeSessionView view;
    PersonaSession session(view, *controller, selected_author_id());

    enter(view, "/exit");
    session.receive_terminal_input();

    EXPECT_FALSE(session.running());
}

TEST(PersonaSession, ClosedAgentEventQueueStopsTheSession) {
    TemporarySessionJournal temporary;
    auto controller = test::from_backends_for_testing(
        test::one_backend(std::make_unique<SessionBackend>()),
        temporary.path,
        notifier());
    FakeSessionView view;
    PersonaSession session(view, *controller, selected_author_id());
    controller->shutdown();

    session.receive_responses();

    EXPECT_FALSE(session.running());
}

TEST(PersonaSession, PollReportedTerminalClosureStopsTheSession) {
    TemporarySessionJournal temporary;
    auto controller = test::from_backends_for_testing(
        test::one_backend(std::make_unique<SessionBackend>()),
        temporary.path,
        notifier());
    FakeSessionView view;
    PersonaSession session(view, *controller, selected_author_id());

    session.close_terminal();

    EXPECT_FALSE(session.running());
}

TEST(PersonaSession, TerminalFailureStopsAndRendersItsNotice) {
    TemporarySessionJournal temporary;
    auto controller = test::from_backends_for_testing(
        test::one_backend(std::make_unique<SessionBackend>()),
        temporary.path,
        notifier());
    FakeSessionView view;
    PersonaSession session(view, *controller, selected_author_id());

    session.report_terminal_failure();
    session.render_if_needed();

    EXPECT_FALSE(session.running());
    EXPECT_EQ(view.render_count, 1);
    EXPECT_EQ(view.rendered_notice, "Terminal input failed.");
}

TEST(PersonaSession, RendersTheGeneratingAgentByName) {
    TemporarySessionJournal temporary;
    auto controller = test::from_backends_for_testing(
        two_agents(),
        temporary.path,
        notifier());
    FakeSessionView view;
    PersonaSession session(view, *controller, selected_author_id());

    session.resize();
    session.render_if_needed();
    EXPECT_FALSE(view.rendered_generating);
    EXPECT_TRUE(view.rendered_agent_name.empty());

    enter(view, "@Ismael Question");
    session.receive_terminal_input();
    session.render_if_needed();

    EXPECT_TRUE(view.rendered_generating);
    EXPECT_EQ(view.rendered_agent_name, "Ismael");

    receive_when_ready(*controller, session);
    session.render_if_needed();
    EXPECT_FALSE(view.rendered_generating);
    EXPECT_TRUE(view.rendered_agent_name.empty());
}

TEST(PersonaSession, RendersAddressingWheneverTheForumHostsSeveralAgents) {
    TemporarySessionJournal temporary;
    auto controller = test::from_backends_for_testing(
        two_agents(),
        temporary.path,
        notifier());
    FakeSessionView view;
    PersonaSession session(view, *controller, selected_author_id());

    session.resize();
    session.render_if_needed();
    EXPECT_TRUE(view.rendered_show_addressing);

    enter(view, "/clear");
    session.receive_terminal_input();
    session.render_if_needed();
    EXPECT_TRUE(view.rendered_show_addressing)
        << "a forum with multiple characters keeps showing every prompt's target";
}

TEST(PersonaSession, PreviewsTheDefaultOrLeadingMentionedInputTarget) {
    TemporarySessionJournal temporary;
    auto controller = test::from_backends_for_testing(
        two_agents(),
        temporary.path,
        notifier());
    FakeSessionView view;
    PersonaSession session(view, *controller, selected_author_id());

    session.resize();
    session.render_if_needed();
    EXPECT_EQ(view.rendered_input_target_name, "Guide");

    view.type("@Ismael Hello");
    session.receive_terminal_input();
    session.render_if_needed();
    EXPECT_EQ(view.rendered_input_target_name, "Ismael");
}

TEST(PersonaSession, RendersASingleAgentForumWithoutAddressingUntilItsHistorySaysOtherwise) {
    TemporarySessionJournal temporary;
    {
        auto controller = test::from_backends_for_testing(
            test::one_backend(std::make_unique<SessionBackend>()),
            temporary.path,
            notifier());
        FakeSessionView view;
        PersonaSession session(view, *controller, selected_author_id());
        session.resize();
        session.render_if_needed();
        EXPECT_FALSE(view.rendered_show_addressing);

        enter(view, "Question");
        session.receive_terminal_input();
        receive_when_ready(*controller, session);
        session.render_if_needed();
        EXPECT_FALSE(view.rendered_show_addressing);
    }

    // Reopening with history from an agent that has since left the forum.
    SessionRestore restored = load_session_state(temporary.path);
    restored.entries.front().addressed_to = "departed";
    restored.entries.front().addressed_to_name = "Departed";
    auto reopened = test::from_backends_for_testing(
        test::one_backend(std::make_unique<SessionBackend>()),
        temporary.path,
        notifier(),
        std::move(restored));
    FakeSessionView view;
    PersonaSession session(view, *reopened, selected_author_id());

    session.resize();
    session.render_if_needed();
    EXPECT_TRUE(view.rendered_show_addressing);

    enter(view, "/clear");
    session.receive_terminal_input();
    session.render_if_needed();
    EXPECT_FALSE(view.rendered_show_addressing)
        << "clearing removes the only reason a one-agent forum showed addressing";
}

TEST(PersonaSession, ShutdownPersistsCancellationOfAnActiveTurn) {
    TemporarySessionJournal temporary;
    auto controller = test::from_backends_for_testing(
        test::one_backend(std::make_unique<SessionBackend>(true)),
        temporary.path,
        notifier());
    FakeSessionView view;
    PersonaSession session(view, *controller, selected_author_id());

    enter(view, "Question");
    session.receive_terminal_input();
    session.shutdown();

    EXPECT_FALSE(controller->generation_status().active);
    EXPECT_EQ(
        load_transcript_entries(temporary.path),
        copy_entries(controller->transcript()));
}

} // namespace
} // namespace cha
