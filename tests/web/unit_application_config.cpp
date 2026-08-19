#include "web/application_config.h"

#include "util/path_name.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace cha::web {
namespace {

class ApplicationConfigTest : public testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path()
            / ("cha_application_config_"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()));
        workspace_ = root_ / "customer-data";
        std::filesystem::create_directories(workspace_);
        std::ofstream(workspace_ / "workspace.toml") << "# test workspace\n";
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    ApplicationConfig load(std::vector<std::string> arguments) {
        std::vector<const char*> pointers;
        pointers.reserve(arguments.size());
        for (const std::string& argument : arguments) {
            pointers.push_back(argument.c_str());
        }
        return load_application_config(
            static_cast<int>(pointers.size()), pointers.data());
    }

    std::filesystem::path root_;
    std::filesystem::path workspace_;
};

TEST_F(ApplicationConfigTest, ReadsSettingsRelativeToTheApplicationRoot) {
    std::ofstream(root_ / "app.toml")
        << "host = \"127.0.0.1\"\n"
           "port = 8080\n"
           "workspace = \"customer-data\"\n"
           "backup_dir = \"backups\"\n";

    const ApplicationConfig config = load({"chaweb", "--root", root_.string()});

    EXPECT_EQ(config.root, std::filesystem::absolute(root_));
    EXPECT_EQ(config.config_file, std::filesystem::absolute(root_ / "app.toml"));
    EXPECT_EQ(config.host, "127.0.0.1");
    EXPECT_EQ(config.port, 8080);
    EXPECT_EQ(config.workspace, std::filesystem::absolute(workspace_));
    EXPECT_EQ(config.backup_dir, std::filesystem::absolute(root_ / "backups"));
}

TEST_F(ApplicationConfigTest, CommandLineSettingsOverrideTheFile) {
    const std::filesystem::path other_workspace = root_ / "other";
    std::filesystem::create_directories(other_workspace);
    std::ofstream(other_workspace / "workspace.toml") << "# other\n";
    std::ofstream(root_ / "app.toml")
        << "host = \"0.0.0.0\"\n"
           "port = 8000\n"
           "workspace = \"customer-data\"\n";

    const ApplicationConfig config = load({
        "chaweb", "--root", root_.string(),
        "--host", "127.0.0.1", "--port", "9000",
        "--workspace", other_workspace.string()});

    EXPECT_EQ(config.host, "127.0.0.1");
    EXPECT_EQ(config.port, 9000);
    EXPECT_EQ(config.workspace, std::filesystem::absolute(other_workspace));
}

TEST_F(ApplicationConfigTest, CompleteCommandLineDoesNotRequireDefaultConfig) {
    const ApplicationConfig config = load({
        "chaweb", "--root", root_.string(), "--host", "127.0.0.1",
        "--port", "8080", "--workspace", workspace_.string(),
        "--test-idle-grace-ms", "25"});
    EXPECT_EQ(config.test_idle_grace_ms, 25);
    EXPECT_THROW(
        (void)load({
            "chaweb", "--root", root_.string(), "--host", "127.0.0.1",
            "--port", "8080", "--workspace", workspace_.string(),
            "--test-idle-grace-ms", "0"}),
        std::runtime_error);
}

TEST_F(ApplicationConfigTest, MissingAndInvalidInputsNameTheRemedy) {
    try {
        (void)load({
            "chaweb", "--root", root_.string(), "--port", "8080",
            "--workspace", workspace_.string()});
        FAIL() << "expected missing host to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("host"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("--host"), std::string::npos);
    }

    EXPECT_THROW(
        (void)load({"chaweb", "--config", (root_ / "missing.toml").string()}),
        std::runtime_error);
    EXPECT_THROW(
        (void)load({"chaweb", "--wat", "value"}),
        std::runtime_error);
}

TEST_F(ApplicationConfigTest, InvalidWorkspaceNamesTheResolvedPath) {
    const std::filesystem::path missing = root_ / "missing-workspace";
    try {
        (void)load({
            "chaweb", "--root", root_.string(), "--host", "127.0.0.1",
            "--port", "8080", "--workspace", missing.string()});
        FAIL() << "expected invalid workspace to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find(missing.string()), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("--workspace"), std::string::npos);
    }
}

TEST(ExecutablePath, ResolvesTheRunningBinaryDirectory) {
    EXPECT_TRUE(std::filesystem::is_directory(executable_directory()));
}

} // namespace
} // namespace cha::web
