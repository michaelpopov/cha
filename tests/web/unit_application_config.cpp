#include "web/application_config.h"

#include "session/sqlite_storage.h"
#include "session/workspace_session_database.h"
#include "util/path_name.h"
#include "workspace/workspace_config_store.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace cha::web {
namespace {

std::string toml_path(const std::filesystem::path& path) {
    std::ostringstream out;
    out << std::quoted(path.string());
    return out.str();
}

class ApplicationConfigTest : public testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path()
            / ("cha_application_config_"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()));
        config_ = root_ / "config";
        import_ = root_ / "import";
        export_ = root_ / "export";
        std::filesystem::create_directories(config_);
        std::filesystem::create_directories(import_);
        write_app();
        write_vault("personal.toml", "Personal", "../data/workspace.sqlite3");
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    void write_app(std::string_view contents =
        "vault = \"Personal\"\n"
        "[web]\n"
        "host = \"127.0.0.1\"\n"
        "port = 8080\n"
        "[logging]\n"
        "file = \"logs/cha.log\"\n"
        "level = \"info\"\n") {
        std::ofstream(config_ / "app.toml") << contents;
    }

    void write_vault(
        std::string_view file,
        std::string_view name,
        std::string_view data,
        std::string_view extra = {}) {
        std::ofstream(config_ / std::string(file))
            << "vault_name = " << std::quoted(std::string(name)) << "\n"
            << "data = " << std::quoted(std::string(data)) << "\n"
            << extra;
    }

    void write_named_vault(
        std::string_view file,
        std::string_view name,
        const std::filesystem::path& data,
        std::string_view extra = {}) {
        write_vault(file, name, data.string(), extra);
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
        command.config_directory,
        std::filesystem::weakly_canonical(config_));
    EXPECT_EQ(command.vault.name, "Personal");
    EXPECT_EQ(
        command.vault.data,
        std::filesystem::weakly_canonical(root_ / "data" / "workspace.sqlite3"));
    EXPECT_EQ(command.root, std::filesystem::weakly_canonical(root_));
    EXPECT_EQ(command.host, "127.0.0.1");
    EXPECT_EQ(command.port, 8080);
    EXPECT_EQ(
        command.log_file,
        std::filesystem::weakly_canonical(config_ / "logs/cha.log"));
    EXPECT_EQ(command.log_level, "info");
    EXPECT_FALSE(command.vault.modify);
    EXPECT_FALSE(command.import_directory);
    EXPECT_FALSE(command.export_directory);
    EXPECT_FALSE(command.upload);
    EXPECT_FALSE(command.download);
}

TEST_F(ApplicationConfigTest, BootstrapsAnEmptyConfigurationDirectory) {
    std::filesystem::remove_all(config_);
    std::filesystem::create_directory(config_);

    const ApplicationCommand command = load({
        "chaweb", "--config=" + config_.string(), "--root", root_.string()});

    EXPECT_EQ(command.vault.name, "Default");
    EXPECT_EQ(command.vaults.size(), 1U);
    EXPECT_EQ(
        command.vault.source,
        std::filesystem::weakly_canonical(config_ / "default.toml"));
    EXPECT_EQ(
        command.vault.data,
        std::filesystem::weakly_canonical(config_ / "default.sqlite3"));
    EXPECT_EQ(
        command.vault.modify,
        std::filesystem::weakly_canonical(config_ / "modify"));
    EXPECT_EQ(command.host, "127.0.0.1");
    EXPECT_EQ(command.port, 8086);
    EXPECT_TRUE(std::filesystem::is_regular_file(config_ / "app.toml"));
    EXPECT_TRUE(std::filesystem::is_regular_file(config_ / "default.toml"));

    {
        storage::SqliteDatabase database(
            command.vault.data, storage::SqliteDatabase::Mode::read_only);
        EXPECT_NO_THROW(validate_workspace_session_contents(database));
        const std::vector<ConfigFile> configuration =
            read_workspace_config_files(database);
        ASSERT_EQ(configuration.size(), 2U);
        EXPECT_EQ(configuration[0].name, "system/assistant/character.toml");
        EXPECT_EQ(
            configuration[1].name,
            "system/providers/chatgpt/config.toml");
        storage::SqliteStatement sessions =
            database.prepare("SELECT COUNT(*) FROM sessions");
        ASSERT_TRUE(sessions.step());
        EXPECT_EQ(sessions.integer(0), 0);
    }
    EXPECT_NO_THROW((void)WorkspaceConfigStore::open(command.vault.data));
}

TEST_F(ApplicationConfigTest, DoesNotBootstrapANonemptyOrOfflineDirectory) {
    std::filesystem::remove_all(config_);
    std::filesystem::create_directory(config_);
    std::ofstream(config_ / "keep.txt") << "keep\n";

    const std::string nonempty =
        error_text({"chaweb", "--config=" + config_.string()});
    EXPECT_NE(nonempty.find("Failed to read application config"), std::string::npos)
        << nonempty;
    EXPECT_FALSE(std::filesystem::exists(config_ / "default.toml"));
    EXPECT_FALSE(std::filesystem::exists(config_ / "default.sqlite3"));

    std::filesystem::remove(config_ / "keep.txt");
    const std::string offline = error_text({
        "chaweb", "--config=" + config_.string(), "--vault=Default",
        "--import", import_.string()});
    EXPECT_NE(offline.find("Failed to read application config"), std::string::npos)
        << offline;
    EXPECT_TRUE(std::filesystem::is_empty(config_));
}

TEST_F(ApplicationConfigTest, AcceptsSeparatedConfigOptionAndAbsolutePaths) {
    const std::filesystem::path database = root_ / "absolute.sqlite3";
    const std::filesystem::path log = root_ / "absolute.log";
    write_app(
        "vault = \"Personal\"\n"
        "[web]\nhost = \"0.0.0.0\"\nport = 9000\n"
        "[logging]\nfile = " + toml_path(log) + "\nlevel = \"debug\"\n");
    write_named_vault("personal.toml", "Personal", database);

    const ApplicationCommand command =
        load({"chaweb", "--config", config_.string()});
    EXPECT_EQ(command.vault.data, std::filesystem::weakly_canonical(database));
    EXPECT_EQ(command.log_file, std::filesystem::weakly_canonical(log));
    EXPECT_EQ(command.host, "0.0.0.0");
    EXPECT_EQ(command.port, 9000);
    EXPECT_EQ(command.log_level, "debug");
}

TEST_F(ApplicationConfigTest, AcceptsZeroAsAnEphemeralPort) {
    write_app(
        "vault = \"Personal\"\n"
        "[web]\nhost = \"127.0.0.1\"\nport = 0\n"
        "[logging]\nfile = \"logs/cha.log\"\nlevel = \"info\"\n");

    const ApplicationCommand command =
        load({"chaweb", "--config", config_.string()});
    EXPECT_EQ(command.port, 0);
}

TEST_F(ApplicationConfigTest, LoadsOptionalAbsoluteMirrorAndModifyPaths) {
    const std::filesystem::path mirror = root_ / "mirror";
    const std::filesystem::path modify = root_ / "modify";
    write_vault(
        "personal.toml",
        "Personal",
        "../data/workspace.sqlite3",
        "mirror = " + toml_path(mirror) + "\n"
        "modify = " + toml_path(modify) + "\n");

    const ApplicationCommand command =
        load({"chaweb", "--config", config_.string()});
    ASSERT_TRUE(command.vault.mirror);
    ASSERT_TRUE(command.vault.modify);
    EXPECT_EQ(
        *command.vault.mirror,
        std::filesystem::weakly_canonical(mirror));
    EXPECT_EQ(
        *command.vault.modify,
        std::filesystem::weakly_canonical(modify));
}

TEST_F(ApplicationConfigTest, RejectsRelativeMirrorAndModifyPaths) {
    write_vault(
        "personal.toml",
        "Personal",
        "../data/workspace.sqlite3",
        "mirror = \"../mirror\"\n");
    EXPECT_NE(
        error_text({"chaweb", "--config", config_.string()})
            .find("absolute path 'mirror'"),
        std::string::npos);

    write_vault(
        "personal.toml",
        "Personal",
        "../data/workspace.sqlite3",
        "modify = \"../modify\"\n");
    EXPECT_NE(
        error_text({"chaweb", "--config", config_.string()})
            .find("absolute path 'modify'"),
        std::string::npos);
}

TEST_F(ApplicationConfigTest, RejectsModifyContainingConfigOrDatabase) {
    write_vault(
        "personal.toml",
        "Personal",
        "../data/workspace.sqlite3",
        "modify = " + toml_path(root_) + "\n");

    const std::string error =
        error_text({"chaweb", "--config", config_.string()});
    EXPECT_NE(error.find("modify"), std::string::npos) << error;
    EXPECT_NE(error.find("Personal"), std::string::npos) << error;
}

TEST_F(ApplicationConfigTest, RejectsAnEmptyMirrorPath) {
    write_vault(
        "personal.toml",
        "Personal",
        "../data/workspace.sqlite3",
        "mirror = \"\"\n");

    EXPECT_NE(
        error_text({"chaweb", "--config", config_.string()})
            .find("non-empty string 'mirror'"),
        std::string::npos);
}

TEST_F(ApplicationConfigTest, ImportAndExportUseConfiguredDatabase) {
    const ApplicationCommand imported = load({
        "chaweb", "--config=" + config_.string(),
        "--vault=Personal", "--import", import_.string()});
    EXPECT_EQ(
        imported.import_directory,
        std::filesystem::weakly_canonical(std::filesystem::absolute(import_)));
    EXPECT_EQ(imported.vault.name, "Personal");
    EXPECT_TRUE(imported.root.empty());

    const ApplicationCommand exported = load({
        "chaweb", "--config=" + config_.string(),
        "--vault", "Personal", "--export=" + export_.string()});
    EXPECT_EQ(
        exported.export_directory,
        std::filesystem::weakly_canonical(std::filesystem::absolute(export_)));

    const std::string both = error_text({
        "chaweb", "--config=" + config_.string(),
        "--vault=Personal",
        "--import", import_.string(), "--export", export_.string()});
    EXPECT_NE(both.find("mutually exclusive"), std::string::npos);
}

TEST_F(ApplicationConfigTest, UploadAndDownloadAreOfflineFlags) {
    const ApplicationCommand uploaded = load({
        "chaweb", "--config=" + config_.string(),
        "--vault=Personal", "--upload"});
    EXPECT_TRUE(uploaded.upload);
    EXPECT_FALSE(uploaded.download);
    EXPECT_TRUE(uploaded.root.empty());

    const ApplicationCommand downloaded = load({
        "chaweb", "--download", "--vault=Personal",
        "--config=" + config_.string()});
    EXPECT_FALSE(downloaded.upload);
    EXPECT_TRUE(downloaded.download);
    EXPECT_TRUE(downloaded.root.empty());

    EXPECT_NE(
        error_text({
            "chaweb", "--config=" + config_.string(),
            "--vault=Personal", "--upload", "--download"})
            .find("mutually exclusive"),
        std::string::npos);
    EXPECT_NE(
        error_text({
            "chaweb", "--config=" + config_.string(),
            "--vault=Personal", "--upload", "--export", export_.string()})
            .find("mutually exclusive"),
        std::string::npos);
}

TEST_F(ApplicationConfigTest, ImportRequiresTheConfigurationDirectoryToBeExternal) {
    const std::filesystem::path inside = import_ / "cha-config";
    std::filesystem::create_directories(inside);
    std::filesystem::copy_file(config_ / "app.toml", inside / "app.toml");
    std::filesystem::copy_file(
        config_ / "personal.toml", inside / "personal.toml");
    const std::string error = error_text({
        "chaweb", "--config=" + inside.string(),
        "--vault=Personal", "--import", import_.string()});
    EXPECT_NE(error.find("outside the imported workspace"), std::string::npos)
        << error;
}

TEST_F(ApplicationConfigTest, RejectsMissingDuplicateAndRuntimeOfflineOptions) {
    EXPECT_NE(
        error_text({"chaweb"}).find("Missing --config"), std::string::npos);
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
            "--vault=Personal",
            "--import", import_.string(), "--root", root_.string()})
            .find("runtime option"),
        std::string::npos);
    EXPECT_NE(
        error_text({
            "chaweb", "--config=" + config_.string(),
            "--vault=Personal", "--download", "--root", root_.string()})
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
            "--vault=Personal", "--upload", "--upload"})
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
        "vault = \"Personal\"\n[logging]\nfile = \"x\"\nlevel = \"off\"\n",
        "vault = \"Personal\"\n[web]\nhost = \"\"\nport = 1\n[logging]\nfile = \"x\"\nlevel = \"off\"\n",
        "vault = \"Personal\"\n[web]\nhost = \"x\"\nport = -1\n[logging]\nfile = \"x\"\nlevel = \"off\"\n",
        "vault = \"Personal\"\n[web]\nhost = \"x\"\nport = 1\n[logging]\nlevel = \"off\"\n",
        "vault = \"Personal\"\nextra = true\n[web]\nhost = \"x\"\nport = 1\n[logging]\nfile = \"x\"\nlevel = \"off\"\n",
    };
    for (const std::string& contents : invalid) {
        write_app(contents);
        EXPECT_THROW(
            (void)load({"chaweb", "--config=" + config_.string()}),
            std::runtime_error)
            << contents;
    }
}

TEST_F(ApplicationConfigTest, LoadsMultipleVaultsWithCanonicalSpellingAndOrder) {
    write_app(
        "vault = \"personal\"\n"
        "[web]\nhost = \"127.0.0.1\"\nport = 8080\n"
        "[logging]\nfile = \"logs/cha.log\"\nlevel = \"info\"\n");
    write_vault("personal.toml", "Personal", "personal.sqlite3");
    write_vault("projects.toml", "Projects", "projects.sqlite3");
    write_vault("archive.toml", "archive", "archive.sqlite3");
    std::ofstream(config_ / "notes.txt") << "ignored\n";
    std::filesystem::create_directories(config_ / "nested");
    std::ofstream(config_ / "nested" / "hidden.toml")
        << "vault_name = \"Hidden\"\ndata = \"hidden.sqlite3\"\n";

    const ConfigurationDirectory loaded =
        load_configuration_directory(config_);
    ASSERT_EQ(loaded.vaults.size(), 3U);
    EXPECT_EQ(loaded.startup_vault, "Personal");
    EXPECT_EQ(loaded.vaults[0].name, "archive");
    EXPECT_EQ(loaded.vaults[1].name, "Personal");
    EXPECT_EQ(loaded.vaults[2].name, "Projects");
    EXPECT_EQ(
        loaded.vaults[1].data,
        std::filesystem::weakly_canonical(config_ / "personal.sqlite3"));
    EXPECT_EQ(find_vault(loaded.vaults, "PROJECTS")->name, "Projects");
    EXPECT_EQ(find_vault(loaded.vaults, "missing"), nullptr);
    EXPECT_TRUE(same_vault_name("Personal", "personal"));

    const ApplicationCommand command =
        load({"chaweb", "--config", config_.string()});
    EXPECT_EQ(command.vault.name, "Personal");
    EXPECT_EQ(command.vaults.size(), 3U);
}

TEST_F(ApplicationConfigTest, AcceptsUnicodeAndEntranceVaultNames) {
    write_app(
        "vault = \"Café\"\n"
        "[web]\nhost = \"127.0.0.1\"\nport = 8080\n"
        "[logging]\nfile = \"logs/cha.log\"\nlevel = \"info\"\n");
    write_vault("cafe.toml", "Café", "cafe.sqlite3");
    write_vault("entrance.toml", "Entrance", "entrance.sqlite3");

    const ConfigurationDirectory loaded =
        load_configuration_directory(config_);
    EXPECT_EQ(loaded.startup_vault, "Café");
    ASSERT_NE(find_vault(loaded.vaults, "Entrance"), nullptr);
    EXPECT_EQ(find_vault(loaded.vaults, "café")->name, "Café");
}

TEST_F(ApplicationConfigTest, RejectsInvalidVaultNames) {
    write_vault("personal.toml", " Personal", "personal.sqlite3");
    EXPECT_NE(
        error_text({"chaweb", "--config", config_.string()})
            .find("whitespace"),
        std::string::npos);

    write_vault("personal.toml", "Bad\nName", "personal.sqlite3");
    EXPECT_NE(
        error_text({"chaweb", "--config", config_.string()})
            .find("control character"),
        std::string::npos);
}

TEST_F(ApplicationConfigTest, RejectsAFileOrMissingDirectoryAsConfig) {
    const std::filesystem::path file = root_ / "cha.toml";
    std::ofstream(file) << "vault = \"Personal\"\n";
    EXPECT_NE(
        error_text({"chaweb", "--config", file.string()})
            .find("existing directory"),
        std::string::npos);
    EXPECT_NE(
        error_text({"chaweb", "--config", (root_ / "missing").string()})
            .find("existing directory"),
        std::string::npos);
}

TEST_F(ApplicationConfigTest, RejectsMissingInvalidAndEmptyVaultRegistry) {
    std::filesystem::remove(config_ / "app.toml");
    EXPECT_NE(
        error_text({"chaweb", "--config", config_.string()})
            .find("Failed to read application config"),
        std::string::npos);

    write_app("not toml");
    EXPECT_NE(
        error_text({"chaweb", "--config", config_.string()})
            .find("Failed to parse application config"),
        std::string::npos);

    write_app();
    std::filesystem::remove(config_ / "personal.toml");
    const std::string empty =
        error_text({"chaweb", "--config", config_.string()});
    EXPECT_NE(empty.find("no vault definitions"), std::string::npos) << empty;

    write_vault("personal.toml", "Personal", "personal.sqlite3", "oops = true\n");
    EXPECT_NE(
        error_text({"chaweb", "--config", config_.string()})
            .find("unknown field 'oops'"),
        std::string::npos);

    write_vault("personal.toml", "Personal", "personal.sqlite3");
    std::ofstream(config_ / "broken.toml") << "vault_name = \"Other\"\n[";
    EXPECT_NE(
        error_text({"chaweb", "--config", config_.string()})
            .find("Failed to parse vault definition"),
        std::string::npos);
}

TEST_F(ApplicationConfigTest, RejectsDuplicateNamesPathsAndFilenames) {
    write_vault("personal.toml", "Personal", "personal.sqlite3");
    write_vault("other.toml", "personal", "other.sqlite3");
    const std::string names =
        error_text({"chaweb", "--config", config_.string()});
    EXPECT_NE(names.find("duplicates"), std::string::npos) << names;
    EXPECT_NE(names.find("personal.toml"), std::string::npos) << names;
    EXPECT_NE(names.find("other.toml"), std::string::npos) << names;

    write_vault("other.toml", "Projects", "personal.sqlite3");
    const std::string paths =
        error_text({"chaweb", "--config", config_.string()});
    EXPECT_NE(paths.find("collides"), std::string::npos) << paths;
    EXPECT_NE(paths.find("data"), std::string::npos) << paths;

    write_named_vault(
        "other.toml",
        "Projects",
        root_ / "elsewhere" / "personal.sqlite3");
    const std::string filenames =
        error_text({"chaweb", "--config", config_.string()});
    EXPECT_NE(filenames.find("filenames must be unique"), std::string::npos)
        << filenames;
}

TEST_F(ApplicationConfigTest, RejectsNestedModifyCollisions) {
    write_vault(
        "personal.toml",
        "Personal",
        "personal.sqlite3",
        "modify = " + toml_path(root_ / "edit") + "\n");
    write_vault(
        "projects.toml",
        "Projects",
        "projects.sqlite3",
        "modify = " + toml_path(root_ / "edit" / "nested") + "\n");
    const std::string error =
        error_text({"chaweb", "--config", config_.string()});
    EXPECT_NE(error.find("modify"), std::string::npos) << error;
    EXPECT_NE(error.find("Personal"), std::string::npos) << error;
    EXPECT_NE(error.find("Projects"), std::string::npos) << error;
}

TEST_F(ApplicationConfigTest, RejectsUnknownStartupAndConsoleSelections) {
    write_app(
        "vault = \"Missing\"\n"
        "[web]\nhost = \"127.0.0.1\"\nport = 8080\n"
        "[logging]\nfile = \"logs/cha.log\"\nlevel = \"info\"\n");
    EXPECT_NE(
        error_text({"chaweb", "--config", config_.string()})
            .find("does not name a discovered vault"),
        std::string::npos);

    write_app();
    EXPECT_NE(
        error_text({
            "chaweb", "--config", config_.string(), "--import",
            import_.string()}).find("required with --import"),
        std::string::npos);
    EXPECT_NE(
        error_text({
            "chaweb", "--config", config_.string(), "--vault=Personal"})
            .find("server mode"),
        std::string::npos);
    EXPECT_NE(
        error_text({
            "chaweb", "--config", config_.string(),
            "--vault=Missing", "--upload"}).find("Unknown vault"),
        std::string::npos);
    EXPECT_NE(
        error_text({
            "chaweb", "--config", config_.string(),
            "--vault=", "--upload"}).find("non-empty name"),
        std::string::npos);
}

TEST_F(ApplicationConfigTest, ConsoleSelectionDoesNotRewriteAppToml) {
    std::ifstream original(config_ / "app.toml");
    const std::string contents{
        std::istreambuf_iterator<char>(original),
        std::istreambuf_iterator<char>()};
    write_vault("projects.toml", "Projects", "projects.sqlite3");
    const ApplicationCommand command = load({
        "chaweb", "--config", config_.string(),
        "--vault=projects", "--upload"});
    EXPECT_EQ(command.vault.name, "Projects");
    std::ifstream after(config_ / "app.toml");
    const std::string rewritten{
        std::istreambuf_iterator<char>(after),
        std::istreambuf_iterator<char>()};
    EXPECT_EQ(rewritten, contents);
}

TEST_F(ApplicationConfigTest, ResolvesSymlinkedConfigParents) {
    const std::filesystem::path real_parent = root_ / "real";
    const std::filesystem::path link_parent = root_ / "link";
    std::filesystem::create_directories(real_parent / "config");
    std::error_code error;
    std::filesystem::create_directory_symlink(real_parent, link_parent, error);
    if (error) {
        GTEST_SKIP() << "symlinks are not supported: " << error.message();
    }
    std::ofstream(real_parent / "config" / "app.toml")
        << "vault = \"Personal\"\n"
           "[web]\nhost = \"127.0.0.1\"\nport = 8080\n"
           "[logging]\nfile = \"cha.log\"\nlevel = \"info\"\n";
    std::ofstream(real_parent / "config" / "personal.toml")
        << "vault_name = \"Personal\"\ndata = \"personal.sqlite3\"\n";

    const ApplicationCommand command = load({
        "chaweb", "--config", (link_parent / "config").string()});
    EXPECT_EQ(
        command.config_directory,
        std::filesystem::weakly_canonical(real_parent / "config"));
    EXPECT_EQ(
        command.vault.data,
        std::filesystem::weakly_canonical(
            real_parent / "config" / "personal.sqlite3"));
}

TEST(ExecutablePath, ResolvesTheRunningBinaryDirectory) {
    EXPECT_TRUE(std::filesystem::is_directory(executable_directory()));
}

} // namespace
} // namespace cha::web
