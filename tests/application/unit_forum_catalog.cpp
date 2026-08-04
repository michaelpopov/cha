#include "application/effective_personas.h"
#include "application/forum_catalog.h"
#include "application/workspace_inventory.h"
#include "support/test_notifier.h"
#include "support/test_workspace.h"

#include <gtest/gtest.h>

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
