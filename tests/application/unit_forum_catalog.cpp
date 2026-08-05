#include "application/effective_personas.h"
#include "application/forum_catalog.h"
#include "application/workspace_inventory.h"
#include "support/test_notifier.h"
#include "support/test_workspace.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

TEST(ForumCatalog, ResolvesEntranceAndListsOnlyCustomForums) {
    cha::test::TestWorkspace fixture;
    cha::Workspace workspace(fixture.root());
    cha::WorkspaceSnapshot snapshot(workspace);
    cha::EffectivePersonas personas(snapshot);
    cha::WorkspaceInventory inventory(snapshot);
    cha::ForumCatalog catalog(workspace, snapshot, personas.roster(), inventory);
    ASSERT_NE(catalog.find("entrance"), nullptr);
    EXPECT_EQ(catalog.find("entrance")->display_name, "Entrance");
    EXPECT_EQ(catalog.custom_names(), std::vector<std::string>({"The Lobby"}));
    EXPECT_TRUE(catalog.entrance_source().list().empty());
}

TEST(ForumCatalog, SourcesOpenAndCreatePersistentSessionsWithTheSharedRoster) {
    cha::test::TestWorkspace fixture;
    cha::Workspace workspace(fixture.root());
    cha::WorkspaceSnapshot snapshot(workspace);
    cha::EffectivePersonas personas(snapshot);
    cha::WorkspaceInventory inventory(snapshot);
    cha::ForumCatalog catalog(workspace, snapshot, personas.roster(), inventory);
    cha::test::NoopNotifier notifier;

    auto entrance = catalog.source_for("Entrance").create("Notes", notifier);
    EXPECT_EQ(entrance.controller->persona_roster().get(), personas.roster().get());
    entrance.controller.reset();
    EXPECT_EQ(catalog.source_for("Entrance").list().front().label, "Notes");
    auto reopened_entrance = catalog.source_for("entrance").open("notes", notifier);
    EXPECT_EQ(reopened_entrance.controller->persona_roster().get(), personas.roster().get());
    reopened_entrance.controller.reset();

    auto workspace_session = catalog.source_for("The Lobby").create("Discussion", notifier);
    EXPECT_EQ(workspace_session.controller->persona_roster().get(), personas.roster().get());
    workspace_session.controller.reset();
    auto reopened_workspace = catalog.source_for("the lobby").open("discussion", notifier);
    EXPECT_EQ(reopened_workspace.controller->persona_roster().get(), personas.roster().get());
}

TEST(ForumCatalog, EntranceSourceRoutesWelcomeToItsEphemeralSource) {
    cha::test::TestWorkspace fixture;
    cha::Workspace workspace(fixture.root());
    cha::WorkspaceSnapshot snapshot(workspace);
    cha::EffectivePersonas personas(snapshot);
    cha::WorkspaceInventory inventory(snapshot);
    cha::ForumCatalog catalog(
        workspace, snapshot, personas.roster(), inventory);
    cha::test::NoopNotifier notifier;

    auto welcome = catalog.source_for("entrance").open("welcome", notifier);

    EXPECT_EQ(welcome.descriptor.forum_display_name, "Entrance");
    EXPECT_EQ(welcome.descriptor.session_label, "Welcome");
    EXPECT_EQ(
        welcome.controller->persona_roster().get(), personas.roster().get());
}

TEST(ForumCatalog, ListsMemberPublicNamesSortedByDisplayName) {
    cha::test::TestWorkspace fixture;
    // A second member whose storage key sorts before "guide" while its public
    // name sorts after "Guide", so key order cannot pass for display order.
    const auto definition = fixture.root() / "characters" / "adviser";
    std::filesystem::create_directories(definition);
    std::filesystem::create_directories(
        fixture.root() / "forums" / "lobby" / "members" / "adviser");
    std::ofstream(definition / "character.toml") << "display_name = \"Zeno\"\n";
    std::ofstream(definition / "CHARACTER.md") << "Character instructions\n";

    cha::Workspace workspace(fixture.root());
    cha::WorkspaceSnapshot snapshot(workspace);
    cha::EffectivePersonas personas(snapshot);
    cha::WorkspaceInventory inventory(snapshot);
    cha::ForumCatalog catalog(workspace, snapshot, personas.roster(), inventory);

    EXPECT_EQ(catalog.member_names("the lobby"),
        std::vector<std::string>({"Guide", "Zeno"}));
    EXPECT_EQ(catalog.member_names("entrance"),
        std::vector<std::string>({"Assistant"}));
}

TEST(ForumCatalog, MemberListingRejectsAnUnknownForumByPublicName) {
    cha::test::TestWorkspace fixture;
    cha::Workspace workspace(fixture.root());
    cha::WorkspaceSnapshot snapshot(workspace);
    cha::EffectivePersonas personas(snapshot);
    cha::WorkspaceInventory inventory(snapshot);
    cha::ForumCatalog catalog(workspace, snapshot, personas.roster(), inventory);
    try {
        (void)catalog.member_names("lobby");
        FAIL() << "Expected an unknown-forum error";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("lobby"), std::string::npos);
    }
}

TEST(ForumCatalog, EntranceErrorsUseOnlyPublicNames) {
    cha::test::TestWorkspace fixture;
    cha::Workspace workspace(fixture.root());
    cha::WorkspaceSnapshot snapshot(workspace);
    cha::EffectivePersonas personas(snapshot);
    cha::WorkspaceInventory inventory(snapshot);
    cha::ForumCatalog catalog(workspace, snapshot, personas.roster(), inventory);
    cha::test::NoopNotifier notifier;
    try {
        (void)catalog.source_for("Entrance").open("Missing", notifier);
        FAIL() << "Expected a missing-session error";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("Entrance"), std::string::npos);
        EXPECT_EQ(std::string(error.what()).find("builtin-entrance"), std::string::npos);
    }
}
