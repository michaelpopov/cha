#include "util/private_filesystem.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace cha {
namespace {

std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

class PrivateFilesystemTest : public testing::Test {
protected:
    void SetUp() override {
        directory_ = std::filesystem::temp_directory_path()
            / ("cha_private_fs_"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory_);
    }

    void TearDown() override {
        std::filesystem::remove_all(directory_);
    }

    std::filesystem::path directory_;
};

#ifndef _WIN32
mode_t posix_mode(const std::filesystem::path& path) {
    struct stat info {};
    EXPECT_EQ(::lstat(path.c_str(), &info), 0);
    return info.st_mode & 0777;
}
#endif

bool try_create_symlink(
    const std::filesystem::path& target,
    const std::filesystem::path& link,
    bool directory) {
    std::error_code error;
    if (directory) {
        std::filesystem::create_directory_symlink(target, link, error);
    } else {
        std::filesystem::create_symlink(target, link, error);
    }
    return !error;
}

TEST_F(PrivateFilesystemTest, CreatesDirectoryWithOwnerOnlyMode) {
    const std::filesystem::path path = directory_ / "private";
    create_private_directory(path);
    EXPECT_TRUE(std::filesystem::is_directory(path));
#ifndef _WIN32
    EXPECT_EQ(posix_mode(path), static_cast<mode_t>(0700));
#endif
}

TEST_F(PrivateFilesystemTest, CreatesFileWithOwnerOnlyMode) {
    const std::filesystem::path path = directory_ / "secret.env";
    create_private_file(path, "TOKEN=1\n");
    EXPECT_EQ(file_bytes(path), "TOKEN=1\n");
#ifndef _WIN32
    EXPECT_EQ(posix_mode(path), static_cast<mode_t>(0600));
#endif
}

TEST_F(PrivateFilesystemTest, ReplacesAnExistingRegularFile) {
    const std::filesystem::path path = directory_ / "secret.env";
    std::ofstream(path) << "old";
#ifndef _WIN32
    ASSERT_EQ(::chmod(path.c_str(), 0644), 0);
    ASSERT_EQ(posix_mode(path), static_cast<mode_t>(0644));
#endif
    create_private_file(path, "new");
    EXPECT_EQ(file_bytes(path), "new");
#ifndef _WIN32
    EXPECT_EQ(posix_mode(path), static_cast<mode_t>(0600));
#endif
}

TEST_F(PrivateFilesystemTest, RejectsExistingDirectoryWhenCreatingDirectory) {
    const std::filesystem::path path = directory_ / "already";
    std::filesystem::create_directory(path);
    EXPECT_THROW(create_private_directory(path), std::runtime_error);
}

TEST_F(PrivateFilesystemTest, RejectsSymlinkWhenCreatingDirectory) {
    const std::filesystem::path target = directory_ / "target";
    std::filesystem::create_directory(target);
#ifndef _WIN32
    const mode_t before = posix_mode(target);
#endif
    const std::filesystem::path link = directory_ / "link";
    if (!try_create_symlink(target, link, true)) {
        GTEST_SKIP() << "symbolic links are not available";
    }
    EXPECT_THROW(create_private_directory(link), std::runtime_error);
    EXPECT_TRUE(std::filesystem::is_symlink(link));
#ifndef _WIN32
    EXPECT_EQ(posix_mode(target), before);
#endif
}

TEST_F(PrivateFilesystemTest, RejectsSymlinkWhenCreatingFile) {
    const std::filesystem::path target = directory_ / "target";
    std::ofstream(target) << "keep";
#ifndef _WIN32
    ASSERT_EQ(::chmod(target.c_str(), 0644), 0);
#endif
    const std::filesystem::path link = directory_ / "link";
    if (!try_create_symlink(target, link, false)) {
        GTEST_SKIP() << "symbolic links are not available";
    }
    EXPECT_THROW(create_private_file(link, "new"), std::runtime_error);
    EXPECT_EQ(file_bytes(target), "keep");
#ifndef _WIN32
    EXPECT_EQ(posix_mode(target), static_cast<mode_t>(0644));
#endif
}

TEST_F(PrivateFilesystemTest, RejectsDirectoryWhenCreatingFile) {
    const std::filesystem::path path = directory_ / "dir";
    std::filesystem::create_directory(path);
    EXPECT_THROW(create_private_file(path, "x"), std::runtime_error);
}

TEST_F(PrivateFilesystemTest, TightensAnExistingRegularFile) {
    const std::filesystem::path path = directory_ / "db";
    std::ofstream(path) << "sqlite";
#ifndef _WIN32
    ASSERT_EQ(::chmod(path.c_str(), 0644), 0);
    ASSERT_EQ(posix_mode(path), static_cast<mode_t>(0644));
#endif
    tighten_private_file(path);
    EXPECT_EQ(file_bytes(path), "sqlite");
#ifndef _WIN32
    EXPECT_EQ(posix_mode(path), static_cast<mode_t>(0600));
#endif
}

TEST_F(PrivateFilesystemTest, RejectsSymlinkWhenTightening) {
    const std::filesystem::path target = directory_ / "target";
    std::ofstream(target) << "keep";
#ifndef _WIN32
    ASSERT_EQ(::chmod(target.c_str(), 0644), 0);
#endif
    const std::filesystem::path link = directory_ / "link";
    if (!try_create_symlink(target, link, false)) {
        GTEST_SKIP() << "symbolic links are not available";
    }
    EXPECT_THROW(tighten_private_file(link), std::runtime_error);
    EXPECT_EQ(file_bytes(target), "keep");
#ifndef _WIN32
    EXPECT_EQ(posix_mode(target), static_cast<mode_t>(0644));
#endif
}

TEST_F(PrivateFilesystemTest, RequireHelpersRejectSymlinksAndWrongTypes) {
    const std::filesystem::path file = directory_ / "file";
    const std::filesystem::path dir = directory_ / "dir";
    std::ofstream(file) << "data";
    std::filesystem::create_directory(dir);
    EXPECT_NO_THROW(require_regular_file(file));
    EXPECT_NO_THROW(require_directory(dir));
    EXPECT_THROW(require_regular_file(dir), std::runtime_error);
    EXPECT_THROW(require_directory(file), std::runtime_error);

    const std::filesystem::path file_link = directory_ / "file-link";
    const std::filesystem::path dir_link = directory_ / "dir-link";
    if (!try_create_symlink(file, file_link, false)
        || !try_create_symlink(dir, dir_link, true)) {
        GTEST_SKIP() << "symbolic links are not available";
    }
    EXPECT_THROW(require_regular_file(file_link), std::runtime_error);
    EXPECT_THROW(require_directory(dir_link), std::runtime_error);
}

TEST_F(PrivateFilesystemTest, NewSqliteFilesUseOwnerOnlyPermissions) {
    const std::filesystem::path path = directory_ / "private.sqlite3";
    sqlite3* handle = nullptr;
    ASSERT_EQ(
        sqlite3_open_v2(
            path.c_str(),
            &handle,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
            nullptr),
        SQLITE_OK);
    sqlite3_close(handle);
    EXPECT_TRUE(std::filesystem::is_regular_file(path));
#ifndef _WIN32
    struct stat info {};
    ASSERT_EQ(::stat(path.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, static_cast<mode_t>(0600));
#endif
}

} // namespace
} // namespace cha
