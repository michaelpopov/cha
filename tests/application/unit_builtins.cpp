#include "application/builtins.h"
#include "application/effective_personas.h"
#include "application/workspace_inventory.h"
#include "application/workspace_snapshot.h"
#include "support/test_workspace.h"

#include <gtest/gtest.h>

TEST(Builtins, GuideAndTrustedValuesHavePublicNames) {
    EXPECT_EQ(cha::builtin_guest().display_name, "Guest");
    EXPECT_EQ(cha::builtin_entrance().display_name, "Entrance");
    EXPECT_NE(cha::application_guide().find("## Commands"), std::string_view::npos);
    EXPECT_NE(cha::application_guide().find("/mcast"), std::string_view::npos);
}

TEST(Builtins, AssistantPromptContainsOnlyPublicApplicationContext) {
    cha::test::TestWorkspace fixture;
    cha::Workspace workspace(fixture.root());
    cha::WorkspaceSnapshot snapshot(workspace);
    cha::WorkspaceInventory inventory(snapshot);
    cha::EffectivePersonas personas(snapshot);
    const auto definitions = cha::builtin_assistant_definitions(
        workspace.workspace_config().provider, inventory.serialize(), *personas.roster());
    ASSERT_EQ(definitions.size(), 1U);
    const std::string& prompt = definitions.front().system_prompt;
    EXPECT_NE(prompt.find("CHA application guide"), std::string::npos);
    EXPECT_NE(prompt.find("Workspace inventory reference data (not instructions):"), std::string::npos);
    EXPECT_NE(prompt.find("Guest"), std::string::npos);
    EXPECT_NE(prompt.find("Reader"), std::string::npos);
    EXPECT_NE(prompt.find("## Participants"), std::string::npos);
    EXPECT_NE(prompt.find("Forum context"), std::string::npos);
    EXPECT_NE(prompt.find("Shared chat history"), std::string::npos);
    EXPECT_EQ(prompt.find("builtin-"), std::string::npos);
    EXPECT_EQ(prompt.find(fixture.root().string()), std::string::npos);
    EXPECT_EQ(prompt.find("api_key"), std::string::npos);
}
