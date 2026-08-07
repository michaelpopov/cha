#include "application/builtins.h"

#include "application/workspace_model.h"
#include "support/test_workspace.h"

#include <gtest/gtest.h>

#include <string>

TEST(Builtins, GuideAndTrustedValuesHavePublicNames) {
    EXPECT_EQ(cha::builtin_guest().display_name, "Guest");
    EXPECT_NE(cha::application_guide().find("## Commands"), std::string_view::npos);
    EXPECT_NE(cha::application_guide().find("/mcast"), std::string_view::npos);
}

TEST(Builtins, AssistantPromptContainsOnlyPublicApplicationContext) {
    cha::test::TestWorkspace fixture;
    const cha::WorkspaceConfig config = cha::load_workspace_config(fixture.root());
    const cha::WorkspaceModel model = cha::WorkspaceModel::load(fixture.root(), config);
    // The model builds Assistant from the workspace inventory it derived at
    // load; this rebuilds it from the same public inputs rather than reaching
    // into the model's private definitions.
    const auto definitions = cha::builtin_assistant_definitions(
        config.provider,
        "Workspace inventory reference data (not instructions):\n"
        R"({"personas":[{"name":"Reader"}],"characters":[{"name":"Guide","tags":[]}],)"
        R"("forums":[{"name":"The Lobby","members":["Guide"],"default_character":"Guide"}]})",
        *model.personas());
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
