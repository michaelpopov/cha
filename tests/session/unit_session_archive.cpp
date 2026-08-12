#include "session/session_archive.h"

#include "session/session_delete_conflict.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#ifndef _WIN32
#include <unistd.h>
#endif

namespace cha {
namespace {

std::string contents_of(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

// The link fallback is production's only archive path on mounts that reject a
// no-replace rename, such as the 9p mounts WSL uses for Windows drives. A test
// host almost never provides one, so these exercise the fallback directly
// instead of waiting for a filesystem that selects it.
class SessionArchiveTest : public testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path()
            / ("cha_archive_"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(active());
        std::filesystem::create_directories(deleted());

        // Hard links are what the fallback is built on, so a filesystem
        // without them cannot run these at all.
        write(active() / "probe", "probe");
        std::error_code error;
        std::filesystem::create_hard_link(
            active() / "probe", active() / "probe-link", error);
        if (error) GTEST_SKIP() << "hard links are unavailable: " << error.message();
        std::filesystem::remove(active() / "probe-link");
        std::filesystem::remove(active() / "probe");
    }

    void TearDown() override {
        std::error_code error;
        // A permission test may leave a directory that cannot be emptied.
        std::filesystem::permissions(
            active(),
            std::filesystem::perms::owner_all,
            std::filesystem::perm_options::replace,
            error);
        std::filesystem::remove_all(root_, error);
    }

    std::filesystem::path active() const { return root_ / "sessions"; }
    std::filesystem::path deleted() const { return root_ / "sessions" / "deleted"; }

    static void write(const std::filesystem::path& path, std::string_view text) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << text;
    }

private:
    std::filesystem::path root_;
};

TEST_F(SessionArchiveTest, MovesTheDatabaseToItsDeletedDestination) {
    const std::filesystem::path source = active() / "session.sqlite3";
    const std::filesystem::path destination = deleted() / "session.sqlite3";
    write(source, "database bytes");

    archive_without_replacement(source, destination);

    EXPECT_FALSE(std::filesystem::exists(source));
    EXPECT_EQ(contents_of(destination), "database bytes");
}

TEST_F(SessionArchiveTest, RefusesToReplaceAnExistingDestination) {
    const std::filesystem::path source = active() / "session.sqlite3";
    const std::filesystem::path destination = deleted() / "session.sqlite3";
    write(source, "active bytes");
    write(destination, "retained bytes");

    EXPECT_THROW(
        archive_without_replacement(source, destination),
        SessionDeleteConflictError);

    EXPECT_EQ(contents_of(source), "active bytes");
    EXPECT_EQ(contents_of(destination), "retained bytes");
}

TEST_F(SessionArchiveTest, LinkFallbackMovesTheDatabaseToItsDestination) {
    const std::filesystem::path source = active() / "session.sqlite3";
    const std::filesystem::path destination = deleted() / "session.sqlite3";
    write(source, "database bytes");

    archive_by_link_without_replacement(source, destination);

    EXPECT_FALSE(std::filesystem::exists(source));
    EXPECT_EQ(contents_of(destination), "database bytes");
    EXPECT_EQ(std::filesystem::hard_link_count(destination), 1U);
}

TEST_F(SessionArchiveTest, LinkFallbackRefusesToReplaceAnExistingDestination) {
    const std::filesystem::path source = active() / "session.sqlite3";
    const std::filesystem::path destination = deleted() / "session.sqlite3";
    write(source, "active bytes");
    write(destination, "retained bytes");

    EXPECT_THROW(
        archive_by_link_without_replacement(source, destination),
        SessionDeleteConflictError);

    EXPECT_EQ(contents_of(source), "active bytes");
    EXPECT_EQ(contents_of(destination), "retained bytes");
}

TEST_F(SessionArchiveTest, LinkFallbackReportsAMissingSourceAsAFailureNotAConflict) {
    const std::filesystem::path source = active() / "absent.sqlite3";
    const std::filesystem::path destination = deleted() / "absent.sqlite3";

    EXPECT_THROW(
        archive_by_link_without_replacement(source, destination),
        std::filesystem::filesystem_error);

    EXPECT_FALSE(std::filesystem::exists(destination));
}

#ifndef _WIN32
TEST_F(SessionArchiveTest, LinkFallbackRollsBackWhenTheSourceCannotBeRemoved) {
    if (::geteuid() == 0) {
        GTEST_SKIP() << "root bypasses the directory permission this test needs";
    }
    const std::filesystem::path source = active() / "session.sqlite3";
    const std::filesystem::path destination = deleted() / "session.sqlite3";
    write(source, "database bytes");

    // Unlinking a name needs write permission on its directory, so a read-only
    // source directory fails the second half of the fallback after its link
    // has already been published.
    std::filesystem::permissions(
        active(),
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec,
        std::filesystem::perm_options::replace);

    EXPECT_THROW(
        archive_by_link_without_replacement(source, destination),
        std::filesystem::filesystem_error);

    std::filesystem::permissions(
        active(),
        std::filesystem::perms::owner_all,
        std::filesystem::perm_options::replace);

    // The session stays where the catalog can still see it, and the published
    // link is rolled back so a later retry does not meet its own leftovers as
    // a conflict.
    EXPECT_EQ(contents_of(source), "database bytes");
    EXPECT_FALSE(std::filesystem::exists(destination));
}
#endif

} // namespace
} // namespace cha
