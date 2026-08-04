#include "application/chat_application.h"
#include "session/generation_status.h"
#include "support/test_notifier.h"
#include "support/test_workspace.h"

#include <gtest/gtest.h>

#include <vector>

namespace cha {
namespace {

void receive_until_idle(SessionController& controller, test::TestNotifier& notifier) {
    while (controller.generation_status().active) {
        const std::size_t observed = notifier.wake_count();
        (void)controller.receive();
        if (controller.generation_status().active) {
            ASSERT_TRUE(notifier.wait_for_wake(observed));
        }
    }
}

class ChatApplicationTest : public ::testing::Test {
protected:
    test::TestWorkspace fixture;
    Workspace workspace{fixture.root()};
    test::TestNotifier notifier;
    ChatApplication application{workspace, notifier};
};

TEST_F(ChatApplicationTest, StartsAsGuestInEntranceWelcome) {
    EXPECT_EQ(application.selected_persona(), "Guest");
    EXPECT_EQ(application.selected_author_key(), "builtin-guest");
    EXPECT_EQ(application.descriptor().forum_display_name, "Entrance");
    EXPECT_EQ(application.descriptor().session_label, "Welcome");
}

TEST_F(ChatApplicationTest, PreservesPersonaAndWelcomeTranscriptAcrossSwitches) {
    ASSERT_TRUE(application.iam("Reader").persona_changed);
    (void)application.controller().submit_prompt(application.selected_author_key(), "Remember this");
    receive_until_idle(application.controller(), notifier);
    ASSERT_EQ(application.controller().transcript().entries().size(), 2U);

    const ApplicationResult created = application.create("The Lobby", "Discussion");
    ASSERT_TRUE(created.session_changed);
    EXPECT_EQ(application.selected_persona(), "Reader");

    const ApplicationResult reopened = application.open("entrance", "welcome");
    ASSERT_TRUE(reopened.session_changed);
    EXPECT_EQ(application.descriptor().forum_display_name, "Entrance");
    EXPECT_EQ(application.descriptor().session_label, "Welcome");
    EXPECT_EQ(application.controller().transcript().entries().size(), 2U);
}

TEST_F(ChatApplicationTest, UsesPublicNamesAndPreservesStateOnRecoverableFailures) {
    const ApplicationResult unknown = application.open("Missing", "Session");
    ASSERT_TRUE(unknown.notice);
    EXPECT_EQ(*unknown.notice, "Unknown forum 'Missing'");
    EXPECT_EQ(application.descriptor().session_label, "Welcome");

    const ApplicationResult reserved = application.create("Entrance", "Welcome");
    ASSERT_TRUE(reserved.notice);
    EXPECT_EQ(*reserved.notice, "Session 'Welcome' is reserved in forum 'Entrance'");
    EXPECT_EQ(application.descriptor().session_label, "Welcome");

    const ApplicationResult same = application.open("Entrance", "Welcome");
    EXPECT_FALSE(same.session_changed);
    ASSERT_TRUE(same.notice);
    EXPECT_EQ(*same.notice, "Session 'Welcome' is already open");
}

TEST_F(ChatApplicationTest, ListsCustomEntitiesAndKeepsAuthoredCasingInEmptyMessages) {
    const ApplicationResult forum_rows = application.forums();
    ASSERT_TRUE(forum_rows.list);
    EXPECT_EQ(forum_rows.list->rows, (std::vector<std::string>{"The Lobby"}));

    const ApplicationResult persona_rows = application.personas();
    ASSERT_TRUE(persona_rows.list);
    EXPECT_EQ(persona_rows.list->rows, (std::vector<std::string>{"Reader"}));

    const ApplicationResult empty = application.sessions("entrance");
    ASSERT_TRUE(empty.notice);
    EXPECT_EQ(*empty.notice, "No stored sessions in forum 'Entrance'");
}

TEST_F(ChatApplicationTest, RejectsEveryStatefulApplicationOperationDuringGeneration) {
    (void)application.controller().submit_prompt(application.selected_author_key(), "Keep working");
    ASSERT_TRUE(application.controller().is_generating());

    const std::vector<ApplicationResult> results{
        application.iam("Reader"),
        application.forums(),
        application.personas(),
        application.sessions("Entrance"),
        application.open("Entrance", "Welcome"),
        application.create("The Lobby", "Blocked"),
    };
    for (const ApplicationResult& result : results) {
        ASSERT_TRUE(result.notice);
        EXPECT_EQ(*result.notice, generation_in_progress_notice);
        EXPECT_FALSE(result.input_consumed);
    }
    EXPECT_EQ(application.selected_persona(), "Guest");
    EXPECT_EQ(application.descriptor().session_label, "Welcome");
}

} // namespace
} // namespace cha
