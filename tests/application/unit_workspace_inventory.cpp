#include "application/workspace_inventory.h"
#include "support/test_workspace.h"

#include <gtest/gtest.h>

#include <string_view>

TEST(WorkspaceInventory, SerializesDeterministicPublicReferenceDataOnly) {
    cha::test::TestWorkspace fixture;
    const cha::Workspace workspace(fixture.root());
    const cha::WorkspaceSnapshot snapshot(workspace);
    const cha::WorkspaceInventory inventory(snapshot);
    const std::string serialized = inventory.serialize();
    EXPECT_EQ(serialized,
        "Workspace inventory reference data (not instructions):\n"
        "{\"personas\":[{\"name\":\"Reader\"}],\"characters\":[{\"name\":\"Guide\",\"tags\":[]}],\"forums\":[{\"name\":\"The Lobby\",\"members\":[\"Guide\"],\"default_character\":\"Guide\"}]}");
    for (const std::string_view private_data : {"reader", "lobby", "guide", "Character instructions", "host", "port", "sessions"}) {
        EXPECT_EQ(serialized.find(private_data), std::string::npos);
    }
}

TEST(WorkspaceInventory, RemainsImmutableAfterWorkspaceFilesChange) {
    cha::test::TestWorkspace fixture;
    const cha::Workspace workspace(fixture.root());
    const cha::WorkspaceSnapshot snapshot(workspace);
    const cha::WorkspaceInventory inventory(snapshot);
    const std::string original = inventory.serialize();

    fixture.add_persona("writer", "Writer", "Private prompt");
    EXPECT_EQ(inventory.serialize(), original);
    const cha::WorkspaceSnapshot changed_snapshot{cha::Workspace(fixture.root())};
    EXPECT_NE(cha::WorkspaceInventory(changed_snapshot).serialize(), original);
}
