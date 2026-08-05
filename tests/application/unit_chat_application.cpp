#include "application/chat_application.h"
#include "session/generation_status.h"
#include "session/catalog_lease.h"
#include "support/test_notifier.h"
#include "support/test_workspace.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
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

TEST_F(ChatApplicationTest, ListsForumMembersByPublicNameWithoutChangingSession) {
    const ApplicationResult lobby = application.members("the lobby");
    ASSERT_TRUE(lobby.list);
    EXPECT_EQ(lobby.list->title, "Members of The Lobby");
    EXPECT_EQ(lobby.list->rows, (std::vector<std::string>{"Guide"}));
    EXPECT_TRUE(lobby.input_consumed);
    EXPECT_FALSE(lobby.session_changed);
    EXPECT_EQ(application.descriptor().session_label, "Welcome");

    const ApplicationResult entrance = application.members("Entrance");
    ASSERT_TRUE(entrance.list);
    EXPECT_EQ(entrance.list->rows, (std::vector<std::string>{"Assistant"}));

    const ApplicationResult unknown = application.members("Missing");
    EXPECT_FALSE(unknown.list);
    ASSERT_TRUE(unknown.notice);
    EXPECT_EQ(*unknown.notice, "Unknown forum 'Missing'");

    // The members directory name is a private storage key, not a public name.
    const ApplicationResult key = application.members("lobby");
    EXPECT_FALSE(key.list);
    ASSERT_TRUE(key.notice);
    EXPECT_EQ(*key.notice, "Unknown forum 'lobby'");
}

TEST_F(ChatApplicationTest, MemberListingIsRefusedWhileGenerating) {
    (void)application.controller().submit_prompt(
        application.selected_author_key(), "Keep working");
    ASSERT_TRUE(application.controller().is_generating());

    const ApplicationResult result = application.members("The Lobby");
    EXPECT_FALSE(result.list);
    EXPECT_FALSE(result.input_consumed);
    ASSERT_TRUE(result.notice);
    EXPECT_EQ(*result.notice, std::string(generation_in_progress_notice));
}

TEST_F(ChatApplicationTest, RepeatingTheCurrentPersonaIsAnIdempotentSuccess) {
    const ApplicationResult guest = application.iam("guest");
    EXPECT_TRUE(guest.input_consumed);
    EXPECT_FALSE(guest.persona_changed);
    ASSERT_TRUE(guest.notice);
    EXPECT_EQ(*guest.notice, "Already speaking as Guest");

    ASSERT_TRUE(application.iam("Reader").persona_changed);
    const ApplicationResult reader = application.iam("reader");
    EXPECT_TRUE(reader.input_consumed);
    EXPECT_FALSE(reader.persona_changed);
    ASSERT_TRUE(reader.notice);
    EXPECT_EQ(*reader.notice, "Already speaking as Reader");
}

TEST_F(ChatApplicationTest, PreservesPublicInvalidSessionNameDiagnostics) {
    const ApplicationResult created = application.create("The Lobby", " invalid");
    ASSERT_TRUE(created.notice);
    EXPECT_EQ(*created.notice, "Session name is not a valid public name");

    const ApplicationResult opened = application.open("The Lobby", " invalid");
    ASSERT_TRUE(opened.notice);
    EXPECT_EQ(*opened.notice, "Session name is not a valid public name");
    EXPECT_EQ(application.descriptor().session_label, "Welcome");
}

TEST_F(ChatApplicationTest, ReportsCatalogContentionAsARetryablePublicError) {
    const std::filesystem::path sessions =
        fixture.root() / "forums" / "lobby" / "sessions";
    std::filesystem::create_directories(sessions);
    {
        CatalogLease held = CatalogLease::acquire(sessions);
        const ApplicationResult result =
            application.create("The Lobby", "Contended");
        ASSERT_TRUE(result.notice);
        EXPECT_EQ(*result.notice,
            "Session catalog for forum 'The Lobby' is busy; try again");
        EXPECT_FALSE(result.session_changed);
        EXPECT_EQ(application.descriptor().session_label, "Welcome");
    }

    const ApplicationResult retried =
        application.create("The Lobby", "Contended");
    EXPECT_TRUE(retried.session_changed);
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

TEST_F(ChatApplicationTest, DoesNotExposeStoragePathsInListingFailures) {
    const std::filesystem::path sessions =
        fixture.root() / "forums" / "lobby" / "sessions";
    std::ofstream(sessions) << "not a directory";

    const ApplicationResult result = application.sessions("The Lobby");
    ASSERT_TRUE(result.notice);
    EXPECT_EQ(*result.notice, "Unable to list sessions in forum 'The Lobby'");
    EXPECT_EQ(result.notice->find(fixture.root().string()), std::string::npos);
    EXPECT_EQ(result.notice->find("lobby/sessions"), std::string::npos);
}

TEST_F(ChatApplicationTest, ValidatesDefinitionsBeforePublishingANewSession) {
    std::ofstream(fixture.root() / "characters" / "guide" / "character.toml")
        << "display_name = [invalid";

    const ApplicationResult result = application.create("The Lobby", "Broken");
    ASSERT_TRUE(result.notice);
    EXPECT_EQ(*result.notice,
        "Unable to create session 'Broken' in forum 'The Lobby'");
    const std::filesystem::path sessions =
        fixture.root() / "forums" / "lobby" / "sessions";
    EXPECT_FALSE(std::filesystem::exists(sessions));
}

TEST_F(ChatApplicationTest, RetainsAndReleasesAPublishedSessionWhenControllerConstructionFails) {
    fixture.write_character_defaults(
        "host = \"test\"\n"
        "port = 1\n"
        "mode = \"test\"\n"
        "model = \"fake\"\n"
        "api_key_env = \"__CHA_TEST_MISSING_PUBLISHED_SESSION_KEY__\"\n");

    const ApplicationResult failed = application.create("The Lobby", "Published");
    ASSERT_TRUE(failed.notice);
    EXPECT_EQ(*failed.notice,
        "Session 'Published' was created in forum 'The Lobby' but could not be opened: session initialization failed");
    EXPECT_FALSE(failed.session_changed);
    EXPECT_EQ(application.descriptor().session_label, "Welcome");

    const ApplicationResult listed = application.sessions("The Lobby");
    ASSERT_TRUE(listed.list);
    EXPECT_EQ(listed.list->rows, (std::vector<std::string>{"Published"}));

    fixture.write_character_defaults(
        "host = \"test\"\n"
        "port = 1\n"
        "mode = \"test\"\n"
        "model = \"fake\"\n");
    const ApplicationResult opened = application.open("The Lobby", "Published");
    EXPECT_TRUE(opened.session_changed);
    EXPECT_EQ(application.descriptor().session_label, "Published");
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
