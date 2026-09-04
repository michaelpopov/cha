#include "web/session_mirror.h"

#include "session/session_repository.h"
#include "support/test_web_graph.h"
#include "support/test_workspace.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace cha::web {
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

TEST(SessionMirror, RequiresAnExistingRoot) {
    test::TestWorkspace workspace;
    test::WebGraph graph(workspace.root());
    const std::filesystem::path missing = workspace.root() / "missing-mirror";

    EXPECT_THROW(SessionMirror(missing, *graph.sessions()), std::runtime_error);
    EXPECT_FALSE(std::filesystem::exists(missing));
}

TEST(SessionMirror, WritesActiveSessionsUnderForumDisplayNameAndNumbersDuplicates) {
    test::TestWorkspace workspace;
    test::WebGraph graph(workspace.root());
    const StoredSession first = graph.sessions()->create("lobby", "xyz");
    const StoredSession second = graph.sessions()->create("lobby", "xyz");
    const std::filesystem::path root = workspace.root() / "mirror";
    std::filesystem::create_directory(root);

    SessionMirror mirror(root, *graph.sessions());

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

} // namespace
} // namespace cha::web
