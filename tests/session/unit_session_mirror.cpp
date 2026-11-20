#include "session/session_mirror.h"

#include "storage/session_repository.h"
#include "support/test_web_graph.h"
#include "support/test_workspace.h"
#include "workspace/workspace.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace cha {
namespace {

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

TEST(SessionMirror, SanitizesUnsafeDisplayNameCharacters) {
    EXPECT_EQ(
        mirror_path_name("one*\"\\/<>:|?#^[]two. "),
        "one-------------two");
    EXPECT_EQ(mirror_path_name("..."), "session");
}

TEST(SessionMirror, WritesActiveSessionsUnderForumDisplayNameAndNumbersDuplicates) {
    test::TestWorkspace workspace;
    test::WebGraph graph(workspace.root());
    const StoredSession first = graph.sessions()->create("lobby", "xyz");
    const StoredSession second = graph.sessions()->create("lobby", "xyz");
    const std::filesystem::path root = workspace.root() / "mirror";
    ASSERT_FALSE(std::filesystem::exists(root));

    SessionMirror mirror(root, *graph.sessions());
    EXPECT_TRUE(std::filesystem::is_directory(root));

    const std::filesystem::path forum = root / "The Lobby";
    EXPECT_TRUE(std::filesystem::is_directory(forum));
    EXPECT_EQ(read_file(forum / "xyz.md"), "<!-- CHA session: xyz -->\n");
    EXPECT_EQ(read_file(forum / "xyz (1).md"), "<!-- CHA session: xyz -->\n");
    EXPECT_FALSE(std::filesystem::exists(root / "Entrance"));

    mirror.update(
        {std::string(entrance_id), std::string(welcome_id)},
        "Welcome",
        {});
    EXPECT_FALSE(std::filesystem::exists(root / "Entrance"));

    graph.sessions()->delete_session(second.identity);
    EXPECT_TRUE(std::filesystem::exists(forum / "xyz (1).md"));

    mirror.update(first.identity, "renamed/name", {});
    EXPECT_FALSE(std::filesystem::exists(forum / "xyz.md"));
    EXPECT_EQ(
        read_file(forum / "renamed-name.md"),
        "<!-- CHA session: renamed/name -->\n");
}

TEST(SessionMirror, ForumRefreshPreservesSurvivorsAndArchivedPaths) {
    test::TestWorkspace fixture;
    fixture.add_forum("forum_1", "First", "guide");
    test::WebGraph graph(fixture.root());
    const auto root = fixture.root() / "mirror";
    SessionMirror mirror(root, *graph.sessions());
    const FullSessionId reused{"forum_1", "chat"};
    mirror.update(reused, "Old label", {});
    const auto archive = root / "First" / "Old label.md";
    ASSERT_EQ(read_file(archive), "<!-- CHA session: Old label -->\n");
    std::filesystem::remove_all(fixture.root() / "forums" / "forum_1");
    fixture.add_forum("lobby", "Renamed lobby", "guide");
    mirror.reconcile_forums(Workspace::load(fixture.root()));
    fixture.add_forum("forum_1", "Different name", "guide");
    const auto existing = root / "Different name";
    std::filesystem::create_directory(existing);
    std::ofstream(existing / "New label.md") << "Archived transcript";
    mirror.reconcile_forums(Workspace::load(fixture.root()));

    mirror.update(reused, "New label", {});
    mirror.update({"lobby", "survivor"}, "Still here", {});
    EXPECT_EQ(read_file(archive), "<!-- CHA session: Old label -->\n");
    EXPECT_EQ(read_file(existing / "New label.md"), "Archived transcript");
    EXPECT_EQ(read_file(root / "Different name (1)" / "New label.md"), "<!-- CHA session: New label -->\n");
    EXPECT_EQ(read_file(root / "The Lobby" / "Still here.md"), "<!-- CHA session: Still here -->\n");
}

} // namespace
} // namespace cha
