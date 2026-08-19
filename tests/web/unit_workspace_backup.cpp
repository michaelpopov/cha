#include "web/workspace_backup.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>

namespace cha::web {
namespace {

class WorkspaceBackupTest : public testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path()
            / ("cha_workspace_backup_"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()));
        workspace_ = root_ / "workspace";
        std::filesystem::create_directories(workspace_);
        std::ofstream(workspace_ / "workspace.toml") << "# test workspace\n";
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    std::filesystem::path root_;
    std::filesystem::path workspace_;
};

TEST_F(WorkspaceBackupTest, CreatesATimestampedTarArchive) {
    const std::filesystem::path archive = backup_workspace(
        workspace_, root_ / "backups");

    EXPECT_TRUE(std::filesystem::is_regular_file(archive));
    EXPECT_EQ(archive.parent_path(), root_ / "backups");
    EXPECT_TRUE(std::regex_match(
        archive.filename().string(),
        std::regex(R"(chaweb-\d{4}-\d{2}-\d{2}-\d{2}-\d{2}\.tar\.gz)")));
}

} // namespace
} // namespace cha::web
