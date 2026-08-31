#include "session/session_lease.h"
#include "session/sqlite_storage.h"
#include "session/workspace_session_database.h"
#include "support/test_workspace.h"
#include "util/environment.h"
#include "util/path_name.h"
#include "workspace/workspace.h"
#include "workspace/workspace_config_store.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace cha {
namespace {

using Database = storage::SqliteDatabase;
using Statement = storage::SqliteStatement;

class ScopedEnvironmentVariable {
public:
    explicit ScopedEnvironmentVariable(std::string name) : name_(std::move(name)) {
        if (const char* value = std::getenv(name_.c_str())) {
            previous_value_ = value;
        }
    }

    ~ScopedEnvironmentVariable() {
        if (previous_value_) {
            (void)set_environment_variable(name_, *previous_value_);
        } else {
            (void)unset_environment_variable(name_);
        }
    }

private:
    std::string name_;
    std::optional<std::string> previous_value_;
};

std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

void write_bytes(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
}

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

void make_v1_database(const std::filesystem::path& path) {
    create_empty_workspace_session_database(path);
    Database database(path, Database::Mode::read_write);
    database.execute("DROP TABLE config");
    database.execute(
        "PRAGMA user_version = "
        + std::to_string(workspace_session_database_version_v1));
}

void seed_session_rows(Database& database) {
    database.execute("INSERT INTO forums (forum_id) VALUES ('lobby')");
    database.execute(
        "INSERT INTO sessions (forum_key, session_id, label, updated_at, "
        "archived_at, history_epoch, next_entry_id, next_request_id) "
        "VALUES (1, 'active', 'Active', 10, NULL, 1, 3, 2), "
        "(1, 'old', 'Archived', 4, 5, 1, 1, 1)");
    database.execute(
        "INSERT INTO turns (session_key, request_id, epoch, state) "
        "VALUES (1, 1, 1, 1)");
    database.execute(
        "INSERT INTO entries (session_key, entry_id, epoch, request_id, kind, "
        "participant_id, display_name, addressed_to, addressed_to_name, text, "
        "status, created_at) VALUES "
        "(1, 1, 1, 1, 0, 'human', 'You', 'guide', 'Guide', 'Hello', 0, 11), "
        "(1, 2, 1, 1, 1, 'guide', 'Guide', '', '', 'Hi', 0, 12)");
}

void expect_seeded_session_rows(Database& database) {
    Statement sessions = database.prepare(
        "SELECT session_id, label, updated_at, archived_at FROM sessions "
        "ORDER BY session_id");
    ASSERT_TRUE(sessions.step());
    EXPECT_EQ(sessions.text(0), "active");
    EXPECT_EQ(sessions.text(1), "Active");
    EXPECT_EQ(sessions.integer(2), 10);
    EXPECT_TRUE(sessions.is_null(3));
    ASSERT_TRUE(sessions.step());
    EXPECT_EQ(sessions.text(0), "old");
    EXPECT_EQ(sessions.text(1), "Archived");
    EXPECT_EQ(sessions.integer(2), 4);
    EXPECT_EQ(sessions.integer(3), 5);
    EXPECT_FALSE(sessions.step());

    Statement entries = database.prepare(
        "SELECT entry_id, text FROM entries ORDER BY entry_id");
    ASSERT_TRUE(entries.step());
    EXPECT_EQ(entries.integer(0), 1);
    EXPECT_EQ(entries.text(1), "Hello");
    ASSERT_TRUE(entries.step());
    EXPECT_EQ(entries.integer(0), 2);
    EXPECT_EQ(entries.text(1), "Hi");
    EXPECT_FALSE(entries.step());
}

class WorkspaceConfigStoreTest : public testing::Test {
protected:
    void SetUp() override {
        write_bytes(
            workspace_.root() / "app.toml",
            "host = \"127.0.0.1\"\nport = 8080\n");
        export_ = workspace_.root().parent_path()
            / (workspace_.root().filename().string() + "_export");
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(export_, error);
    }

    const std::filesystem::path& source() const { return workspace_.root(); }
    std::filesystem::path database() const {
        return source() / "workspace.sqlite3";
    }

    std::size_t import_from_source() {
        return import_workspace_configuration(source(), database()).file_count;
    }

    test::TestWorkspace workspace_;
    std::filesystem::path export_;
};

TEST_F(WorkspaceConfigStoreTest, ImportsIntoAMissingDatabase) {
    const std::size_t count = import_from_source();
    EXPECT_GE(count, 8U);
    EXPECT_EQ(
        inspect_workspace_session_database(database()),
        WorkspaceDatabaseState::valid_v2);
    Database handle(database(), Database::Mode::read_only);
    EXPECT_EQ(
        handle.pragma_integer("user_version"),
        workspace_session_database_version);
    bool found_app = false;
    bool found_workspace = false;
    for (const ConfigFile& row : read_workspace_config_files(handle)) {
        if (row.name == "app.toml") found_app = true;
        if (row.name == "workspace.toml") found_workspace = true;
    }
    EXPECT_TRUE(found_app);
    EXPECT_TRUE(found_workspace);
#ifndef _WIN32
    EXPECT_EQ(posix_mode(database()), static_cast<mode_t>(0600));
#endif
}

TEST_F(WorkspaceConfigStoreTest, UpgradesV1AndPreservesSessions) {
    make_v1_database(database());
    {
        Database handle(database(), Database::Mode::read_write);
        seed_session_rows(handle);
    }

    import_from_source();

    EXPECT_EQ(
        inspect_workspace_session_database(database()),
        WorkspaceDatabaseState::valid_v2);
    Database handle(database(), Database::Mode::read_write);
    expect_seeded_session_rows(handle);
    EXPECT_EQ(
        handle.pragma_integer("user_version"),
        workspace_session_database_version);
}

TEST_F(WorkspaceConfigStoreTest, ReplacesV2ConfigurationAndPreservesSessions) {
    import_from_source();
    {
        Database handle(database(), Database::Mode::read_write);
        seed_session_rows(handle);
        storage::SqliteTransaction transaction(handle);
        replace_workspace_config_files(
            handle,
            {{"app.toml", "host = \"0.0.0.0\"\nport = 1\n"},
             {"workspace.toml", "old = true\n"}});
        transaction.commit();
    }

    write_bytes(source() / "app.toml", "host = \"127.0.0.1\"\nport = 9050\n");
    import_from_source();

    Database handle(database(), Database::Mode::read_only);
    expect_seeded_session_rows(handle);
    bool found_new_port = false;
    for (const ConfigFile& row : read_workspace_config_files(handle)) {
        if (row.name == "app.toml") {
            EXPECT_NE(row.content.find("9050"), std::string::npos);
            found_new_port = true;
        }
        if (row.name == "workspace.toml") {
            EXPECT_EQ(row.content.find("old = true"), std::string::npos);
        }
    }
    EXPECT_TRUE(found_new_port);
}

TEST_F(WorkspaceConfigStoreTest, RoundTripsAcceptedFilesByteForByte) {
    std::string binary{"hi"};
    binary.push_back('\0');
    binary.push_back(static_cast<char>(0xFF));
    binary.push_back('\n');
    write_bytes(source() / "notes.md", binary);
    write_bytes(source() / "empty.md", "");
    write_bytes(source() / "shared" / "nested.toml", "value = 1\n");
    write_bytes(
        source() / "shared" / "notes-юникод.md",
        "Привет\n");
    write_bytes(source() / ".env", "CHA_UNUSED=1\n");
    write_bytes(source() / "ignored.txt", "not stored\n");
    write_bytes(source() / "README", "not stored\n");
    write_bytes(source() / "workspace.sqlite3-wal", "ignored sidecar\n");

    const std::size_t imported = import_from_source();
    const std::size_t exported =
        export_workspace_configuration(database(), export_).file_count;
    EXPECT_EQ(imported, exported);

    Database handle(database(), Database::Mode::read_only);
    const std::vector<ConfigFile> rows = read_workspace_config_files(handle);
    EXPECT_EQ(rows.size(), imported);
    for (const ConfigFile& row : rows) {
        const std::filesystem::path exported_file = export_ / path_from_utf8(row.name);
        EXPECT_EQ(file_bytes(exported_file), row.content) << row.name;
        EXPECT_EQ(file_bytes(source() / path_from_utf8(row.name)), row.content)
            << row.name;
    }
    EXPECT_FALSE(std::filesystem::exists(export_ / "ignored.txt"));
    EXPECT_FALSE(std::filesystem::exists(export_ / "README"));
    EXPECT_FALSE(std::filesystem::exists(export_ / "workspace.sqlite3"));
    EXPECT_FALSE(std::filesystem::exists(export_ / "workspace.sqlite3-wal"));
    EXPECT_FALSE(std::filesystem::exists(export_ / "workspace.sqlite3-shm"));
    EXPECT_FALSE(std::filesystem::exists(export_ / "workspace.sqlite3.cha-lock"));
    EXPECT_FALSE(std::filesystem::exists(export_ / "sessions"));
    EXPECT_TRUE(std::filesystem::is_directory(export_ / "system" / "providers"));
    EXPECT_TRUE(std::filesystem::is_directory(export_ / "personas"));
    EXPECT_TRUE(std::filesystem::is_directory(export_ / "characters"));
    EXPECT_TRUE(std::filesystem::is_directory(export_ / "forums"));
#ifndef _WIN32
    EXPECT_EQ(posix_mode(export_ / ".env"), static_cast<mode_t>(0600));
#endif
}

TEST_F(WorkspaceConfigStoreTest, RecreatesEmptySkeletonDirectories) {
    import_from_source();
    export_workspace_configuration(database(), export_);
    EXPECT_TRUE(std::filesystem::is_directory(export_ / "system" / "providers"));
    EXPECT_TRUE(std::filesystem::is_directory(export_ / "personas"));
    EXPECT_TRUE(std::filesystem::is_directory(export_ / "characters"));
    EXPECT_TRUE(std::filesystem::is_directory(export_ / "forums"));
}

TEST_F(WorkspaceConfigStoreTest, RejectsMissingRequiredFiles) {
    std::filesystem::remove(source() / "app.toml");
    EXPECT_THROW((void)import_from_source(), std::runtime_error);
    EXPECT_EQ(
        inspect_workspace_session_database(database()),
        WorkspaceDatabaseState::missing);

    write_bytes(source() / "app.toml", "host = \"127.0.0.1\"\nport = 8080\n");
    std::filesystem::remove(source() / "workspace.toml");
    EXPECT_THROW((void)import_from_source(), std::runtime_error);
    EXPECT_EQ(
        inspect_workspace_session_database(database()),
        WorkspaceDatabaseState::missing);
}

TEST_F(WorkspaceConfigStoreTest, RejectsUnsafeAndUnsupportedStoredNames) {
    write_bytes(source() / "C:host.toml", "host = 1\n");
    EXPECT_THROW((void)import_from_source(), std::runtime_error);

    std::filesystem::remove(source() / "C:host.toml");
    write_bytes(source() / "bad\\name.toml", "x = 1\n");
    EXPECT_THROW((void)import_from_source(), std::runtime_error);
    EXPECT_EQ(
        inspect_workspace_session_database(database()),
        WorkspaceDatabaseState::missing);
}

TEST_F(WorkspaceConfigStoreTest, RejectsMatchingSymlinksAndSkipsSymlinkedDirectories) {
    const std::filesystem::path target = source() / "characters" / "guide" / "CHARACTER.md";
    const std::filesystem::path link = source() / "copy.md";
    if (!try_create_symlink(target, link, false)) {
        GTEST_SKIP() << "symbolic links are not available";
    }
    EXPECT_THROW((void)import_from_source(), std::runtime_error);
    EXPECT_EQ(
        inspect_workspace_session_database(database()),
        WorkspaceDatabaseState::missing);

    std::filesystem::remove(link);
    const std::filesystem::path extra = source() / "extra-config";
    std::filesystem::create_directories(extra);
    write_bytes(extra / "hidden.toml", "x = 1\n");
    const std::filesystem::path dir_link = source() / "linked-dir";
    if (!try_create_symlink(extra, dir_link, true)) {
        GTEST_SKIP() << "directory symbolic links are not available";
    }
    const std::size_t count = import_from_source();
    Database handle(database(), Database::Mode::read_only);
    bool found_real_extra = false;
    for (const ConfigFile& row : read_workspace_config_files(handle)) {
        EXPECT_NE(row.name, "linked-dir/hidden.toml");
        if (row.name == "extra-config/hidden.toml") found_real_extra = true;
    }
    EXPECT_TRUE(found_real_extra);
    EXPECT_GE(count, 8U);
}

TEST_F(WorkspaceConfigStoreTest, UsesDotenvOnlyProviderKeysAndRestoresEnvironment) {
    constexpr char variable[] = "CHA_IMPORT_STORE_CREDENTIAL_A1B2";
    ScopedEnvironmentVariable guard(variable);
    ASSERT_TRUE(unset_environment_variable(variable));
    workspace_.write_provider(
        "secured",
        "host = \"example.test\"\n"
        "port = 443\n"
        "mode = \"net\"\n"
        "model = \"secured\"\n"
        "api_key_env = \"CHA_IMPORT_STORE_CREDENTIAL_A1B2\"\n");
    workspace_.write_character_config(
        "display_name = \"Guide\"\nprovider = \"secured\"\n");
    write_bytes(source() / ".env", "CHA_IMPORT_STORE_CREDENTIAL_A1B2=secret-key\n");

    EXPECT_EQ(std::getenv(variable), nullptr);
    EXPECT_NO_THROW((void)import_from_source());
    EXPECT_EQ(std::getenv(variable), nullptr);
    EXPECT_EQ(
        inspect_workspace_session_database(database()),
        WorkspaceDatabaseState::valid_v2);
}

TEST_F(WorkspaceConfigStoreTest, InheritedEnvironmentValuesWinIncludingEmpty) {
    constexpr char variable[] = "CHA_IMPORT_STORE_INHERITED_C3D4";
    ScopedEnvironmentVariable guard(variable);
    workspace_.write_provider(
        "secured",
        "host = \"example.test\"\n"
        "port = 443\n"
        "mode = \"net\"\n"
        "model = \"secured\"\n"
        "api_key_env = \"CHA_IMPORT_STORE_INHERITED_C3D4\"\n");
    workspace_.write_character_config(
        "display_name = \"Guide\"\nprovider = \"secured\"\n");
    write_bytes(source() / ".env", "CHA_IMPORT_STORE_INHERITED_C3D4=from-file\n");

    ASSERT_TRUE(set_environment_variable(variable, "from-process"));
    EXPECT_NO_THROW((void)import_from_source());
    EXPECT_STREQ(std::getenv(variable), "from-process");

    std::filesystem::remove(database());
    ASSERT_TRUE(set_environment_variable(variable, ""));
    if (std::getenv(variable) == nullptr) {
        GTEST_SKIP() << "this platform does not retain empty environment values";
    }
    EXPECT_THROW((void)import_from_source(), std::runtime_error);
    EXPECT_STREQ(std::getenv(variable), "");
    EXPECT_EQ(
        inspect_workspace_session_database(database()),
        WorkspaceDatabaseState::missing);
}

TEST_F(WorkspaceConfigStoreTest, RestoresEnvironmentWhenValidationThrows) {
    constexpr char inserted[] = "CHA_IMPORT_STORE_TEMP_E5F6";
    ScopedEnvironmentVariable guard(inserted);
    ASSERT_TRUE(unset_environment_variable(inserted));
    write_bytes(source() / ".env", "CHA_IMPORT_STORE_TEMP_E5F6=temporary\n");
    std::ofstream(source() / "workspace.toml") << "not toml";

    EXPECT_THROW((void)import_from_source(), std::runtime_error);
    EXPECT_EQ(std::getenv(inserted), nullptr);
    EXPECT_EQ(
        inspect_workspace_session_database(database()),
        WorkspaceDatabaseState::missing);
}

TEST_F(WorkspaceConfigStoreTest, AcceptsCollectedMarkdownIncludesAndRejectsExcludedText) {
    write_bytes(
        source() / "characters" / "guide" / "shared" / "snippet.md",
        "Included markdown.\n");
    write_bytes(
        source() / "characters" / "guide" / "CHARACTER.md",
        "$$(shared/snippet.md)\nGuide instructions.\n");
    EXPECT_NO_THROW((void)import_from_source());

    std::filesystem::remove(database());
    write_bytes(
        source() / "characters" / "guide" / "shared" / "snippet.txt",
        "Excluded text.\n");
    write_bytes(
        source() / "characters" / "guide" / "CHARACTER.md",
        "$$(shared/snippet.txt)\nGuide instructions.\n");
    EXPECT_THROW((void)import_from_source(), std::runtime_error);
    EXPECT_EQ(
        inspect_workspace_session_database(database()),
        WorkspaceDatabaseState::missing);
}

TEST_F(WorkspaceConfigStoreTest, MalformedInputsLeaveTheTargetUntouched) {
    make_v1_database(database());
    {
        Database handle(database(), Database::Mode::read_write);
        seed_session_rows(handle);
    }

    write_bytes(source() / "app.toml", "host = \n");
    EXPECT_THROW((void)import_from_source(), std::runtime_error);
    EXPECT_EQ(
        inspect_workspace_session_database(database()),
        WorkspaceDatabaseState::valid_v1);

    write_bytes(source() / "app.toml", "host = \"127.0.0.1\"\nport = 8080\n");
    write_bytes(source() / ".env", "not a valid entry\n");
    EXPECT_THROW((void)import_from_source(), std::runtime_error);
    EXPECT_EQ(
        inspect_workspace_session_database(database()),
        WorkspaceDatabaseState::valid_v1);

    std::filesystem::remove(source() / ".env");
    std::ofstream(source() / "workspace.toml") << "[logging]\nlevel = 1\n";
    EXPECT_THROW((void)import_from_source(), std::runtime_error);
    EXPECT_EQ(
        inspect_workspace_session_database(database()),
        WorkspaceDatabaseState::valid_v1);
    {
        Database handle(database(), Database::Mode::read_only);
        expect_seeded_session_rows(handle);
    }
}

TEST_F(WorkspaceConfigStoreTest, V1TransactionFailureLeavesValidV1) {
    make_v1_database(database());
    {
        Database handle(database(), Database::Mode::read_write);
        seed_session_rows(handle);
        handle.execute("CREATE VIEW config AS SELECT 'x' AS name, 'y' AS content");
    }
    EXPECT_THROW((void)import_from_source(), std::runtime_error);
    EXPECT_EQ(
        inspect_workspace_session_database(database()),
        WorkspaceDatabaseState::valid_v1);
    Database handle(database(), Database::Mode::read_only);
    expect_seeded_session_rows(handle);
}

TEST_F(WorkspaceConfigStoreTest, V2TransactionFailureLeavesPriorConfiguration) {
    import_from_source();
    {
        Database handle(database(), Database::Mode::read_write);
        seed_session_rows(handle);
        handle.execute(
            "CREATE TRIGGER abort_config BEFORE INSERT ON config "
            "BEGIN SELECT RAISE(ABORT, 'blocked'); END");
    }
    write_bytes(source() / "app.toml", "host = \"127.0.0.1\"\nport = 9051\n");
    EXPECT_THROW((void)import_from_source(), std::runtime_error);
    Database handle(database(), Database::Mode::read_only);
    expect_seeded_session_rows(handle);
    Statement app = handle.prepare(
        "SELECT content FROM config WHERE name = 'app.toml'");
    ASSERT_TRUE(app.step());
    EXPECT_EQ(app.text(0).find("9051"), std::string::npos);
}

TEST_F(WorkspaceConfigStoreTest, LegacyDetectorUsesTargetExistenceMessages) {
    const std::filesystem::path sessions =
        source() / "forums" / "lobby" / "sessions";
    std::filesystem::create_directories(sessions);
    write_bytes(sessions / "old.sqlite3", "legacy");

    try {
        (void)import_from_source();
        FAIL() << "expected missing-target legacy failure";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("archived"), std::string::npos) << message;
        EXPECT_NE(message.find("chaweb --migration"), std::string::npos)
            << message;
    }
    EXPECT_EQ(
        inspect_workspace_session_database(database()),
        WorkspaceDatabaseState::missing);

    create_empty_workspace_session_database(database());
    try {
        (void)import_from_source();
        FAIL() << "expected present-target legacy failure";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("remain"), std::string::npos) << message;
        EXPECT_NE(message.find("remove the legacy"), std::string::npos)
            << message;
        EXPECT_EQ(message.find("archived"), std::string::npos) << message;
    }
    EXPECT_EQ(
        inspect_workspace_session_database(database()),
        WorkspaceDatabaseState::valid_v2);
    Database handle(database(), Database::Mode::read_only);
    EXPECT_TRUE(read_workspace_config_files(handle).empty());
}

TEST_F(WorkspaceConfigStoreTest, ExportsToMissingAndEmptyDestinations) {
    import_from_source();
    EXPECT_FALSE(std::filesystem::exists(export_));
    const std::size_t first =
        export_workspace_configuration(database(), export_).file_count;
    EXPECT_GT(first, 0U);
    EXPECT_TRUE(std::filesystem::is_regular_file(export_ / "app.toml"));

    std::error_code error;
    std::filesystem::remove_all(export_, error);
    std::filesystem::create_directories(export_);
    const std::size_t second =
        export_workspace_configuration(database(), export_).file_count;
    EXPECT_EQ(first, second);
}

TEST_F(WorkspaceConfigStoreTest, RejectsNonEmptyDestinationAndUnexportableDatabases) {
    import_from_source();
    std::filesystem::create_directories(export_);
    write_bytes(export_ / "keep.txt", "stay\n");
    EXPECT_THROW(
        (void)export_workspace_configuration(database(), export_),
        std::runtime_error);
    EXPECT_EQ(file_bytes(export_ / "keep.txt"), "stay\n");
    EXPECT_FALSE(std::filesystem::exists(export_ / "app.toml"));

    make_v1_database(source() / "v1.sqlite3");
    EXPECT_THROW(
        (void)export_workspace_configuration(source() / "v1.sqlite3", export_ / "out"),
        std::runtime_error);

    const std::filesystem::path foreign = source() / "foreign.sqlite3";
    {
        Database handle(foreign, Database::Mode::read_write_create);
        handle.execute("CREATE TABLE x (y INTEGER)");
    }
    EXPECT_THROW(
        (void)export_workspace_configuration(foreign, export_ / "foreign-out"),
        std::runtime_error);

    write_bytes(source() / "garbage.sqlite3", "not a database");
    EXPECT_THROW(
        (void)export_workspace_configuration(
            source() / "garbage.sqlite3", export_ / "garbage-out"),
        std::runtime_error);
}

TEST_F(WorkspaceConfigStoreTest, ReportsIncompleteExportOnIoFailure) {
    import_from_source();
    {
        Database handle(database(), Database::Mode::read_write);
        const std::string long_name(251, 'a');
        handle.execute(
            "INSERT INTO config (name, content) VALUES ('"
            + long_name + ".toml', 'x')");
    }
    try {
        (void)export_workspace_configuration(database(), export_);
        FAIL() << "expected export I/O failure";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("may be incomplete"), std::string::npos)
            << message;
        EXPECT_NE(message.find("emptied before retrying"), std::string::npos)
            << message;
    }
}

TEST_F(WorkspaceConfigStoreTest, ImportAndExportRejectAHeldLease) {
    {
        SessionLease lease = SessionLease::acquire(database(), "held");
        EXPECT_THROW((void)import_from_source(), SessionBusyError);
        EXPECT_EQ(
            inspect_workspace_session_database(database()),
            WorkspaceDatabaseState::missing);
    }
    import_from_source();
    {
        SessionLease export_lease = SessionLease::acquire(database(), "held");
        EXPECT_THROW(
            (void)export_workspace_configuration(database(), export_),
            SessionBusyError);
    }
}

TEST_F(WorkspaceConfigStoreTest, ExportRejectsCollidingStoredNames) {
    import_from_source();
    {
        Database handle(database(), Database::Mode::read_write);
        handle.execute(
            "INSERT INTO config (name, content) VALUES ('a.toml', 'x')");
        handle.execute(
            "INSERT INTO config (name, content) VALUES ('a.toml/b.md', 'y')");
    }
    EXPECT_THROW(
        (void)export_workspace_configuration(database(), export_),
        std::runtime_error);
    EXPECT_FALSE(std::filesystem::exists(export_ / "a.toml"));
}

void remove_database_bundle(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::remove(path, error);
    for (const char* suffix : {"-wal", "-shm", "-journal"}) {
        std::filesystem::path sidecar = path;
        sidecar += suffix;
        std::filesystem::remove(sidecar, error);
    }
    std::filesystem::remove(SessionLease::companion_path(path), error);
}

std::string stored_config(
    const std::filesystem::path& database,
    std::string_view name) {
    Database handle(database, Database::Mode::read_only);
    Statement statement = handle.prepare(
        "SELECT content FROM config WHERE name = ?1", name);
    if (!statement.step()) return {};
    return statement.text(0);
}

#ifndef _WIN32
void expect_same_directory(
    const std::filesystem::path& path,
    const struct stat& before) {
    struct stat after {};
    ASSERT_EQ(::stat(path.c_str(), &after), 0);
    EXPECT_EQ(before.st_dev, after.st_dev);
    EXPECT_EQ(before.st_ino, after.st_ino);
}
#endif

void expect_session_root_identity(
    const std::shared_ptr<const Workspace>& workspace,
    const std::filesystem::path& workspace_root) {
    ASSERT_TRUE(workspace);
    EXPECT_EQ(workspace->root(), workspace_root);
    EXPECT_NE(workspace->find_forum("lobby"), nullptr);
    EXPECT_NE(workspace->find_character("guide"), nullptr);
    EXPECT_FALSE(workspace->forums().empty());
}

class RuntimeWorkspaceConfigStoreTest : public testing::Test {
protected:
    static constexpr char dotenv_variable[] = "CHA_RUNTIME_STORE_DOTENV_B4A1";

    void SetUp() override {
        ASSERT_TRUE(unset_environment_variable(dotenv_variable));
        workspace_.write_provider(
            "second",
            "host = \"test\"\nport = 2\nmode = \"test\"\nmodel = \"second\"\n");
        workspace_.write_style("mono", "font = \"mono\"\n");
        workspace_.add_character("writer", "Writer");
        write_bytes(
            source() / "forums" / "lobby" / "members" / "writer"
                / "character.toml",
            "# forum membership\n");
        write_bytes(
            source() / "app.toml", "host = \"127.0.0.1\"\nport = 8080\n");
        write_bytes(source() / ".env", "CHA_RUNTIME_STORE_DOTENV_B4A1=from-file\n");
        (void)import_workspace_configuration(source(), database());
        export_ = source().parent_path()
            / (source().filename().string() + "_runtime_export");
    }

    void TearDown() override {
        force_next_workspace_config_fault(WorkspaceConfigFault::none);
        std::error_code error;
        std::filesystem::remove_all(export_, error);
        remove_database_bundle(database());
    }

    const std::filesystem::path& source() const { return workspace_.root(); }
    std::filesystem::path database() const {
        return source().parent_path()
            / (source().filename().string() + "_runtime.sqlite3");
    }

    std::unique_ptr<WorkspaceConfigStore> open_store() {
        return WorkspaceConfigStore::open(database());
    }

    ScopedEnvironmentVariable dotenv_guard_{dotenv_variable};
    test::TestWorkspace workspace_;
    std::filesystem::path export_;
};

TEST_F(RuntimeWorkspaceConfigStoreTest, OpensOneOwnerOnlyRootWithChildren) {
    std::filesystem::path root;
    std::filesystem::path workspace_child;
    std::filesystem::path welcome_child;
    {
        const auto store = open_store();
        root = store->private_root();
        workspace_child = store->workspace_path();
        welcome_child = store->welcome_path();
        EXPECT_EQ(workspace_child, root / "workspace");
        EXPECT_EQ(welcome_child, root / "welcome");
        EXPECT_TRUE(std::filesystem::is_directory(workspace_child));
        EXPECT_TRUE(std::filesystem::is_directory(welcome_child));
        std::size_t children = 0;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(root)) {
            (void)entry;
            ++children;
        }
        EXPECT_EQ(children, 2U);
#ifndef _WIN32
        EXPECT_EQ(posix_mode(root), static_cast<mode_t>(0700));
        EXPECT_EQ(posix_mode(workspace_child), static_cast<mode_t>(0700));
        EXPECT_EQ(posix_mode(welcome_child), static_cast<mode_t>(0700));
        const std::filesystem::path wal = database();
        std::filesystem::path wal_file = wal;
        wal_file += "-wal";
        std::filesystem::path shm_file = wal;
        shm_file += "-shm";
        if (std::filesystem::exists(wal_file)) {
            EXPECT_EQ(posix_mode(wal_file), static_cast<mode_t>(0600));
        }
        if (std::filesystem::exists(shm_file)) {
            EXPECT_EQ(posix_mode(shm_file), static_cast<mode_t>(0600));
        }
#endif
        EXPECT_EQ(store->host(), "127.0.0.1");
        EXPECT_EQ(store->port(), 8080);
        EXPECT_STREQ(std::getenv(dotenv_variable), "from-file");
        EXPECT_TRUE(
            std::filesystem::is_directory(
                workspace_child / "system" / "providers"));
        const std::shared_ptr<const Workspace> published = getws();
        expect_session_root_identity(published, workspace_child);
        EXPECT_EQ(
            published->settings().log_file,
            store->database_parent() / "logs" / "cha.log");
        EXPECT_NE(
            published->settings().log_file,
            workspace_child / "logs" / "cha.log");
    }
    EXPECT_FALSE(std::filesystem::exists(root));
    EXPECT_FALSE(std::filesystem::exists(workspace_child));
    EXPECT_FALSE(std::filesystem::exists(welcome_child));
}

TEST_F(RuntimeWorkspaceConfigStoreTest, IgnoresAnOrphanedPriorTemporaryTree) {
    const std::filesystem::path orphan =
        std::filesystem::temp_directory_path() / "cha-runtime-orphan-block4";
    std::filesystem::create_directories(orphan / "workspace");
    write_bytes(orphan / "workspace" / "app.toml", "junk = true\n");
    std::filesystem::path used;
    {
        const auto store = open_store();
        used = store->private_root();
        EXPECT_NE(used, orphan);
        EXPECT_TRUE(std::filesystem::exists(orphan / "workspace" / "app.toml"));
        EXPECT_EQ(getws()->find_character("guide")->provider_id, "test");
    }
    EXPECT_FALSE(std::filesystem::exists(used));
    EXPECT_TRUE(std::filesystem::exists(orphan));
    std::error_code error;
    std::filesystem::remove_all(orphan, error);
}

TEST_F(RuntimeWorkspaceConfigStoreTest, HoldsTheLeaseAgainstRuntimeImportAndExport) {
    const auto store = open_store();
    EXPECT_THROW(
        (void)WorkspaceConfigStore::open(database()), SessionBusyError);
    EXPECT_THROW(
        (void)import_workspace_configuration(source(), database()),
        SessionBusyError);
    EXPECT_THROW(
        (void)export_workspace_configuration(database(), export_),
        SessionBusyError);
}

TEST_F(RuntimeWorkspaceConfigStoreTest, InheritedEnvironmentValuesWinAtStartup) {
    ASSERT_TRUE(set_environment_variable(dotenv_variable, "from-process"));
    const auto store = open_store();
    EXPECT_STREQ(std::getenv(dotenv_variable), "from-process");
    EXPECT_NE(getws()->find_character("guide"), nullptr);
    EXPECT_EQ(store->host(), "127.0.0.1");
}

TEST_F(RuntimeWorkspaceConfigStoreTest, StartsAfterDeletingTheOriginalImportTree) {
    std::error_code error;
    std::filesystem::remove_all(source(), error);
    ASSERT_TRUE(std::filesystem::exists(database()));
    const auto store = open_store();
    EXPECT_NE(getws()->find_character("guide"), nullptr);
    EXPECT_EQ(store->host(), "127.0.0.1");
    EXPECT_EQ(
        getws()->settings().log_file,
        store->database_parent() / "logs" / "cha.log");
}

TEST_F(RuntimeWorkspaceConfigStoreTest, RejectsMissingV1AndForeignDatabases) {
    remove_database_bundle(database());
    try {
        (void)WorkspaceConfigStore::open(database());
        FAIL() << "expected missing database to fail";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("does not exist"), std::string::npos) << message;
        EXPECT_NE(message.find("--import"), std::string::npos) << message;
    }

    make_v1_database(database());
    try {
        (void)WorkspaceConfigStore::open(database());
        FAIL() << "expected version-1 database to fail";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("schema-1"), std::string::npos) << message;
        EXPECT_NE(message.find("--import"), std::string::npos) << message;
    }
    remove_database_bundle(database());

    {
        Database handle(database(), Database::Mode::read_write_create);
        handle.execute("CREATE TABLE x (y INTEGER)");
    }
    EXPECT_THROW(
        (void)WorkspaceConfigStore::open(database()), std::runtime_error);
    remove_database_bundle(database());

    write_bytes(database(), "not a database");
    EXPECT_THROW(
        (void)WorkspaceConfigStore::open(database()), std::runtime_error);
}

TEST_F(RuntimeWorkspaceConfigStoreTest, SuccessfulEditUpdatesFilesDatabaseAndWorkspace) {
    const auto store = open_store();
    const std::filesystem::path character =
        store->workspace_path() / "characters" / "guide" / "character.toml";
    const std::filesystem::path forum =
        store->workspace_path() / "forums" / "lobby" / "config.toml";
    const std::string dotenv_before =
        std::getenv(dotenv_variable) == nullptr
            ? std::string()
            : std::getenv(dotenv_variable);

    const WorkspaceConfigEditResult character_result =
        store->apply_character_settings(
            "guide", "second", std::string_view{"mono"});
    EXPECT_NE(
        std::find(
            character_result.affected_forum_ids.begin(),
            character_result.affected_forum_ids.end(),
            "lobby"),
        character_result.affected_forum_ids.end());
    const std::shared_ptr<const Workspace> after_character = getws();
    EXPECT_EQ(after_character->find_character("guide")->provider_id, "second");
    EXPECT_EQ(after_character->find_character("guide")->style_id, "mono");
    EXPECT_NE(file_bytes(character).find("second"), std::string::npos);
    EXPECT_NE(
        stored_config(database(), "characters/guide/character.toml").find("second"),
        std::string::npos);

    const WorkspaceConfigEditResult forum_result =
        store->apply_forum_default_character("lobby", "writer");
    EXPECT_EQ(forum_result.affected_forum_ids, std::vector<std::string>{"lobby"});
    EXPECT_EQ(getws()->find_forum("lobby")->default_character_id, "writer");

    const WorkspaceConfigEditResult persona_result =
        store->apply_forum_default_persona("lobby", "reader");
    EXPECT_EQ(persona_result.affected_forum_ids, std::vector<std::string>{"lobby"});
    EXPECT_EQ(getws()->find_forum("lobby")->default_persona_id, "reader");
    EXPECT_NE(file_bytes(forum).find("reader"), std::string::npos);
    EXPECT_EQ(store->host(), "127.0.0.1");
    EXPECT_EQ(store->port(), 8080);
    EXPECT_EQ(
        std::getenv(dotenv_variable) == nullptr
            ? std::string()
            : std::getenv(dotenv_variable),
        dotenv_before);
}

TEST_F(RuntimeWorkspaceConfigStoreTest, SerializesTwoEditsAndEditReadInteraction) {
    const auto store = open_store();
    const std::filesystem::path workspace_root = store->workspace_path();
    std::atomic<bool> stop{false};
    std::thread reader([&] {
        while (!stop.load()) {
            const std::shared_ptr<const Workspace> workspace = getws();
            expect_session_root_identity(workspace, workspace_root);
            EXPECT_NE(workspace->find_provider("test"), nullptr);
        }
    });
    std::thread first([&] {
        store->apply_character_settings("guide", "second", std::nullopt);
    });
    std::thread second([&] {
        store->apply_forum_default_persona("lobby", "reader");
    });
    first.join();
    second.join();
    stop.store(true);
    reader.join();

    const std::shared_ptr<const Workspace> published = getws();
    EXPECT_EQ(published->find_character("guide")->provider_id, "second");
    EXPECT_EQ(published->find_forum("lobby")->default_persona_id, "reader");
    EXPECT_NE(
        stored_config(database(), "characters/guide/character.toml").find("second"),
        std::string::npos);
    EXPECT_NE(
        stored_config(database(), "forums/lobby/config.toml").find("reader"),
        std::string::npos);
}

void expect_restored_old_configuration(
    const std::filesystem::path& database,
    WorkspaceConfigStore& store,
    const std::shared_ptr<const Workspace>& published) {
    const std::filesystem::path workspace = store.workspace_path();
    expect_session_root_identity(published, workspace);
    EXPECT_EQ(published->find_character("guide")->provider_id, "test");
    EXPECT_EQ(getws()->find_character("guide")->provider_id, "test");
    EXPECT_EQ(getws()->root(), workspace);
    EXPECT_EQ(
        stored_config(database, "characters/guide/character.toml")
            .find("second"),
        std::string::npos);
    EXPECT_EQ(
        file_bytes(workspace / "characters" / "guide" / "character.toml")
            .find("second"),
        std::string::npos);
    EXPECT_NE(
        file_bytes(workspace / "workspace.toml").find("[logging]"),
        std::string::npos);
}

TEST_F(
    RuntimeWorkspaceConfigStoreTest,
    CandidateValidationFailureRestoresTheStableDirectory) {
    const auto store = open_store();
    const std::shared_ptr<const Workspace> published = getws();
    const std::filesystem::path workspace = store->workspace_path();
#ifndef _WIN32
    struct stat before {};
    ASSERT_EQ(::stat(workspace.c_str(), &before), 0);
#endif
    write_bytes(workspace / "workspace.toml", "not toml\n");
    std::atomic<bool> stop{false};
    std::thread reader([&] {
        while (!stop.load()) {
            expect_session_root_identity(published, workspace);
            EXPECT_EQ(published->find_character("guide")->provider_id, "test");
            const std::shared_ptr<const Workspace> current = getws();
            expect_session_root_identity(current, workspace);
        }
    });
    EXPECT_THROW(
        (void)store->apply_character_settings("guide", "second", std::nullopt),
        std::runtime_error);
    stop.store(true);
    reader.join();
#ifndef _WIN32
    expect_same_directory(workspace, before);
#endif
    expect_restored_old_configuration(database(), *store, published);
}

TEST_F(RuntimeWorkspaceConfigStoreTest, ForcedPreCommitFailuresRestoreOldContents) {
    const auto store = open_store();
    const std::shared_ptr<const Workspace> published = getws();
    const std::filesystem::path workspace = store->workspace_path();
    const WorkspaceConfigFault faults[]{
        WorkspaceConfigFault::collect_rows,
        WorkspaceConfigFault::sqlite_begin,
        WorkspaceConfigFault::sqlite_write,
        WorkspaceConfigFault::sqlite_commit,
    };
    for (const WorkspaceConfigFault fault : faults) {
#ifndef _WIN32
        struct stat before {};
        ASSERT_EQ(::stat(workspace.c_str(), &before), 0);
#endif
        force_next_workspace_config_fault(fault);
        EXPECT_THROW(
            (void)store->apply_character_settings(
                "guide", "second", std::nullopt),
            std::runtime_error)
            << static_cast<int>(fault);
#ifndef _WIN32
        expect_same_directory(workspace, before);
#endif
        expect_restored_old_configuration(database(), *store, published);
    }
}

TEST_F(
    RuntimeWorkspaceConfigStoreTest,
    RestorationFailureRequiresRestartAndLeavesCommittedStateOld) {
    const auto store = open_store();
    const std::shared_ptr<const Workspace> published = getws();
    const std::filesystem::path workspace = store->workspace_path();
#ifndef _WIN32
    struct stat before {};
    ASSERT_EQ(::stat(workspace.c_str(), &before), 0);
#endif
    write_bytes(workspace / "workspace.toml", "not toml\n");
    force_next_workspace_config_fault(WorkspaceConfigFault::restore);
    try {
        (void)store->apply_character_settings("guide", "second", std::nullopt);
        FAIL() << "expected restart-required restoration failure";
    } catch (const WorkspaceRestartRequiredError& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("Failed to restore"), std::string::npos)
            << message;
        EXPECT_NE(message.find("Restart is required"), std::string::npos)
            << message;
    }
#ifndef _WIN32
    expect_same_directory(workspace, before);
#endif
    EXPECT_EQ(published->find_character("guide")->provider_id, "test");
    EXPECT_EQ(getws().get(), published.get());
    EXPECT_EQ(
        stored_config(database(), "characters/guide/character.toml")
            .find("second"),
        std::string::npos);
}

TEST_F(
    RuntimeWorkspaceConfigStoreTest,
    PostCommitPublicationFailureRequiresRestartAndSurvivesReopen) {
    {
        const auto store = open_store();
        const std::shared_ptr<const Workspace> published = getws();
        force_next_workspace_config_fault(WorkspaceConfigFault::publication);
        try {
            (void)store->apply_character_settings(
                "guide", "second", std::nullopt);
            FAIL() << "expected restart-required publication failure";
        } catch (const WorkspaceRestartRequiredError& error) {
            const std::string message = error.what();
            EXPECT_NE(message.find("committed"), std::string::npos) << message;
            EXPECT_NE(message.find("Restart is required"), std::string::npos)
                << message;
        }
        EXPECT_EQ(published->find_character("guide")->provider_id, "test");
        EXPECT_EQ(getws().get(), published.get());
        EXPECT_NE(
            stored_config(database(), "characters/guide/character.toml")
                .find("second"),
            std::string::npos);
        EXPECT_NE(
            file_bytes(
                store->workspace_path() / "characters" / "guide"
                / "character.toml")
                .find("second"),
            std::string::npos);
    }

    const auto restarted = open_store();
    EXPECT_EQ(getws()->find_character("guide")->provider_id, "second");
    EXPECT_NE(
        file_bytes(
            restarted->workspace_path() / "characters" / "guide"
            / "character.toml")
            .find("second"),
        std::string::npos);
}

} // namespace
} // namespace cha
