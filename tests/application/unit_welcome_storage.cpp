#include "application/welcome_storage.h"
#include "application/environment.h"
#include "session/session_database.h"
#include "support/test_notifier.h"
#include "support/test_workspace.h"

#include <gtest/gtest.h>

TEST(WelcomeStorage, IsFreshAndPrivateToItsOwner) {
    cha::WelcomeStorage first;
    cha::WelcomeStorage second;
    EXPECT_NE(first.database_path(), second.database_path());
    EXPECT_TRUE(cha::load_session_state(first.database_path()).entries.empty());
    const auto path = first.directory();
    EXPECT_TRUE(std::filesystem::exists(path));
}

TEST(WelcomeStorage, RemovesOnlyItsOwnedDirectory) {
    std::filesystem::path path;
    {
        cha::WelcomeStorage storage;
        path = storage.directory();
        ASSERT_TRUE(std::filesystem::exists(path));
    }
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(WelcomeStorage, EnvironmentOpensGuestEntranceWelcomeWithSharedRoster) {
    cha::test::TestWorkspace fixture;
    cha::Workspace workspace(fixture.root());
    cha::ApplicationEnvironment environment(workspace);
    cha::test::NoopNotifier notifier;
    cha::OpenedSession opened = environment.open_welcome(notifier);
    EXPECT_EQ(opened.descriptor.forum_display_name, "Entrance");
    EXPECT_EQ(opened.descriptor.session_label, "Welcome");
    EXPECT_TRUE(opened.controller->transcript().entries().empty());
    EXPECT_EQ(opened.controller->persona_roster().get(), environment.personas().roster().get());
}

TEST(WelcomeStorage, ReopensOnlyTheSameEnvironmentWelcomeTranscript) {
    cha::test::TestWorkspace fixture;
    cha::Workspace workspace(fixture.root());
    cha::test::NoopNotifier notifier;
    cha::ApplicationEnvironment first(workspace);
    cha::ApplicationEnvironment second(workspace);

    auto opened = first.open_welcome(notifier);
    (void)opened.controller->submit_prompt("builtin-guest", "Remember this");
    ASSERT_EQ(opened.controller->transcript().entries().size(), 1U);
    opened.controller.reset();

    auto reopened = first.open_welcome(notifier);
    EXPECT_EQ(reopened.controller->transcript().entries().size(), 1U);
    reopened.controller.reset();
    auto other = second.open_welcome(notifier);
    EXPECT_TRUE(other.controller->transcript().entries().empty());
}
