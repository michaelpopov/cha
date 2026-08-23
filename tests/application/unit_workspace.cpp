#include "workspace/workspace.h"

#include "support/test_workspace.h"

#include <gtest/gtest.h>

#include <fstream>

namespace cha {
namespace {

TEST(Workspace, EagerlyLoadsOwnedResolvedData) {
    test::TestWorkspace fixture;
    fixture.write_style("serif", "font = \"serif\"\nweight = \"bold\"\n");
    fixture.write_character_config(
        "display_name = \"Guide\"\n"
        "description = \"A helpful guide.\"\n"
        "provider = \"test\"\n"
        "style = \"serif\"\n"
        "tags = [\"help\"]\n"
        "[prompt]\n"
        "greeting = \"Hello\"\n");
    std::ofstream(
        fixture.root() / "forums" / "lobby" / "members"
            / "character_defaults.toml")
        << "[prompt]\ngreeting = \"Welcome\"\n";
    std::ofstream(fixture.root() / "characters" / "guide" / "CHARACTER.md")
        << "$${greeting}, I am $${character.display_name} in "
           "$${forum.display_name}.\n"
           "<character_profile>\nIntrinsic Guide.\n</character_profile>\n";

    const Workspace workspace = Workspace::load(fixture.root());

    EXPECT_EQ(workspace.settings().log_level, "off");
    ASSERT_NE(workspace.find_provider("test"), nullptr);
    EXPECT_EQ(workspace.find_provider("test")->model, "fake");
    EXPECT_EQ(workspace.find_provider("test")->label, "Test");
    ASSERT_NE(workspace.find_style("serif"), nullptr);
    EXPECT_EQ(workspace.find_style("serif")->label, "Serif");
    EXPECT_EQ(
        workspace.find_style("serif")->appearance.font,
        WorkspaceFont::serif);
    ASSERT_NE(workspace.find_persona("reader"), nullptr);
    ASSERT_NE(workspace.find_character("guide"), nullptr);
    EXPECT_EQ(workspace.find_character("guide")->provider_id, "test");
    EXPECT_EQ(
        workspace.find_character("guide")->appearance.weight,
        WorkspaceWeight::bold);
    ASSERT_NE(workspace.find_forum("lobby"), nullptr);
    ASSERT_EQ(workspace.find_forum("lobby")->members.size(), 1U);
    EXPECT_EQ(
        workspace.find_forum("lobby")->members.front().character_prompt,
        "Welcome, I am Guide in The Lobby.\n"
        "<character_profile>\nIntrinsic Guide.\n</character_profile>\n");
    EXPECT_EQ(workspace.find_character("guide")->markdown, "Intrinsic Guide.");
    ASSERT_NE(workspace.find_persona(workspace_guest_id), nullptr);
    EXPECT_EQ(
        workspace.find_persona(workspace_guest_id)->display_name,
        "Guest");
    ASSERT_NE(workspace.find_character(workspace_assistant_id), nullptr);
    EXPECT_EQ(
        workspace.find_character(workspace_assistant_id)->provider_id,
        "test");
    EXPECT_FALSE(
        workspace.find_character(workspace_assistant_id)->markdown.empty());
    ASSERT_NE(workspace.find_forum(workspace_entrance_id), nullptr);
    EXPECT_EQ(
        workspace.find_forum(workspace_entrance_id)->default_character_id,
        workspace_assistant_id);
    ASSERT_EQ(
        workspace.find_forum(workspace_entrance_id)->members.size(), 1U);
    EXPECT_FALSE(
        workspace.find_forum(workspace_entrance_id)
            ->members.front().system_prompt.empty());

    std::ofstream(fixture.root() / "characters" / "guide" / "CHARACTER.md")
        << "changed on disk\n";
    EXPECT_EQ(
        workspace.find_character("guide")->prompt_template,
        "$${greeting}, I am $${character.display_name} in "
        "$${forum.display_name}.\n"
        "<character_profile>\nIntrinsic Guide.\n</character_profile>\n");
}

TEST(Workspace, OmitsAnInvalidUnusedProvider) {
    test::TestWorkspace fixture;
    fixture.write_provider("unused", "host = \"localhost\"\nport = 80\n");

    const Workspace workspace = Workspace::load(fixture.root());
    EXPECT_EQ(workspace.find_provider("unused"), nullptr);
}

TEST(Workspace, WritesConfigurationWithoutChangingTheLoadedInstance) {
    test::TestWorkspace fixture;
    fixture.write_provider(
        "second",
        "host = \"test\"\nport = 2\nmode = \"test\"\nmodel = \"second\"\n");
    fixture.write_style("mono", "font = \"mono\"\n");
    fixture.add_character("writer", "Writer");
    std::filesystem::create_directories(
        fixture.root() / "forums" / "lobby" / "members" / "writer");

    const Workspace workspace = Workspace::load(fixture.root());
    workspace.write_character_settings(
        "guide", "second", std::string_view{"mono"});
    workspace.write_forum_default_character("lobby", "writer");
    workspace.write_forum_default_persona("lobby", "reader");

    EXPECT_EQ(workspace.find_character("guide")->provider_id, "test");
    EXPECT_EQ(
        workspace.find_forum("lobby")->default_character_id,
        "guide");
    EXPECT_EQ(
        workspace.find_forum("lobby")->default_persona_id,
        workspace_guest_id);

    const Workspace reloaded = Workspace::load(fixture.root());
    EXPECT_EQ(reloaded.find_character("guide")->provider_id, "second");
    EXPECT_EQ(reloaded.find_character("guide")->style_id, "mono");
    EXPECT_EQ(
        reloaded.find_forum("lobby")->default_character_id,
        "writer");
    EXPECT_EQ(
        reloaded.find_forum("lobby")->default_persona_id,
        "reader");
}

TEST(Workspace, LoadwsPublishesOnlyACompleteWorkspace) {
    test::TestWorkspace valid;
    loadws(valid.root());
    const std::shared_ptr<const Workspace> published = getws();
    ASSERT_NE(published, nullptr);
    EXPECT_EQ(published->root(), valid.root());

    test::TestWorkspace invalid;
    invalid.write_character_config("display_name =\n");
    EXPECT_THROW(loadws(invalid.root()), std::runtime_error);
    EXPECT_EQ(getws(), published);
}

} // namespace
} // namespace cha
