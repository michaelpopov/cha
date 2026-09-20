#include "workspace/workspace.h"

#include "support/test_workspace.h"

#include <gtest/gtest.h>

namespace cha {
namespace {

TEST(Builtins, WorkspacePublishesGuestAssistantAndEntrance) {
    test::TestWorkspace fixture;
    const Workspace workspace = Workspace::load(fixture.root());

    const WorkspacePersona* const guest =
        workspace.find_persona(workspace_guest_id);
    ASSERT_NE(guest, nullptr);
    EXPECT_EQ(guest->display_name, "Guest");

    const WorkspaceCharacter* const assistant =
        workspace.find_character(workspace_assistant_id);
    ASSERT_NE(assistant, nullptr);
    EXPECT_EQ(assistant->character.display_name, "Assistant");
    EXPECT_NE(assistant->markdown.find("## Commands"), std::string::npos);
    EXPECT_NE(assistant->markdown.find("/mcast"), std::string::npos);

    const WorkspaceForum* const entrance =
        workspace.find_forum(workspace_entrance_id);
    ASSERT_NE(entrance, nullptr);
    EXPECT_EQ(entrance->display_name, "Entrance");
    EXPECT_EQ(entrance->default_character_id, workspace_assistant_id);
    EXPECT_EQ(entrance->default_persona_id, workspace_guest_id);
    ASSERT_EQ(entrance->members.size(), 1U);
    EXPECT_EQ(entrance->members.front().character_id, workspace_assistant_id);
    EXPECT_NE(
        entrance->members.front().system_prompt.find(
            "Workspace inventory reference data"),
        std::string::npos);
}

} // namespace
} // namespace cha
