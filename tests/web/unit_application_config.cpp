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
        database_ = root_ / "workspace.sqlite3";
        import_ = root_ / "import";
        export_ = root_ / "export";
        std::filesystem::create_directories(import_);
        std::ofstream(database_) << "placeholder";
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    ApplicationCommand load(std::vector<std::string> arguments) {
        std::vector<const char*> pointers;
        pointers.reserve(arguments.size());
        for (const std::string& argument : arguments) {
            pointers.push_back(argument.c_str());
        }
        return parse_application_command(
            static_cast<int>(pointers.size()), pointers.data());
    }

    std::string error_text(std::vector<std::string> arguments) {
        try {
            (void)load(std::move(arguments));
            ADD_FAILURE() << "expected command line to fail";
            return {};
        } catch (const std::runtime_error& error) {
            return error.what();
        }
    }

    std::filesystem::path root_;
    std::filesystem::path database_;
    std::filesystem::path import_;
    std::filesystem::path export_;
};

TEST_F(ApplicationConfigTest, RuntimeRequiresDataAndKeepsRootIndependent) {
    const ApplicationCommand command = load({
        "chaweb", "--data", database_.string(), "--root", root_.string()});

    EXPECT_EQ(
        command.database,
        std::filesystem::weakly_canonical(std::filesystem::absolute(database_)));
    EXPECT_EQ(
        command.root,
        std::filesystem::weakly_canonical(std::filesystem::absolute(root_)));
    EXPECT_FALSE(command.import_directory);
    EXPECT_FALSE(command.export_directory);
    EXPECT_FALSE(command.host);
    EXPECT_FALSE(command.port);
    EXPECT_FALSE(command.test_idle_grace_ms);
}

TEST_F(ApplicationConfigTest, RuntimeHostAndPortAreOptionalOverrides) {
    const ApplicationCommand command = load({
        "chaweb", "--data", database_.string(), "--root", root_.string(),
        "--host", "127.0.0.1", "--port", "9000",
        "--test-idle-grace-ms", "25"});

    EXPECT_EQ(command.host, "127.0.0.1");
    EXPECT_EQ(command.port, 9000);
    EXPECT_EQ(command.test_idle_grace_ms, 25);
}

TEST_F(ApplicationConfigTest, ImportAndExportRequireDataAndRejectRuntimeOptions) {
    const ApplicationCommand imported = load({
        "chaweb", "--data", database_.string(),
        "--import", import_.string()});
    EXPECT_EQ(
        imported.import_directory,
        std::filesystem::weakly_canonical(std::filesystem::absolute(import_)));
    EXPECT_FALSE(imported.export_directory);
    EXPECT_TRUE(imported.root.empty());

    const ApplicationCommand exported = load({
        "chaweb", "--data", database_.string(),
        "--export", export_.string()});
    EXPECT_EQ(
        exported.export_directory,
        std::filesystem::weakly_canonical(std::filesystem::absolute(export_)));
    EXPECT_FALSE(exported.import_directory);

    const std::string both = error_text({
        "chaweb", "--data", database_.string(),
        "--import", import_.string(), "--export", export_.string()});
    EXPECT_NE(both.find("mutually exclusive"), std::string::npos);
    EXPECT_NE(both.find("--data DATABASE --import"), std::string::npos);

    for (const char* option : {"--root", "--host", "--port", "--test-idle-grace-ms"}) {
        const std::string value = std::string(option) == "--port"
            || std::string(option) == "--test-idle-grace-ms" ? "8080" : "x";
        const std::string text = error_text({
            "chaweb", "--data", database_.string(),
            "--import", import_.string(), option, value});
        EXPECT_NE(text.find(option), std::string::npos) << text;
        EXPECT_NE(text.find("runtime option"), std::string::npos) << text;
        EXPECT_NE(text.find("--import"), std::string::npos) << text;
    }
}

TEST_F(ApplicationConfigTest, MissingDuplicateAndUnknownOptionsNameTheRemedy) {
    const std::string missing = error_text({"chaweb"});
    EXPECT_NE(missing.find("--data DATABASE"), std::string::npos);
    EXPECT_NE(missing.find("--import SOURCE_DIRECTORY"), std::string::npos);

    const std::string duplicate = error_text({
        "chaweb", "--data", database_.string(), "--data", database_.string()});
    EXPECT_NE(duplicate.find("more than once"), std::string::npos);

    const std::string removed_config = error_text({
        "chaweb", "--config", (root_ / "app.toml").string()});
    EXPECT_NE(removed_config.find("--config"), std::string::npos);
    EXPECT_NE(removed_config.find("removed"), std::string::npos);
    EXPECT_NE(removed_config.find("--data DATABASE"), std::string::npos);

    const std::string removed_workspace = error_text({
        "chaweb", "--workspace", import_.string()});
    EXPECT_NE(removed_workspace.find("--workspace"), std::string::npos);
    EXPECT_NE(removed_workspace.find("removed"), std::string::npos);
    EXPECT_NE(removed_workspace.find("--data DATABASE"), std::string::npos);

    EXPECT_THROW((void)load({"chaweb", "--wat", "value"}), std::runtime_error);
    EXPECT_THROW((void)load({"chaweb", "--migration"}), std::runtime_error);
    EXPECT_THROW(
        (void)load({
            "chaweb", "--data", database_.string(),
            "--test-idle-grace-ms", "0"}),
        std::runtime_error);
}

TEST_F(ApplicationConfigTest, StoredSettingsReadHostAndPort) {
    const std::filesystem::path file = root_ / "app.toml";
    std::ofstream(file) << "host = \"127.0.0.1\"\nport = 8080\n";
    const StoredApplicationSettings settings =
        load_stored_application_settings(file);
    EXPECT_EQ(settings.host, "127.0.0.1");
    EXPECT_EQ(settings.port, 8080);
}

TEST_F(ApplicationConfigTest, StoredSettingsRejectOldWorkspaceFields) {
    const std::filesystem::path file = root_ / "app.toml";
    std::ofstream(file)
        << "host = \"127.0.0.1\"\n"
           "port = 8080\n"
           "workspace = \"customer-data\"\n";
    EXPECT_THROW(
        (void)load_stored_application_settings(file), std::runtime_error);

    std::ofstream(file)
        << "host = \"127.0.0.1\"\n"
           "port = 8080\n"
           "backup_dir = \"backups\"\n";
    EXPECT_THROW(
        (void)load_stored_application_settings(file), std::runtime_error);
}

TEST_F(ApplicationConfigTest, StoredSettingsRequireHostAndPort) {
    const std::filesystem::path file = root_ / "app.toml";
    std::ofstream(file) << "host = \"127.0.0.1\"\n";
    EXPECT_THROW(
        (void)load_stored_application_settings(file), std::runtime_error);
    std::ofstream(file) << "port = 8080\n";
    EXPECT_THROW(
        (void)load_stored_application_settings(file), std::runtime_error);
    std::ofstream(file) << "host = \"\"\nport = 8080\n";
    EXPECT_THROW(
        (void)load_stored_application_settings(file), std::runtime_error);
    std::ofstream(file) << "host = \"127.0.0.1\"\nport = 0\n";
    EXPECT_THROW(
        (void)load_stored_application_settings(file), std::runtime_error);
}

TEST(ExecutablePath, ResolvesTheRunningBinaryDirectory) {
    EXPECT_TRUE(std::filesystem::is_directory(executable_directory()));
}

} // namespace
} // namespace cha::web
