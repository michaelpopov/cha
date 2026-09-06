#include "web/session_mirror.h"

#include "session/session_repository.h"
#include "support/test_web_graph.h"
#include "support/test_workspace.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
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

TEST(SessionMirror, InactiveAddAndUpdateAreNoOps) {
    SessionMirror mirror;
    mirror.add({.identity = {"lobby", "s1"}, .label = "One", .updated_at = 1});
    mirror.update({"lobby", "s1"}, "One", {});
}

TEST(SessionMirror, RetargetRebuildsMapsAndCanBecomeInactive) {
    test::TestWorkspace workspace;
    test::WebGraph graph(workspace.root());
    const StoredSession stored = graph.sessions()->create("lobby", "alpha");
    const std::filesystem::path first = workspace.root() / "mirror-a";
    const std::filesystem::path second = workspace.root() / "mirror-b";
    std::filesystem::create_directory(first);
    std::filesystem::create_directory(second);

    SessionMirror mirror(first, *graph.sessions());
    EXPECT_TRUE(std::filesystem::exists(first / "The Lobby" / "alpha.md"));

    mirror.retarget(second, mirror_rebuild_input(*graph.sessions()));
    EXPECT_TRUE(std::filesystem::exists(second / "The Lobby" / "alpha.md"));

    mirror.retarget(std::nullopt, {});
    mirror.update(stored.identity, "alpha", {});
    EXPECT_TRUE(std::filesystem::exists(second / "The Lobby" / "alpha.md"));
}

TEST(SessionMirror, RetargetFailureLeavesTheMirrorInactive) {
    test::TestWorkspace workspace;
    test::WebGraph graph(workspace.root());
    (void)graph.sessions()->create("lobby", "alpha");
    const std::filesystem::path first = workspace.root() / "mirror-ok";
    std::filesystem::create_directory(first);
    SessionMirror mirror(first, *graph.sessions());

    const std::filesystem::path missing = workspace.root() / "missing-mirror";
    EXPECT_THROW(
        mirror.retarget(missing, mirror_rebuild_input(*graph.sessions())),
        std::runtime_error);
    mirror.add({.identity = {"lobby", "later"}, .label = "Later", .updated_at = 1});
    EXPECT_FALSE(std::filesystem::exists(missing));
}

TEST(SessionMirror, RebuildsFromAMaintenanceGuardWithoutRelocking) {
    test::TestWorkspace workspace;
    test::WebGraph graph(workspace.root());
    (void)graph.sessions()->create("lobby", "guarded");
    const std::filesystem::path root = workspace.root() / "mirror-guard";
    std::filesystem::create_directory(root);

    SessionMirror mirror;
    {
        const SessionRepository::MaintenanceGuard maintenance =
            graph.sessions()->reserve_maintenance();
        mirror.retarget(root, mirror_rebuild_input(maintenance));
    }
    EXPECT_TRUE(std::filesystem::exists(root / "The Lobby" / "guarded.md"));
}

} // namespace
} // namespace cha::web
