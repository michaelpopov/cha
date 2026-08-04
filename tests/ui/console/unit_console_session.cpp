#include "application/chat_application.h"
#include "session/workspace.h"
#include "support/scripted_console.h"
#include "support/test_workspace.h"
#include "ui/console/console_session.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

namespace cha {
namespace {

void receive_until_idle(SessionController& controller) {
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(1);
    while (controller.is_generating()) {
        (void)controller.receive();
        ASSERT_LT(std::chrono::steady_clock::now(), deadline);
        std::this_thread::yield();
    }
}

TEST(ConsoleSession, NavigationUsesNoticesAndTheNewCurrentSession) {
    test::TestWorkspace fixture;
    Workspace workspace(fixture.root());
    test::ScriptedConsole port({
        {.input = true, .closed = true,
         .lines = {"/create \"The Lobby\" \"Saved discussion\"", "after switch"}},
        {.notification = true, .repeat = true},
    });
    ChatApplication application(workspace, port);
    ConsoleSession session(port, application);

    EXPECT_EQ(session.run(), 0);
    EXPECT_EQ(application.descriptor().forum_display_name, "The Lobby");
    EXPECT_EQ(application.descriptor().session_label, "Saved discussion");
    EXPECT_NE(port.notice_output().find("Opened session 'Saved discussion' in forum 'The Lobby'"), std::string::npos);
    EXPECT_NE(port.transcript_output().find("after switch"), std::string::npos);
    EXPECT_EQ(port.transcript_output().find("builtin-guest"), std::string::npos);
}

TEST(ConsoleSession, ListsStayOnTheNoticeStream) {
    test::TestWorkspace fixture;
    Workspace workspace(fixture.root());
    test::ScriptedConsole port({
        {.input = true, .closed = true, .lines = {"/forums", "/personas", "/help"}},
    });
    ChatApplication application(workspace, port);
    ConsoleSession session(port, application);

    EXPECT_EQ(session.run(), 0);
    EXPECT_NE(port.notice_output().find("Forums:"), std::string::npos);
    EXPECT_NE(port.notice_output().find("The Lobby"), std::string::npos);
    EXPECT_NE(port.notice_output().find("Personas:"), std::string::npos);
    EXPECT_NE(port.notice_output().find("Commands:"), std::string::npos);
    EXPECT_TRUE(port.transcript_output().empty());
}

TEST(ConsoleSession, PromptTracksTheNewForumDefaultAgent) {
    test::TestWorkspace fixture;
    Workspace workspace(fixture.root());
    test::ScriptedConsole port({
        {.input = true, .lines = {"/create \"The Lobby\" \"Prompt test\""}},
        {.signal = true},
    });
    ChatApplication application(workspace, port);
    ConsoleSession session(port, application, {.show_prompt = true});

    EXPECT_EQ(session.run(), 0);
    EXPECT_NE(port.notice_output().find("@Assistant> "), std::string::npos);
    EXPECT_NE(port.notice_output().find("@Guide> "), std::string::npos);
}

TEST(ConsoleSession, FlushFailureBeforeSwitchFailsTheSession) {
    test::TestWorkspace fixture;
    Workspace workspace(fixture.root());
    test::ScriptedConsole port({
        {.input = true, .closed = true,
         .lines = {"/create \"The Lobby\" \"Unwritten\""}},
    });
    ChatApplication application(workspace, port);
    ConsoleSession session(port, application);
    // The first flush initializes Welcome; the second is the mandated
    // pre-switch flush that protects its append-only suffix.
    port.fail_flush_on(2);

    EXPECT_EQ(session.run(), 1);
    EXPECT_EQ(application.descriptor().session_label, "Welcome");
    EXPECT_NE(port.notice_output().find("Failed to write console transcript."), std::string::npos);
}

TEST(ConsoleSession, RejectsNavigationImmediatelyWhileGenerating) {
    test::TestWorkspace fixture;
    Workspace workspace(fixture.root());
    test::ScriptedConsole port({
        {.input = true, .closed = true,
         .lines = {"/open \"The Lobby\" \"Later\""}},
        {.notification = true, .repeat = true},
    });
    ChatApplication application(workspace, port);
    (void)application.controller().submit_prompt(
        application.selected_author_key(), "Keep working");
    ASSERT_TRUE(application.controller().is_generating());
    ConsoleSession session(port, application);

    EXPECT_EQ(session.run(), 0);
    EXPECT_EQ(application.descriptor().session_label, "Welcome");
    EXPECT_NE(port.notice_output().find(generation_in_progress_notice), std::string::npos);
}

TEST(ConsoleSession, IamChangesSubsequentTranscriptAuthorship) {
    test::TestWorkspace fixture;
    Workspace workspace(fixture.root());
    test::ScriptedConsole port({
        {.input = true, .closed = true, .lines = {"/iam Reader", "hello"}},
        {.notification = true, .repeat = true},
    });
    ChatApplication application(workspace, port);
    ConsoleSession session(port, application);

    EXPECT_EQ(session.run(), 0);
    EXPECT_NE(port.transcript_output().find("[Reader] hello"), std::string::npos);
    EXPECT_NE(port.notice_output().find("Now speaking as Reader"), std::string::npos);
}

TEST(ConsoleSession, EmitsRestoredHistoryBeforeFollowingSwitchInput) {
    test::TestWorkspace fixture;
    Workspace workspace(fixture.root());
    test::ScriptedConsole port({
        {.input = true, .closed = true,
         .lines = {"/open \"The Lobby\" \"Saved discussion\"", "after switch"}},
        {.notification = true, .repeat = true},
    });
    ChatApplication application(workspace, port);
    ASSERT_TRUE(application.create("The Lobby", "Saved discussion").session_changed);
    (void)application.controller().submit_prompt(
        application.selected_author_key(), "Earlier");
    receive_until_idle(application.controller());
    ASSERT_TRUE(application.open("Entrance", "Welcome").session_changed);
    ConsoleSession session(port, application, {.show_prompt = true});

    EXPECT_EQ(session.run(), 0);
    EXPECT_NE(port.transcript_output().find("[Guest] Earlier"), std::string::npos);
    EXPECT_EQ(port.transcript_output().find("[Guest] after switch"), std::string::npos);
}

} // namespace
} // namespace cha
