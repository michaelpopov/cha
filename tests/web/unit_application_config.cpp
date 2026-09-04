#include "web/application_config.h"

#include "util/path_name.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
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
        config_ = root_ / "config" / "cha.toml";
        import_ = root_ / "import";
        export_ = root_ / "export";
        std::filesystem::create_directories(config_.parent_path());
        std::filesystem::create_directories(import_);
        write_config();
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    void write_config(std::string_view contents =
        "data = \"../data/workspace.sqlite3\"\n"
        "[web]\n"
        "host = \"127.0.0.1\"\n"
        "port = 8080\n"
        "[logging]\n"
        "file = \"logs/cha.log\"\n"
        "level = \"info\"\n") {
        std::ofstream(config_) << contents;
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
    std::filesystem::path config_;
    std::filesystem::path import_;
    std::filesystem::path export_;
};

TEST_F(ApplicationConfigTest, LoadsUnifiedExternalConfigWithEqualsSyntax) {
    const ApplicationCommand command = load({
        "chaweb", "--config=" + config_.string(), "--root", root_.string()});

    EXPECT_EQ(
        command.database,
        std::filesystem::weakly_canonical(root_ / "data" / "workspace.sqlite3"));
    EXPECT_EQ(command.root, std::filesystem::weakly_canonical(root_));
    EXPECT_EQ(command.host, "127.0.0.1");
    EXPECT_EQ(command.port, 8080);
    EXPECT_EQ(
        command.log_file,
        std::filesystem::weakly_canonical(config_.parent_path() / "logs/cha.log"));
    EXPECT_EQ(command.log_level, "info");
    EXPECT_FALSE(command.import_directory);
    EXPECT_FALSE(command.export_directory);
    EXPECT_FALSE(command.upload);
    EXPECT_FALSE(command.download);
}

TEST_F(ApplicationConfigTest, AcceptsSeparatedConfigOptionAndAbsolutePaths) {
    const std::filesystem::path database = root_ / "absolute.sqlite3";
    const std::filesystem::path log = root_ / "absolute.log";
    write_config(
        "data = \"" + database.string() + "\"\n"
        "[web]\nhost = \"0.0.0.0\"\nport = 9000\n"
        "[logging]\nfile = \"" + log.string() + "\"\nlevel = \"debug\"\n");

    const ApplicationCommand command =
        load({"chaweb", "--config", config_.string()});
    EXPECT_EQ(command.database, std::filesystem::weakly_canonical(database));
    EXPECT_EQ(command.log_file, std::filesystem::weakly_canonical(log));
    EXPECT_EQ(command.host, "0.0.0.0");
    EXPECT_EQ(command.port, 9000);
    EXPECT_EQ(command.log_level, "debug");
}

TEST_F(ApplicationConfigTest, LoadsOptionalMirrorRelativeToConfig) {
    write_config(
        "data = \"../data/workspace.sqlite3\"\n"
        "mirror = \"../mirror\"\n"
        "[web]\nhost = \"127.0.0.1\"\nport = 8080\n"
        "[logging]\nfile = \"logs/cha.log\"\nlevel = \"info\"\n");

    const ApplicationCommand command =
        load({"chaweb", "--config", config_.string()});
    ASSERT_TRUE(command.mirror);
    EXPECT_EQ(
        *command.mirror,
        std::filesystem::weakly_canonical(root_ / "mirror"));
}

TEST_F(ApplicationConfigTest, RejectsAnEmptyMirrorPath) {
    write_config(
        "data = \"../data/workspace.sqlite3\"\n"
        "mirror = \"\"\n"
        "[web]\nhost = \"127.0.0.1\"\nport = 8080\n"
        "[logging]\nfile = \"logs/cha.log\"\nlevel = \"info\"\n");

    EXPECT_NE(
        error_text({"chaweb", "--config", config_.string()})
            .find("non-empty string 'mirror'"),
        std::string::npos);
}

TEST_F(ApplicationConfigTest, ImportAndExportUseConfiguredDatabase) {
    const ApplicationCommand imported = load({
        "chaweb", "--config=" + config_.string(),
        "--import", import_.string()});
    EXPECT_EQ(
        imported.import_directory,
        std::filesystem::weakly_canonical(std::filesystem::absolute(import_)));
    EXPECT_TRUE(imported.root.empty());

    const ApplicationCommand exported = load({
        "chaweb", "--config=" + config_.string(),
        "--export=" + export_.string()});
    EXPECT_EQ(
        exported.export_directory,
        std::filesystem::weakly_canonical(std::filesystem::absolute(export_)));

    const std::string both = error_text({
        "chaweb", "--config=" + config_.string(),
        "--import", import_.string(), "--export", export_.string()});
    EXPECT_NE(both.find("mutually exclusive"), std::string::npos);
}

TEST_F(ApplicationConfigTest, UploadAndDownloadAreOfflineFlags) {
    const ApplicationCommand uploaded = load({
        "chaweb", "--config=" + config_.string(), "--upload"});
    EXPECT_TRUE(uploaded.upload);
    EXPECT_FALSE(uploaded.download);
    EXPECT_TRUE(uploaded.root.empty());

    const ApplicationCommand downloaded = load({
        "chaweb", "--download", "--config=" + config_.string()});
    EXPECT_FALSE(downloaded.upload);
    EXPECT_TRUE(downloaded.download);
    EXPECT_TRUE(downloaded.root.empty());

    EXPECT_NE(
        error_text({
            "chaweb", "--config=" + config_.string(),
            "--upload", "--download"}).find("mutually exclusive"),
        std::string::npos);
    EXPECT_NE(
        error_text({
            "chaweb", "--config=" + config_.string(),
            "--upload", "--export", export_.string()})
            .find("mutually exclusive"),
        std::string::npos);
}

TEST_F(ApplicationConfigTest, ImportRequiresTheApplicationConfigToBeExternal) {
    const std::filesystem::path inside = import_ / "cha.toml";
    std::filesystem::copy_file(config_, inside);
    const std::string error = error_text({
        "chaweb", "--config=" + inside.string(),
        "--import", import_.string()});
    EXPECT_NE(error.find("outside the imported workspace"), std::string::npos)
        << error;
}

TEST_F(ApplicationConfigTest, RejectsMissingDuplicateAndRuntimeOfflineOptions) {
    EXPECT_NE(error_text({"chaweb"}).find("Missing --config"), std::string::npos);
    EXPECT_NE(
        error_text({"chaweb", "--config"}).find("requires a value"),
        std::string::npos);
    EXPECT_NE(
        error_text({
            "chaweb", "--config", config_.string(),
            "--config=" + config_.string()}).find("more than once"),
        std::string::npos);
    EXPECT_NE(
        error_text({"chaweb", "--data", "old.sqlite3"}).find("Unknown option"),
        std::string::npos);
    EXPECT_NE(
        error_text({"chaweb", "--host", "127.0.0.1"}).find("Unknown option"),
        std::string::npos);
    EXPECT_NE(
        error_text({
            "chaweb", "--config=" + config_.string(),
            "--import", import_.string(), "--root", root_.string()})
            .find("runtime option"),
        std::string::npos);
    EXPECT_NE(
        error_text({
            "chaweb", "--config=" + config_.string(),
            "--download", "--root", root_.string()})
            .find("runtime option"),
        std::string::npos);
    EXPECT_NE(
        error_text({
            "chaweb", "--config=" + config_.string(), "--upload=value"})
            .find("does not take a value"),
        std::string::npos);
    EXPECT_NE(
        error_text({
            "chaweb", "--config=" + config_.string(),
            "--upload", "--upload"})
            .find("more than once"),
        std::string::npos);
    EXPECT_NE(
        error_text({
            "chaweb", "--config=" + config_.string(),
            "--test-idle-grace-ms", "0"}).find("positive integer"),
        std::string::npos);
}

TEST_F(ApplicationConfigTest, RequiresAllSettingsWithValidTypes) {
    const std::vector<std::string> invalid{
        "[web]\nhost = \"x\"\nport = 1\n[logging]\nfile = \"x\"\nlevel = \"off\"\n",
        "data = \"x\"\n[logging]\nfile = \"x\"\nlevel = \"off\"\n",
        "data = \"x\"\n[web]\nhost = \"\"\nport = 1\n[logging]\nfile = \"x\"\nlevel = \"off\"\n",
        "data = \"x\"\n[web]\nhost = \"x\"\nport = 0\n[logging]\nfile = \"x\"\nlevel = \"off\"\n",
        "data = \"x\"\n[web]\nhost = \"x\"\nport = 1\n[logging]\nlevel = \"off\"\n",
        "data = \"x\"\nextra = true\n[web]\nhost = \"x\"\nport = 1\n[logging]\nfile = \"x\"\nlevel = \"off\"\n",
    };
    for (const std::string& contents : invalid) {
        write_config(contents);
        EXPECT_THROW(
            (void)load({"chaweb", "--config=" + config_.string()}),
            std::runtime_error)
            << contents;
    }
}

TEST(ExecutablePath, ResolvesTheRunningBinaryDirectory) {
    EXPECT_TRUE(std::filesystem::is_directory(executable_directory()));
}

} // namespace
} // namespace cha::web
