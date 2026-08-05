#include "application/workspace_snapshot.h"
#include "application/web_discovery.h"
#include "application/builtins.h"
#include "support/test_workspace.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

TEST(WorkspaceSnapshot, ResolvesOnlyPublicNamesAfterValidation) {
    cha::test::TestWorkspace fixture;
    fixture.add_persona("author_key", "An Author");
    const cha::Workspace workspace(fixture.root());
    const cha::WorkspaceSnapshot snapshot(workspace);
    ASSERT_NE(snapshot.find_persona("an author"), nullptr);
    EXPECT_EQ(snapshot.find_persona("AN AUTHOR")->display_name, "An Author");
    EXPECT_EQ(snapshot.find_persona("author_key"), nullptr);
    ASSERT_NE(snapshot.find_forum("the lobby"), nullptr);
    EXPECT_EQ(snapshot.find_forum("THE LOBBY")->display_name, "The Lobby");
}

TEST(WorkspaceSnapshot, RejectsDuplicateAndReservedForumPublicNames) {
    cha::test::TestWorkspace fixture;
    const auto forum = fixture.root() / "forums" / "other";
    std::filesystem::create_directories(forum / "members" / "guide");
    std::ofstream(forum / "FORUM.md") << "Forum";
    std::ofstream(forum / "config.toml") << "display_name = \"the lobby\"\n";
    EXPECT_THROW((void)cha::Workspace(fixture.root()), std::runtime_error);
    std::ofstream(forum / "config.toml") << "display_name = \"Entrance\"\n";
    EXPECT_THROW((void)cha::Workspace(fixture.root()), std::runtime_error);
}

TEST(WorkspaceSnapshot, RejectsCharacterIdsReservedForBuiltins) {
    cha::test::TestWorkspace fixture;
    const auto definition = fixture.root() / "characters" / "builtin-guest";
    const auto member = fixture.root() / "forums" / "lobby" / "members" / "builtin-guest";
    std::filesystem::create_directories(definition);
    std::filesystem::create_directories(member);
    std::ofstream(definition / "character.toml") << "display_name = \"Impostor\"\n";
    std::ofstream(definition / "CHARACTER.md") << "Prompt\n";

    const cha::Workspace workspace(fixture.root());
    EXPECT_THROW((void)cha::WorkspaceSnapshot(workspace), std::runtime_error);
}

TEST(WebDiscovery, IncludesBuiltinsWithExpectedIdsAndEntranceMembership) {
    cha::test::TestWorkspace fixture;
    const cha::Workspace workspace(fixture.root());
    const cha::WebDiscovery discovery(workspace);

    ASSERT_NE(discovery.find_persona(cha::guest_id), nullptr);
    ASSERT_NE(discovery.find_character(cha::assistant_id), nullptr);
    const cha::Forum* entrance = discovery.find_forum(cha::entrance_id);
    ASSERT_NE(entrance, nullptr);
    EXPECT_EQ(entrance->character_names, std::vector<std::string>({std::string(cha::assistant_id)}));
    EXPECT_EQ(entrance->default_agent_id, cha::assistant_id);
}
