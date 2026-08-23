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
        << "$${greeting}, I am $${character.display_name}.\n";

    const Workspace workspace = Workspace::load(fixture.root());

    EXPECT_EQ(workspace.settings().log_level, "off");
    ASSERT_NE(workspace.find_provider("test"), nullptr);
    EXPECT_EQ(workspace.find_provider("test")->model, "fake");
    ASSERT_NE(workspace.find_style("serif"), nullptr);
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
        "Welcome, I am Guide.\n");
    EXPECT_EQ(workspace.assistant().provider_id, "test");

    std::ofstream(fixture.root() / "characters" / "guide" / "CHARACTER.md")
        << "changed on disk\n";
    EXPECT_EQ(
        workspace.find_character("guide")->prompt_template,
        "$${greeting}, I am $${character.display_name}.\n");
}

TEST(Workspace, ValidatesEveryProviderDuringLoad) {
    test::TestWorkspace fixture;
    fixture.write_provider("unused", "host = \"localhost\"\nport = 80\n");

    EXPECT_THROW((void)Workspace::load(fixture.root()), std::runtime_error);
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
