#include "session/session_lease.h"
#include "session/sqlite_storage.h"
#include "session/workspace_session_database.h"
#include "support/test_workspace.h"
#include "util/environment.h"
#include "util/path_name.h"
#include "util/private_filesystem.h"
#include "workspace/workspace.h"
#include "workspace/workspace_config_store.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifndef _WIN32
#include "support/lease_test_process.h"
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
        // Legacy root settings are external application configuration and must
        // be ignored by metadata import.
        write_bytes(
            workspace_.root() / "app.toml",
            "host = \"127.0.0.1\"\nport = 8080\n");
        write_bytes(
            workspace_.root() / "workspace.toml",
            "[logging]\nfile = \"logs/cha.log\"\nlevel = \"off\"\n");
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

TEST_F(WorkspaceConfigStoreTest, ImportsThenReplacesConfigurationWithoutLosingSessions) {
    const std::size_t count = import_from_source();
    EXPECT_GE(count, 6U);
    EXPECT_EQ(
        inspect_workspace_session_database(database()),
        WorkspaceDatabaseState::valid_v2);
    {
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
        EXPECT_FALSE(found_app);
        EXPECT_FALSE(found_workspace);
    }
#ifndef _WIN32
    EXPECT_EQ(posix_mode(database()), static_cast<mode_t>(0600));
#endif

    {
        Database handle(database(), Database::Mode::read_write);
        seed_session_rows(handle);
        storage::SqliteTransaction transaction(handle);
        replace_workspace_config_files(
            handle,
            {{"notes.md", "old\n"}});
        transaction.commit();
    }

    write_bytes(source() / "notes.md", "new\n");
    import_from_source();

    Database handle(database(), Database::Mode::read_only);
    expect_seeded_session_rows(handle);
    bool found_new_notes = false;
    for (const ConfigFile& row : read_workspace_config_files(handle)) {
        if (row.name == "notes.md") {
            EXPECT_EQ(row.content, "new\n");
            found_new_notes = true;
        }
        EXPECT_NE(row.name, "app.toml");
        EXPECT_NE(row.name, "workspace.toml");
    }
    EXPECT_TRUE(found_new_notes);
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

TEST_F(WorkspaceConfigStoreTest, RoundTripsAcceptedFilesByteForByte) {
    workspace_.write_voice(
        "warm-narrator",
        "elevenlabs_voice_id = \"eleven-voice-123\"\nstability = 0.45\n");
    std::string binary{"hi"};
    binary.push_back('\0');
    binary.push_back(static_cast<char>(0xFF));
    binary.push_back('\n');
    write_bytes(source() / "notes.md", binary);
    write_bytes(source() / "empty.md", "");
    write_bytes(source() / "shared" / "nested.toml", "value = 1\n");
    write_bytes(
        source() / path_from_utf8("shared/notes-юникод.md"),
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
        EXPECT_NE(row.name, ".env");
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
    EXPECT_FALSE(std::filesystem::exists(export_ / ".env"));
    EXPECT_FALSE(std::filesystem::exists(export_ / "sessions"));
    EXPECT_TRUE(std::filesystem::is_directory(export_ / "system" / "providers"));
    EXPECT_TRUE(std::filesystem::is_directory(export_ / "system" / "voices"));
    EXPECT_EQ(
        file_bytes(export_ / "system" / "voices" / "warm-narrator" / "config.toml"),
        "elevenlabs_voice_id = \"eleven-voice-123\"\nstability = 0.45\n");
    EXPECT_TRUE(std::filesystem::is_directory(export_ / "personas"));
    EXPECT_TRUE(std::filesystem::is_directory(export_ / "characters"));
    EXPECT_TRUE(std::filesystem::is_directory(export_ / "forums"));
}

TEST_F(WorkspaceConfigStoreTest, LeavesOpenAiAuthFileOutOfImportAndExport) {
    import_from_source();
    std::filesystem::path auth = database();
    auth += ".openai-auth.json";
    const std::string secret =
        R"({"access_token":"store-access-secret","refresh_token":"store-refresh-secret","expires_at":2000000000,"account_id":"acct_store"})";
    create_private_file(auth, secret);
    write_bytes(source() / "notes.openai-auth.json", secret);

    export_workspace_configuration(database(), export_);
    EXPECT_EQ(file_bytes(auth), secret);
    EXPECT_FALSE(std::filesystem::exists(
        export_ / "workspace.sqlite3.openai-auth.json"));
    EXPECT_FALSE(std::filesystem::exists(export_ / "notes.openai-auth.json"));
    EXPECT_EQ(
        file_bytes(database()).find("store-access-secret"), std::string::npos);

    write_bytes(source() / "personas" / "reader" / "PERSONA.md", "updated\n");
    import_from_source();
    EXPECT_EQ(file_bytes(auth), secret);
    EXPECT_EQ(
        file_bytes(database()).find("store-access-secret"), std::string::npos);
}

TEST_F(WorkspaceConfigStoreTest, SynthesizesMissingForumMemberMarker) {
    const std::filesystem::path source_marker =
        source() / "forums" / "lobby" / "members" / "guide"
        / "character.toml";
    ASSERT_TRUE(std::filesystem::remove(source_marker));

    import_from_source();

    EXPECT_FALSE(std::filesystem::exists(source_marker));
    {
        Database handle(database(), Database::Mode::read_only);
        bool found_marker = false;
        for (const ConfigFile& row : read_workspace_config_files(handle)) {
            if (row.name == "forums/lobby/members/guide/character.toml") {
                EXPECT_EQ(row.content, "# Required placeholder\n");
                found_marker = true;
            }
        }
        EXPECT_TRUE(found_marker);
    }

    export_workspace_configuration(database(), export_);
    const std::filesystem::path exported_marker =
        export_ / "forums" / "lobby" / "members" / "guide"
        / "character.toml";
    EXPECT_EQ(file_bytes(exported_marker), "# Required placeholder\n");
    const Workspace exported = Workspace::load(export_);
    EXPECT_NE(exported.find_forum_member("lobby", "guide"), nullptr);
}

TEST_F(WorkspaceConfigStoreTest, IgnoresATopLevelCharacterFileWithoutAnIdDirectory) {
    // A "characters/character.toml" row with no id directory used to
    // underflow a string_view length while pruning orphaned import rows.
    write_bytes(source() / "characters" / "character.toml", "stray = true\n");
    const std::size_t count = import_from_source();
    EXPECT_GE(count, 6U);
    Database handle(database(), Database::Mode::read_only);
    bool found_stray = false;
    for (const ConfigFile& row : read_workspace_config_files(handle)) {
        if (row.name == "characters/character.toml") found_stray = true;
    }
    EXPECT_TRUE(found_stray);
}

TEST_F(WorkspaceConfigStoreTest, DoesNotRequireLegacyRootSettingsFiles) {
    std::filesystem::remove(source() / "app.toml");
    std::filesystem::remove(source() / "workspace.toml");
    EXPECT_NO_THROW((void)import_from_source());
    EXPECT_EQ(
        inspect_workspace_session_database(database()),
        WorkspaceDatabaseState::valid_v2);
}

TEST_F(WorkspaceConfigStoreTest, RejectsUnsafeAndUnsupportedStoredNames) {
#ifdef _WIN32
    EXPECT_THROW(
        validate_stored_config_name("C:host.toml"),
        std::runtime_error);
    EXPECT_THROW(
        validate_stored_config_name("bad\\name.toml"),
        std::runtime_error);
#else
    write_bytes(source() / "C:host.toml", "host = 1\n");
    EXPECT_THROW((void)import_from_source(), std::runtime_error);

    std::filesystem::remove(source() / "C:host.toml");
    write_bytes(source() / "bad\\name.toml", "x = 1\n");
    EXPECT_THROW((void)import_from_source(), std::runtime_error);
    EXPECT_EQ(
        inspect_workspace_session_database(database()),
        WorkspaceDatabaseState::missing);
#endif
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

TEST_F(WorkspaceConfigStoreTest, IgnoresDotenvInsteadOfStoringOrLoadingIt) {
    constexpr char variable[] = "CHA_IMPORT_STORE_IGNORED_CREDENTIAL_A1B2";
    ScopedEnvironmentVariable guard(variable);
    ASSERT_TRUE(unset_environment_variable(variable));
    workspace_.write_provider(
        "secured",
        "host = \"example.test\"\n"
        "port = 443\n"
        "mode = \"net\"\n"
        "model = \"secured\"\n"
        "api_key = \"api_key_1\"\n");
    workspace_.write_character_config(
        "display_name = \"Guide\"\nprovider = \"secured\"\n");

    for (const std::string_view dotenv : {
             "CHA_IMPORT_STORE_IGNORED_CREDENTIAL_A1B2=secret-key\n",
             "not a valid entry\n"}) {
        SCOPED_TRACE(dotenv);
        write_bytes(source() / ".env", dotenv);
        EXPECT_EQ(std::getenv(variable), nullptr);
        EXPECT_NO_THROW((void)import_from_source());
        EXPECT_EQ(std::getenv(variable), nullptr);
        EXPECT_EQ(
            inspect_workspace_session_database(database()),
            WorkspaceDatabaseState::valid_v2);
        Database handle(database(), Database::Mode::read_only);
        for (const ConfigFile& row : read_workspace_config_files(handle)) {
            EXPECT_NE(row.name, ".env");
        }
    }
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

    std::ofstream(source() / "characters" / "guide" / "character.toml")
        << "not toml\n";
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
    write_bytes(source() / "notes.md", "changed\n");
    EXPECT_THROW((void)import_from_source(), std::runtime_error);
    Database handle(database(), Database::Mode::read_only);
    expect_seeded_session_rows(handle);
    Statement notes = handle.prepare(
        "SELECT COUNT(*) FROM config WHERE name = 'notes.md'");
    ASSERT_TRUE(notes.step());
    EXPECT_EQ(notes.integer(0), 0);
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
        EXPECT_NE(message.find("CHA --migration"), std::string::npos)
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
    EXPECT_FALSE(std::filesystem::exists(export_ / "app.toml"));
    EXPECT_FALSE(std::filesystem::exists(export_ / "workspace.toml"));

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

std::map<std::string, std::int64_t> config_rowids(
    const std::filesystem::path& database) {
    Database handle(database, Database::Mode::read_only);
    Statement statement = handle.prepare(
        "SELECT name, rowid FROM config ORDER BY name");
    std::map<std::string, std::int64_t> result;
    while (statement.step()) {
        result.emplace(statement.text(0), statement.integer(1));
    }
    return result;
}

std::map<std::string, std::string> config_contents(
    const std::filesystem::path& database) {
    Database handle(database, Database::Mode::read_only);
    Statement statement = handle.prepare(
        "SELECT name, content FROM config ORDER BY name");
    std::map<std::string, std::string> result;
    while (statement.step()) {
        result.emplace(statement.text(0), statement.text(1));
    }
    return result;
}

std::set<std::string> changed_config_names(
    const std::map<std::string, std::string>& before,
    const std::map<std::string, std::string>& after) {
    std::set<std::string> changed;
    for (const auto& [name, content] : before) {
        const auto found = after.find(name);
        if (found == after.end() || found->second != content) changed.insert(name);
    }
    for (const auto& [name, content] : after) {
        const auto found = before.find(name);
        if (found == before.end() || found->second != content) changed.insert(name);
    }
    return changed;
}

std::int64_t table_row_count(
    const std::filesystem::path& database,
    std::string_view table) {
    Database handle(database, Database::Mode::read_only);
    Statement statement = handle.prepare(
        "SELECT COUNT(*) FROM " + std::string(table));
    EXPECT_TRUE(statement.step());
    return statement.integer(0);
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
        write_bytes(source() / ".env", "CHA_RUNTIME_STORE_DOTENV_B4A1=from-file\n");
        std::filesystem::create_directories(database_directory());
        write_bytes(
            database_directory() / ".env",
            "CHA_RUNTIME_STORE_DOTENV_B4A1=from-file\n");
        (void)import_workspace_configuration(source(), database());
        export_ = source().parent_path()
            / (source().filename().string() + "_runtime_export");
    }

    void TearDown() override {
        force_next_workspace_config_fault(WorkspaceConfigFault::none);
        std::error_code error;
        std::filesystem::remove_all(export_, error);
        remove_database_bundle(database());
        std::filesystem::remove_all(database_directory(), error);
    }

    const std::filesystem::path& source() const { return workspace_.root(); }
    std::filesystem::path database_directory() const {
        return source().parent_path()
            / (source().filename().string() + "_runtime_data");
    }
    std::filesystem::path database() const {
        return database_directory() / "workspace.sqlite3";
    }

    std::unique_ptr<WorkspaceConfigStore> open_store() {
        return WorkspaceConfigStore::open(database());
    }

    ScopedEnvironmentVariable dotenv_guard_{dotenv_variable};
    test::TestWorkspace workspace_;
    std::filesystem::path export_;
};

std::filesystem::path import_source_database(
    const test::TestWorkspace& source,
    std::string_view password = {}) {
    const std::filesystem::path database = source.root() / "source.sqlite3";
    (void)import_workspace_configuration(
        source.root(), database, WorkspaceConfigLease::acquire, password);
    return database;
}

void merge_from(
    WorkspaceConfigStore& store,
    const std::filesystem::path& source_database,
    std::string_view password = {}) {
    const SessionLease lease = SessionLease::acquire(source_database, "test");
    store.merge(source_database, lease, password);
}

constexpr std::string_view merge_model_key_toml =
    "display_name = \"Models\"\n"
    "type = \"models\"\n"
    "value = \"secret\"\n";

constexpr std::string_view merge_r2_key_toml =
    "display_name = \"Storage\"\n"
    "type = \"R2\"\n"
    "url = \"https://r2.example\"\n"
    "access_key_id = \"access\"\n"
    "secret_key = \"storage-secret\"\n";

void write_saved_key(
    const std::filesystem::path& root,
    std::string_view id,
    std::string_view content) {
    write_bytes(
        root / "system" / "keys" / std::string(id) / "config.toml", content);
}

void write_key_next_id(
    const std::filesystem::path& root,
    std::uint64_t next_id) {
    write_bytes(
        root / "system" / "keys" / "config.toml",
        "next_id = " + std::to_string(next_id) + "\n");
}

TEST_F(RuntimeWorkspaceConfigStoreTest, OpensOneOwnerOnlyRootWithChildren) {
    std::error_code error;
    std::filesystem::remove_all(source(), error);
    ASSERT_FALSE(error);
    ASSERT_TRUE(std::filesystem::exists(database()));

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
        EXPECT_EQ(std::getenv(dotenv_variable), nullptr);
        EXPECT_FALSE(std::filesystem::exists(workspace_child / ".env"));
        EXPECT_TRUE(
            std::filesystem::is_directory(
                workspace_child / "system" / "providers"));
        EXPECT_NE(getws()->find_character("guide"), nullptr);
        const std::shared_ptr<const Workspace> published = getws();
        expect_session_root_identity(published, workspace_child);
    }
    EXPECT_FALSE(std::filesystem::exists(root));
    EXPECT_FALSE(std::filesystem::exists(workspace_child));
    EXPECT_FALSE(std::filesystem::exists(welcome_child));
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

TEST_F(RuntimeWorkspaceConfigStoreTest, ClosesSqliteButKeepsLeaseForMaintenance) {
    const auto store = open_store();
    const std::filesystem::path workspace_path = store->workspace_path();
    {
        auto maintenance = store->reserve_maintenance();
        maintenance.close();
        EXPECT_THROW(
            (void)SessionLease::acquire(
                database(), "maintenance keeps the runtime lease"),
            SessionBusyError);
        maintenance.reopen();
    }

    EXPECT_EQ(store->workspace_path(), workspace_path);
    expect_session_root_identity(getws(), workspace_path);
    EXPECT_THROW(
        (void)WorkspaceConfigStore::open(database()), SessionBusyError);
}

TEST_F(RuntimeWorkspaceConfigStoreTest, InheritedEnvironmentValuesWinAtStartup) {
    ASSERT_TRUE(set_environment_variable(dotenv_variable, "from-process"));
    const auto store = open_store();
    EXPECT_STREQ(std::getenv(dotenv_variable), "from-process");
    EXPECT_NE(getws()->find_character("guide"), nullptr);
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
            "guide", "second", std::string_view{"mono"},
            std::nullopt, std::string_view{"high"}, WebSearchMode::automatic);
    EXPECT_NE(
        std::find(
            character_result.affected_forum_ids.begin(),
            character_result.affected_forum_ids.end(),
            "lobby"),
        character_result.affected_forum_ids.end());
    const std::shared_ptr<const Workspace> after_character = getws();
    EXPECT_EQ(after_character->find_character("guide")->provider_id, "second");
    EXPECT_EQ(after_character->find_character("guide")->style_id, "mono");
    EXPECT_EQ(
        after_character->find_character("guide")->reasoning_effort,
        "high");
    EXPECT_EQ(
        after_character->find_character("guide")->web_search,
        WebSearchMode::automatic);
    EXPECT_NE(file_bytes(character).find("second"), std::string::npos);
    EXPECT_NE(file_bytes(character).find("reasoning_effort"), std::string::npos);
    EXPECT_NE(file_bytes(character).find("web_search"), std::string::npos);
    EXPECT_NE(
        stored_config(database(), "characters/guide/character.toml").find("second"),
        std::string::npos);

    const WorkspaceConfigEditResult forum_result =
        store->apply_forum_default_character("lobby", "writer");
    EXPECT_EQ(forum_result.affected_forum_ids, std::vector<std::string>{"lobby"});
    EXPECT_EQ(getws()->find_forum("lobby")->default_character_id, "writer");

    const std::vector<std::string> member_ids{"guide", "writer"};
    const WorkspaceConfigEditResult persona_result =
        store->apply_forum_members_and_persona(
            "lobby", member_ids, "reader");
    EXPECT_EQ(persona_result.affected_forum_ids, std::vector<std::string>{"lobby"});
    EXPECT_EQ(getws()->find_forum("lobby")->default_persona_id, "reader");
    EXPECT_NE(file_bytes(forum).find("reader"), std::string::npos);
    EXPECT_EQ(
        std::getenv(dotenv_variable) == nullptr
            ? std::string()
            : std::getenv(dotenv_variable),
        dotenv_before);
}

TEST_F(
    RuntimeWorkspaceConfigStoreTest,
    CharacterDefinitionUpdatesExactlyItsConfigAndMarkdownRows) {
    {
        Database handle(database(), Database::Mode::read_write);
        handle.execute(
            "INSERT INTO config (name, content) VALUES "
            "('system/keys/config.toml', 'next_id = 3\n'), "
            "('system/keys/api_key_1/config.toml', "
            "'display_name = ''Models''\ntype = ''models''\nvalue = ''secret''\n'), "
            "('system/keys/api_key_2/config.toml', "
            "'display_name = ''Storage''\ntype = ''R2''\n"
            "url = ''https://r2.example''\naccess_key_id = ''access''\n"
            "secret_key = ''storage-secret''\n')");
        handle.execute(
            "CREATE TRIGGER reject_unrelated_config_update "
            "BEFORE UPDATE ON config "
            "WHEN OLD.name NOT IN ("
            "'characters/guide/character.toml', "
            "'characters/guide/CHARACTER.md') "
            "BEGIN SELECT RAISE(ABORT, 'unrelated config update'); END");
    }

    const auto store = open_store();
    const auto before = config_contents(database());

    store->apply_character_definition(
        "guide", "Updated Guide", std::string_view{"Updated profile"});

    const auto after = config_contents(database());
    EXPECT_EQ(
        changed_config_names(before, after),
        (std::set<std::string>{
            "characters/guide/CHARACTER.md",
            "characters/guide/character.toml",
        }));
    EXPECT_EQ(
        after.at("system/keys/api_key_1/config.toml"),
        before.at("system/keys/api_key_1/config.toml"));
    EXPECT_EQ(
        after.at("system/keys/api_key_2/config.toml"),
        before.at("system/keys/api_key_2/config.toml"));
}

TEST_F(RuntimeWorkspaceConfigStoreTest, CharacterFileEditsPersistOnlyTheSelectedFile) {
    const auto store = open_store();
    const auto before = config_contents(database());
    // An unused Markdown file is stored verbatim; its template syntax is not evaluated.
    const auto created = store->apply_character_file(
        "guide", "NOTES.md", std::string_view{"$$(unused.md)\n"}, true);
    EXPECT_EQ(created.affected_forum_ids, std::vector<std::string>{"lobby"});
    EXPECT_EQ(changed_config_names(before, config_contents(database())),
        (std::set<std::string>{"characters/guide/NOTES.md"}));
    EXPECT_EQ(getws()->find_character("guide")->markdown_files.at("NOTES.md"), "$$(unused.md)\n");
    const auto snapshot = getws();
    store->apply_character_file("guide", "NOTES.md", std::string_view{"Updated notes\n"});
    EXPECT_EQ(snapshot->find_character("guide")->markdown_files.at("NOTES.md"), "$$(unused.md)\n");
    store->apply_character_file("guide", "NOTES.md", std::nullopt);
    EXPECT_EQ(config_contents(database()), before);
    EXPECT_FALSE(getws()->find_character("guide")->markdown_files.contains("NOTES.md"));
}

TEST_F(RuntimeWorkspaceConfigStoreTest, ForumFilesPersistExpandAndRollBackInvalidChanges) {
    write_bytes(source() / "forums" / "forum-definition.md",
        "$${character.display_name} in $${forum.display_name}.\n");
    (void)import_workspace_configuration(source(), database());
    const auto store = open_store();
    const auto snapshot = getws();
    const auto created = store->apply_forum_file(
        "lobby", "HOUSE-RULES.md", std::string_view{"Local rules.\n"}, true);
    EXPECT_EQ(created.affected_forum_ids, std::vector<std::string>{"lobby"});
    EXPECT_FALSE(snapshot->find_forum("lobby")->markdown_files.contains("HOUSE-RULES.md"));
    store->apply_forum_file("lobby", "FORUM.md",
        std::string_view{"$${FORUM_DEFINITION}\n$$(HOUSE-RULES.md)"});
    EXPECT_NE(getws()->find_forum_member("lobby", "guide")->system_prompt.find("Guide in The Lobby."), std::string::npos);
    const auto before = config_contents(database());
    EXPECT_THROW(store->apply_forum_file("lobby", "FORUM.md", std::nullopt), std::invalid_argument);
    EXPECT_THROW(store->apply_forum_file("lobby", "HOUSE-RULES.md", std::nullopt), WorkspaceConfigValidationError);
    EXPECT_THROW(store->apply_forum_file("lobby", "forum.md", "Changed"), std::out_of_range);
    EXPECT_THROW(store->apply_forum_file("lobby", "../outside.md", "Changed", true), std::invalid_argument);
    EXPECT_THROW(store->apply_forum_file("lobby", "HOUSE-RULES.md", "Duplicate", true), std::invalid_argument);
    EXPECT_EQ(config_contents(database()), before);
    store->apply_forum_file("lobby", "NOTES.md", std::string_view{"$$(unused.md)"}, true);
    EXPECT_EQ(getws()->find_forum("lobby")->markdown_files.at("NOTES.md"), "$$(unused.md)");
    store->apply_forum_file("lobby", "FORUM.md", std::string_view{"Plain text"});
    store->apply_forum_file("lobby", "HOUSE-RULES.md", std::nullopt);
    EXPECT_FALSE(getws()->find_forum("lobby")->markdown_files.contains("HOUSE-RULES.md"));
    EXPECT_EQ(config_contents(database()).at("forums/lobby/FORUM.md"), "Plain text");
}

TEST_F(
    RuntimeWorkspaceConfigStoreTest,
    CreatesAndDeletesOnlyRowsOwnedByEachItemType) {
    const auto store = open_store();
    const auto expect_changes = [&]<typename Change>(
        std::initializer_list<std::string_view> expected,
        Change change) {
        const auto before = config_contents(database());
        change();
        std::set<std::string> expected_names;
        for (const std::string_view name : expected) {
            expected_names.emplace(name);
        }
        EXPECT_EQ(
            changed_config_names(before, config_contents(database())),
            expected_names);
    };

    std::string provider_id;
    expect_changes(
        {"system/providers/provider_1/config.toml"},
        [&] { provider_id = store->create_provider("Fresh"); });
    EXPECT_EQ(provider_id, "provider_1");
    expect_changes(
        {"system/providers/provider_1/config.toml"},
        [&] { store->apply_provider_delete(provider_id); });

    std::string style_id;
    expect_changes(
        {"system/styles/style_1/config.toml"},
        [&] { style_id = store->create_style("Fresh"); });
    EXPECT_EQ(style_id, "style_1");
    expect_changes(
        {"system/styles/style_1/config.toml"},
        [&] { store->apply_style_delete(style_id); });

    std::string voice_id;
    expect_changes(
        {"system/voices/voice_1/config.toml"},
        [&] {
            voice_id = store->create_voice(
                "Fresh", "Fresh voice", "eleven-fresh");
        });
    EXPECT_EQ(voice_id, "voice_1");
    expect_changes(
        {"system/voices/voice_1/config.toml"},
        [&] { store->apply_voice_delete(voice_id); });

    std::string persona_id;
    expect_changes(
        {"personas/persona_1/PERSONA.md", "personas/persona_1/persona.toml"},
        [&] { persona_id = store->create_persona("Fresh"); });
    EXPECT_EQ(persona_id, "persona_1");
    expect_changes(
        {"personas/persona_1/PERSONA.md", "personas/persona_1/persona.toml"},
        [&] { store->apply_persona_delete(persona_id); });

    std::string character_id;
    expect_changes(
        {
            "characters/character-voice.md",
            "characters/character_1/CHARACTER.md",
            "characters/character_1/PROFILE.md",
            "characters/character_1/character.toml",
        },
        [&] {
            character_id = store->create_character(
                "Fresh", "A fresh character");
        });
    EXPECT_EQ(character_id, "character_1");
    expect_changes(
        {
            "characters/character_1/CHARACTER.md",
            "characters/character_1/PROFILE.md",
            "characters/character_1/character.toml",
        },
        [&] { store->apply_character_delete(character_id); });

    std::string forum_id;
    expect_changes(
        {
            "forums/forum_1/FORUM.md",
            "forums/forum_1/config.toml",
            "forums/forum_1/members/builtin-assistant/character.toml",
        },
        [&] { forum_id = store->create_forum("Fresh", "reader"); });
    EXPECT_EQ(forum_id, "forum_1");
    expect_changes(
        {
            "forums/forum_1/FORUM.md",
            "forums/forum_1/config.toml",
            "forums/forum_1/members/builtin-assistant/character.toml",
        },
        [&] { store->apply_forum_delete(forum_id); });

    expect_changes(
        {
            "system/keys/api_key_1/config.toml",
            "system/keys/config.toml",
        },
        [&] { store->apply_api_key_create("api_key_1", "Models", "secret"); });
    expect_changes(
        {"system/keys/api_key_1/config.toml"},
        [&] { store->apply_api_key_delete("api_key_1"); });

    const R2StorageKey storage{
        .id = "api_key_2",
        .display_name = "Storage",
        .url = "https://r2.example",
        .access_key_id = "access",
        .secret_key = "storage-secret",
    };
    expect_changes(
        {
            "system/keys/api_key_2/config.toml",
            "system/keys/config.toml",
        },
        [&] { store->apply_r2_storage_create(storage); });
    expect_changes(
        {"system/keys/api_key_2/config.toml"},
        [&] { store->apply_r2_storage_delete(); });
}

TEST_F(
    RuntimeWorkspaceConfigStoreTest,
    ApiKeyCreateRollsBackKeyWhenCounterInsertFails) {
    {
        Database handle(database(), Database::Mode::read_write);
        handle.execute(
            "CREATE TRIGGER fail_key_counter_insert "
            "BEFORE INSERT ON config "
            "WHEN NEW.name = 'system/keys/config.toml' "
            "BEGIN SELECT RAISE(ABORT, 'forced counter insert failure'); END");
    }
    const auto before = config_contents(database());
    const auto store = open_store();

    EXPECT_THROW(
        store->apply_api_key_create("api_key_1", "Models", "secret"),
        std::runtime_error);

    EXPECT_EQ(config_contents(database()), before);
    EXPECT_EQ(getws()->find_api_key("api_key_1"), nullptr);
    EXPECT_FALSE(std::filesystem::exists(
        store->workspace_path() / "system" / "keys" / "api_key_1"));
}

TEST_F(
    RuntimeWorkspaceConfigStoreTest,
    MultiFileDeleteRollsBackEarlierDeletes) {
    {
        Database handle(database(), Database::Mode::read_write);
        handle.execute(
            "INSERT INTO config (name, content) VALUES "
            "('characters/unused/CHARACTER.md', 'Unused instructions\n'), "
            "('characters/unused/character.toml', "
            "'display_name = ''Unused''\nprovider = ''test''\n')");
        handle.execute(
            "CREATE TRIGGER fail_character_config_delete "
            "BEFORE DELETE ON config "
            "WHEN OLD.name = 'characters/unused/character.toml' "
            "BEGIN SELECT RAISE(ABORT, 'forced config delete failure'); END");
    }
    const auto before = config_contents(database());
    const auto store = open_store();

    EXPECT_THROW(
        (void)store->apply_character_delete("unused"),
        std::runtime_error);

    EXPECT_EQ(config_contents(database()), before);
    EXPECT_NE(getws()->find_character("unused"), nullptr);
    EXPECT_EQ(
        file_bytes(
            store->workspace_path() / "characters" / "unused"
            / "CHARACTER.md"),
        "Unused instructions\n");
}

TEST_F(
    RuntimeWorkspaceConfigStoreTest,
    EmptyEditPublicationFailureRestoresWithoutRequiringRestart) {
    const auto store = open_store();
    store->apply_character_settings(
        "guide", "second", std::string_view{"mono"},
        std::nullopt, std::string_view{"high"}, WebSearchMode::automatic);
    const std::shared_ptr<const Workspace> published = getws();
    const auto committed_rows = config_contents(database());
    const std::filesystem::path character =
        store->workspace_path() / "characters" / "guide" / "character.toml";
    const std::string materialized = file_bytes(character);

    force_next_workspace_config_fault(WorkspaceConfigFault::publication);
    try {
        (void)store->apply_character_settings(
            "guide", "second", std::string_view{"mono"},
            std::nullopt, std::string_view{"high"},
            WebSearchMode::automatic);
        FAIL() << "expected publication failure";
    } catch (const WorkspaceRestartRequiredError& error) {
        FAIL() << "no-op publication failure required restart: " << error.what();
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string_view(error.what()).find("Forced workspace publication"),
            std::string_view::npos);
    }

    EXPECT_EQ(config_contents(database()), committed_rows);
    EXPECT_EQ(file_bytes(character), materialized);
    EXPECT_EQ(getws().get(), published.get());

    store->apply_character_settings("guide", "test", std::nullopt);
    EXPECT_EQ(getws()->find_character("guide")->provider_id, "test");
}

TEST_F(
    RuntimeWorkspaceConfigStoreTest,
    ForumDeleteRemovesOnlyItsConfigAndSessions) {
    {
        Database handle(database(), Database::Mode::read_write);
        seed_session_rows(handle);
    }
    const auto store = open_store();
    const auto before = config_contents(database());
    std::set<std::string> forum_rows;
    for (const auto& [name, content] : before) {
        (void)content;
        if (name.starts_with("forums/lobby/")) forum_rows.insert(name);
    }
    ASSERT_FALSE(forum_rows.empty());

    store->apply_forum_delete("lobby");

    EXPECT_EQ(
        changed_config_names(before, config_contents(database())),
        forum_rows);
    EXPECT_EQ(table_row_count(database(), "forums"), 0);
    EXPECT_EQ(table_row_count(database(), "sessions"), 0);
    EXPECT_EQ(table_row_count(database(), "turns"), 0);
    EXPECT_EQ(table_row_count(database(), "entries"), 0);
    EXPECT_EQ(getws()->find_forum("lobby"), nullptr);
}

TEST_F(
    RuntimeWorkspaceConfigStoreTest,
    ForumDeleteRollsBackConfigAndSessionsWhenForumDeleteFails) {
    {
        Database handle(database(), Database::Mode::read_write);
        seed_session_rows(handle);
        handle.execute(
            "CREATE TRIGGER fail_forum_delete "
            "BEFORE DELETE ON forums "
            "WHEN OLD.forum_id = 'lobby' "
            "BEGIN SELECT RAISE(ABORT, 'forced forum delete failure'); END");
    }
    const auto before = config_contents(database());
    const auto store = open_store();

    EXPECT_THROW(
        (void)store->apply_forum_delete("lobby"),
        std::runtime_error);

    EXPECT_EQ(config_contents(database()), before);
    EXPECT_EQ(table_row_count(database(), "forums"), 1);
    EXPECT_EQ(table_row_count(database(), "sessions"), 2);
    EXPECT_EQ(table_row_count(database(), "turns"), 1);
    EXPECT_EQ(table_row_count(database(), "entries"), 2);
    EXPECT_NE(getws()->find_forum("lobby"), nullptr);
    EXPECT_TRUE(std::filesystem::exists(
        store->workspace_path() / "forums" / "lobby" / "config.toml"));
}

TEST_F(
    RuntimeWorkspaceConfigStoreTest,
    MaterializedWorkspaceCollectsBackToIdenticalRowsWithPlaceholder) {
    std::filesystem::remove(
        source() / "forums" / "lobby" / "members" / "writer"
            / "character.toml");
    (void)import_workspace_configuration(source(), database());
    const auto expected = config_contents(database());
    ASSERT_EQ(
        expected.at("forums/lobby/members/writer/character.toml"),
        "# Required placeholder\n");

    const auto store = open_store();
    std::filesystem::create_directories(export_);
    const std::filesystem::path collected_database =
        export_ / "collected.sqlite3";
    (void)import_workspace_configuration(
        store->workspace_path(), collected_database);

    EXPECT_EQ(config_contents(collected_database), expected);
}

TEST_F(RuntimeWorkspaceConfigStoreTest, PersistsTheVoiceLifecycle) {
    const auto store = open_store();
    const std::string voice_id = store->create_voice(
        "Brian", "Deep, resonant, comforting", "brian-id");
    EXPECT_EQ(voice_id, "voice_1");
    const WorkspaceVoice* voice = getws()->find_voice(voice_id);
    ASSERT_NE(voice, nullptr);
    EXPECT_EQ(voice->description, "Deep, resonant, comforting");
    EXPECT_NE(
        stored_config(database(), "system/voices/voice_1/config.toml")
            .find("brian-id"),
        std::string::npos);

    store->apply_character_settings(
        "guide", "test", std::nullopt, std::string_view{voice_id});
    const WorkspaceConfigEditResult updated = store->apply_voice_update(
        voice_id, "George", "Warm, captivating storyteller", "george-id",
        VoiceSettings{.speed = 0.9});
    EXPECT_EQ(updated.affected_forum_ids, std::vector<std::string>{"lobby"});
    voice = getws()->find_voice(voice_id);
    ASSERT_NE(voice, nullptr);
    EXPECT_EQ(voice->label, "George");
    EXPECT_EQ(voice->settings.speed, 0.9);

    store->apply_character_settings(
        "guide", "test", std::nullopt, std::nullopt);
    store->apply_voice_delete(voice_id);
    EXPECT_EQ(getws()->find_voice(voice_id), nullptr);
}

TEST_F(RuntimeWorkspaceConfigStoreTest, PersistsVoiceOutputSettings) {
    const auto store = open_store();
    const std::string voice_id = store->create_voice(
        "Default Reader", "", "eleven-default");
    store->apply_voice_output_update({
        .url = "https://api.fish.audio/v1/tts",
        .model = "s2.1-pro",
        .api_key_id = "api_key_8",
        .output_format = "mp3",
        .default_voice = "Default Reader",
    });

    ASSERT_TRUE(getws()->voice_output());
    EXPECT_EQ(getws()->voice_output()->model, "s2.1-pro");
    EXPECT_EQ(getws()->voice_output()->api_key_id, "api_key_8");
    EXPECT_EQ(getws()->voice_output()->output_format, "mp3");
    EXPECT_EQ(getws()->voice_output()->default_voice, "Default Reader");
    const std::string stored = stored_config(
        database(), "system/voice-output/config.toml");
    EXPECT_NE(stored.find("api.fish.audio"), std::string::npos);
    EXPECT_NE(stored.find("s2.1-pro"), std::string::npos);
    EXPECT_NE(stored.find("api_key_8"), std::string::npos);
    EXPECT_NE(stored.find("mp3"), std::string::npos);
    EXPECT_NE(stored.find("Default Reader"), std::string::npos);

    store->apply_voice_update(
        voice_id,
        "Renamed Reader",
        "",
        "eleven-default",
        {});
    ASSERT_TRUE(getws()->voice_output());
    EXPECT_EQ(getws()->voice_output()->default_voice, "Renamed Reader");
    EXPECT_THROW(
        store->apply_voice_delete(voice_id),
        std::invalid_argument);
}

TEST_F(RuntimeWorkspaceConfigStoreTest, RejectsInvalidUnusedProviderUpdates) {
    const auto store = open_store();
    const auto published = getws();
    const auto before = config_contents(database());
    const auto path = store->workspace_path()
        / "system" / "providers" / "second" / "config.toml";
    const std::string file_before = file_bytes(path);
    const ModelBackendConfig original = published->find_provider("second")->config;
    for (const std::string_view invalid : {"host", "model", "timeout"}) {
        SCOPED_TRACE(invalid);
        ModelBackendConfig config = original;
        if (invalid == "host") config.host.clear();
        if (invalid == "model") config.model.clear();
        if (invalid == "timeout") config.timeout_s = 0;
        EXPECT_THROW(
            store->apply_provider_update("second", "Second", config),
            std::invalid_argument);
        EXPECT_EQ(config_contents(database()), before);
        EXPECT_EQ(file_bytes(path), file_before);
        EXPECT_EQ(getws(), published);
        ASSERT_NE(getws()->find_provider("second"), nullptr);
        EXPECT_EQ(getws()->find_provider("second")->config.model, original.model);
    }
}

TEST_F(RuntimeWorkspaceConfigStoreTest, ConcurrentCreatesAllocateDistinctIds) {
    const auto store = open_store();
    std::string first_id;
    std::string second_id;
    std::exception_ptr first_error;
    std::exception_ptr second_error;
    std::thread first([&] {
        try {
            first_id = store->create_persona("First Reader");
        } catch (...) {
            first_error = std::current_exception();
        }
    });
    std::thread second([&] {
        try {
            second_id = store->create_persona("Second Reader");
        } catch (...) {
            second_error = std::current_exception();
        }
    });
    first.join();
    second.join();

    ASSERT_FALSE(first_error);
    ASSERT_FALSE(second_error);
    EXPECT_NE(first_id, second_id);
    EXPECT_EQ(
        (std::set<std::string>{first_id, second_id}),
        (std::set<std::string>{"persona_1", "persona_2"}));
    const auto published = getws();
    const WorkspacePersona* first_persona = published->find_persona(first_id);
    const WorkspacePersona* second_persona = published->find_persona(second_id);
    ASSERT_NE(first_persona, nullptr);
    ASSERT_NE(second_persona, nullptr);
    EXPECT_EQ(first_persona->display_name, "First Reader");
    EXPECT_EQ(second_persona->display_name, "Second Reader");
    EXPECT_NE(
        stored_config(database(), "personas/" + first_id + "/persona.toml")
            .find("First Reader"),
        std::string::npos);
    EXPECT_NE(
        stored_config(database(), "personas/" + second_id + "/persona.toml")
            .find("Second Reader"),
        std::string::npos);
}

TEST_F(RuntimeWorkspaceConfigStoreTest, CreationSkipsOccupiedPersonaContainer) {
    workspace_.add_persona("persona_1/nested", "Nested Reader", "Nested prompt");
    (void)import_workspace_configuration(source(), database());
    const auto before = config_contents(database());
    const auto store = open_store();
    ASSERT_EQ(getws()->find_persona("persona_1"), nullptr);
    ASSERT_TRUE(std::filesystem::exists(
        store->workspace_path() / "personas" / "persona_1"));

    const std::string id = store->create_persona("New Reader");

    EXPECT_EQ(id, "persona_2");
    const auto published = getws();
    const WorkspacePersona* created = published->find_persona(id);
    const WorkspacePersona* nested = published->find_persona("nested");
    ASSERT_NE(created, nullptr);
    ASSERT_NE(nested, nullptr);
    EXPECT_EQ(created->display_name, "New Reader");
    EXPECT_EQ(nested->display_name, "Nested Reader");
    EXPECT_EQ(nested->prompt, "Nested prompt");
    const auto after = config_contents(database());
    EXPECT_EQ(
        after.at("personas/persona_1/nested/persona.toml"),
        before.at("personas/persona_1/nested/persona.toml"));
    EXPECT_EQ(
        after.at("personas/persona_1/nested/PERSONA.md"),
        before.at("personas/persona_1/nested/PERSONA.md"));
    EXPECT_NE(
        after.at("personas/" + id + "/persona.toml").find("New Reader"),
        std::string::npos);
}

TEST_F(RuntimeWorkspaceConfigStoreTest, SerializesTwoEditsAndEditReadInteraction) {
    const auto store = open_store();
    const std::filesystem::path workspace_root = store->workspace_path();
    const std::vector<std::string> member_ids{"guide", "writer"};
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
        store->apply_forum_members_and_persona(
            "lobby", member_ids, "reader");
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
        file_bytes(workspace / "characters" / "guide" / "character.toml")
            .find("provider = \"test\""),
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
    write_bytes(
        workspace / "system" / "providers" / "test" / "config.toml",
        "not toml\n");
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
    MidChangeSqliteFailureRollsBackEarlierRowUpdates) {
    const std::string original_config = stored_config(
        database(), "characters/guide/character.toml");
    const std::string original_markdown = stored_config(
        database(), "characters/guide/CHARACTER.md");
    {
        Database handle(database(), Database::Mode::read_write);
        handle.execute(
            "CREATE TRIGGER fail_character_config_update "
            "BEFORE UPDATE ON config "
            "WHEN OLD.name = 'characters/guide/character.toml' "
            "BEGIN SELECT RAISE(ABORT, 'forced config update failure'); END");
    }

    const auto store = open_store();
    EXPECT_THROW(
        (void)store->apply_character_definition(
            "guide", "Updated Guide", std::string_view{"Updated profile"}),
        std::runtime_error);

    EXPECT_EQ(
        stored_config(database(), "characters/guide/character.toml"),
        original_config);
    EXPECT_EQ(
        stored_config(database(), "characters/guide/CHARACTER.md"),
        original_markdown);
    EXPECT_EQ(
        file_bytes(
            store->workspace_path() / "characters" / "guide"
            / "character.toml"),
        original_config);
    EXPECT_EQ(
        file_bytes(
            store->workspace_path() / "characters" / "guide"
            / "CHARACTER.md"),
        original_markdown);
    EXPECT_EQ(getws()->find_character("guide")->character.display_name, "Guide");
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
    write_bytes(
        workspace / "system" / "providers" / "test" / "config.toml",
        "not toml\n");
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

TEST_F(RuntimeWorkspaceConfigStoreTest, RetargetReopensTheNewDatabaseInTheSameTree) {
    test::TestWorkspace other;
    other.add_persona("beta", "Beta");
    const std::filesystem::path other_database =
        other.root() / "other.sqlite3";
    (void)import_workspace_configuration(other.root(), other_database);

    const auto store = open_store();
    const std::filesystem::path workspace = store->workspace_path();
    const std::filesystem::path welcome = store->welcome_path();
    EXPECT_EQ(getws()->find_persona("beta"), nullptr);

    const std::filesystem::path target = std::filesystem::weakly_canonical(
        std::filesystem::absolute(other_database));
    SessionLease lease = SessionLease::acquire(target, "busy");
    {
        auto maintenance = store->reserve_maintenance();
        maintenance.close();
        maintenance.retarget(target, std::move(lease));
        maintenance.reopen();
    }

    EXPECT_EQ(store->workspace_path(), workspace);
    EXPECT_EQ(store->welcome_path(), welcome);
    EXPECT_EQ(
        store->database_path(),
        std::filesystem::weakly_canonical(
            std::filesystem::absolute(other_database)));
    ASSERT_NE(getws()->find_persona("beta"), nullptr);
    EXPECT_THROW(
        (void)WorkspaceConfigStore::open(other_database), SessionBusyError);
#ifndef _WIN32
    EXPECT_EQ(
        test::probe_lease(database()), test::LeaseProbeResult::acquired);
    EXPECT_EQ(
        test::probe_lease(other_database), test::LeaseProbeResult::busy);
#endif
}

TEST_F(
    RuntimeWorkspaceConfigStoreTest,
    MergeOverlaysExactPathsAndRetainsDestinationOnlyRows) {
    write_bytes(
        source() / "characters" / "mix" / "yoda" / "character.toml",
        "display_name = \"Yoda\"\nprovider = \"test\"\n");
    write_bytes(
        source() / "characters" / "mix" / "yoda" / "CHARACTER.md",
        "Do or do not.\n");
    write_bytes(
        source() / "forums" / "lobby" / "members" / "guide" / "CHARACTER.md",
        "Destination prompt override\n");
    (void)import_workspace_configuration(source(), database());
    {
        Database handle(database(), Database::Mode::read_write);
        seed_session_rows(handle);
    }

    test::TestWorkspace source_workspace;
    source_workspace.add_persona("beta", "Beta");
    source_workspace.add_forum("stoics", "Stoics", "guide");
    write_bytes(
        source_workspace.root() / "characters" / "guide" / "CHARACTER.md",
        "Source guide instructions\n");
    const std::filesystem::path source_database =
        import_source_database(source_workspace);
    {
        Database handle(source_database, Database::Mode::read_write);
        seed_session_rows(handle);
    }
    const auto source_before = config_contents(source_database);

    const auto store = open_store();
    merge_from(*store, source_database);

    EXPECT_EQ(getws()->root(), store->workspace_path());
    ASSERT_NE(getws()->find_persona("beta"), nullptr);
    EXPECT_EQ(
        stored_config(database(), "characters/guide/CHARACTER.md"),
        "Source guide instructions\n");
    EXPECT_NE(getws()->find_character("writer"), nullptr);
    EXPECT_NE(getws()->find_character("yoda"), nullptr);
    EXPECT_EQ(
        stored_config(
            database(), "forums/lobby/members/guide/CHARACTER.md"),
        "Destination prompt override\n");
    EXPECT_FALSE(
        stored_config(
            database(), "forums/lobby/members/character_defaults.toml")
            .empty());
    EXPECT_FALSE(
        stored_config(
            database(), "forums/lobby/members/writer/character.toml")
            .empty());
    EXPECT_NE(getws()->find_forum("stoics"), nullptr);
    EXPECT_NE(getws()->find_forum("lobby"), nullptr);
    {
        Database handle(database(), Database::Mode::read_only);
        expect_seeded_session_rows(handle);
    }
    EXPECT_EQ(config_contents(source_database), source_before);
    {
        Database handle(source_database, Database::Mode::read_only);
        expect_seeded_session_rows(handle);
    }

    {
        Database handle(source_database, Database::Mode::read_write);
        handle.execute("DELETE FROM config WHERE name LIKE 'personas/beta/%'");
        handle.execute("DELETE FROM config WHERE name LIKE 'forums/stoics/%'");
    }
    merge_from(*store, source_database);
    EXPECT_NE(getws()->find_persona("beta"), nullptr);
    EXPECT_NE(getws()->find_forum("stoics"), nullptr);
    EXPECT_EQ(
        stored_config(database(), "characters/guide/CHARACTER.md"),
        "Source guide instructions\n");
    EXPECT_NE(getws()->find_character("writer"), nullptr);
    {
        Database handle(database(), Database::Mode::read_only);
        expect_seeded_session_rows(handle);
    }
}

TEST_F(
    RuntimeWorkspaceConfigStoreTest,
    MergeRejectsCandidateConflictsBeforeCommit) {
    const auto before = config_contents(database());
    const auto store = open_store();
    const std::shared_ptr<const Workspace> published = getws();

    {
        test::TestWorkspace source_workspace;
        std::filesystem::remove_all(
            source_workspace.root() / "characters" / "guide");
        write_bytes(
            source_workspace.root() / "characters" / "other" / "guide"
                / "character.toml",
            "display_name = \"Other Guide\"\nprovider = \"test\"\n");
        write_bytes(
            source_workspace.root() / "characters" / "other" / "guide"
                / "CHARACTER.md",
            "Other guide\n");
        const std::filesystem::path source_database =
            import_source_database(source_workspace);
        EXPECT_THROW(
            merge_from(*store, source_database), std::runtime_error);
    }
    {
        test::TestWorkspace source_workspace;
        std::filesystem::remove_all(
            source_workspace.root() / "personas" / "reader");
        write_bytes(
            source_workspace.root() / "personas" / "extra" / "reader"
                / "persona.toml",
            "display_name = \"Reader\"\n");
        const std::filesystem::path source_database =
            import_source_database(source_workspace);
        EXPECT_THROW(
            merge_from(*store, source_database), std::runtime_error);
    }
    {
        test::TestWorkspace source_workspace;
        source_workspace.add_persona("scribe", "Writer");
        const std::filesystem::path source_database =
            import_source_database(source_workspace);
        EXPECT_THROW(
            merge_from(*store, source_database), std::runtime_error);
    }
    {
        test::TestWorkspace source_workspace;
        write_bytes(
            source_workspace.root() / "characters" / "writer" / "character.toml",
            "display_name = \"Writer\"\n");
        write_bytes(
            source_workspace.root() / "characters" / "writer" / "CHARACTER.md",
            "Writer instructions\n");
        const std::filesystem::path source_database =
            import_source_database(source_workspace);
        EXPECT_THROW(
            merge_from(*store, source_database), std::runtime_error);
    }

    EXPECT_EQ(config_contents(database()), before);
    EXPECT_EQ(getws().get(), published.get());
}

TEST_F(
    RuntimeWorkspaceConfigStoreTest,
    MergeRejectsMissingSourceIncludesDuringStandaloneLoad) {
    write_bytes(
        source() / "characters" / "guide" / "shared.md",
        "Destination include body\n");
    (void)import_workspace_configuration(source(), database());
    const auto before = config_contents(database());
    const auto store = open_store();
    const std::shared_ptr<const Workspace> published = getws();

    test::TestWorkspace source_workspace;
    const std::filesystem::path source_database =
        import_source_database(source_workspace);
    {
        Database handle(source_database, Database::Mode::read_write);
        handle.execute(
            "UPDATE config SET content = '$$(shared.md)\nSource guide\n' "
            "WHERE name = 'characters/guide/CHARACTER.md'");
    }
    EXPECT_THROW(merge_from(*store, source_database), std::runtime_error);
    EXPECT_EQ(config_contents(database()), before);
    EXPECT_EQ(getws().get(), published.get());
}

TEST_F(
    RuntimeWorkspaceConfigStoreTest,
    MergeRejectsMalformedSourceProvidersAndStyles) {
    const auto before = config_contents(database());
    const auto store = open_store();
    const std::shared_ptr<const Workspace> published = getws();

    {
        test::TestWorkspace source_workspace;
        source_workspace.write_provider("broken", "not toml\n");
        const std::filesystem::path source_database =
            import_source_database(source_workspace);
        EXPECT_THROW(
            merge_from(*store, source_database), std::runtime_error);
    }
    {
        test::TestWorkspace source_workspace;
        source_workspace.write_style("broken", "font = \"nope\"\n");
        const std::filesystem::path source_database =
            import_source_database(source_workspace);
        EXPECT_THROW(
            merge_from(*store, source_database), std::runtime_error);
    }

    EXPECT_EQ(config_contents(database()), before);
    EXPECT_EQ(getws().get(), published.get());
}

TEST_F(
    RuntimeWorkspaceConfigStoreTest,
    MergeKeepsMalformedDestinationOnlyProvidersAndStyles) {
    {
        Database handle(database(), Database::Mode::read_write);
        handle.execute(
            "INSERT INTO config (name, content) VALUES "
            "('system/providers/broken/config.toml', 'not toml\n'), "
            "('system/styles/broken/config.toml', 'font = \"nope\"\n')");
    }
    test::TestWorkspace source_workspace;
    source_workspace.add_persona("beta", "Beta");
    const std::filesystem::path source_database =
        import_source_database(source_workspace);
    const auto store = open_store();
    merge_from(*store, source_database);

    EXPECT_EQ(
        stored_config(database(), "system/providers/broken/config.toml"),
        "not toml\n");
    EXPECT_EQ(
        stored_config(database(), "system/styles/broken/config.toml"),
        "font = \"nope\"\n");
    EXPECT_NE(getws()->find_persona("beta"), nullptr);
    EXPECT_EQ(getws()->find_provider("broken"), nullptr);
    EXPECT_EQ(getws()->find_style("broken"), nullptr);
}

TEST_F(RuntimeWorkspaceConfigStoreTest, MergeNormalizesSavedKeyNextId) {
    {
        Database handle(database(), Database::Mode::read_write);
        handle.execute(
            "INSERT INTO config (name, content) VALUES "
            "('system/keys/config.toml', 'next_id = 5\n'), "
            "('system/keys/api_key_1/config.toml', "
            "'display_name = \"Dest\"\ntype = \"models\"\nvalue = \"d\"\n')");
    }
    test::TestWorkspace source_workspace;
    write_key_next_id(source_workspace.root(), 4);
    write_saved_key(
        source_workspace.root(), "api_key_3", merge_model_key_toml);
    const std::filesystem::path source_database =
        import_source_database(source_workspace);
    const auto store = open_store();
    merge_from(*store, source_database);

    EXPECT_EQ(
        stored_config(database(), "system/keys/config.toml"),
        "next_id = 5\n");
    EXPECT_NE(getws()->find_api_key("api_key_1"), nullptr);
    EXPECT_NE(getws()->find_api_key("api_key_3"), nullptr);
    EXPECT_EQ(getws()->next_api_key_id(), 5U);
}

TEST_F(
    RuntimeWorkspaceConfigStoreTest,
    MergeRejectsMalformedCountersAndKeyIdExhaustion) {
    const auto before = config_contents(database());
    const auto store = open_store();
    const std::shared_ptr<const Workspace> published = getws();

    {
        test::TestWorkspace source_workspace;
        const std::filesystem::path source_database =
            import_source_database(source_workspace);
        {
            Database handle(source_database, Database::Mode::read_write);
            handle.execute(
                "INSERT INTO config (name, content) VALUES "
                "('system/keys/config.toml', 'next_id = 0\n')");
        }
        EXPECT_THROW(
            merge_from(*store, source_database), std::runtime_error);
    }
    {
        test::TestWorkspace source_workspace;
        const std::filesystem::path source_database =
            import_source_database(source_workspace);
        {
            Database handle(source_database, Database::Mode::read_write);
            handle.execute(
                "INSERT INTO config (name, content) VALUES "
                "('system/keys/api_key_9223372036854775807/config.toml', "
                "'display_name = \"Huge\"\ntype = \"models\"\n"
                "value = \"secret\"\n')");
        }
        EXPECT_THROW(
            merge_from(*store, source_database), std::runtime_error);
    }

    EXPECT_EQ(config_contents(database()), before);
    EXPECT_EQ(getws().get(), published.get());
}

TEST_F(
    RuntimeWorkspaceConfigStoreTest,
    MergeReplacesDestinationR2WithSourceR2AtADifferentId) {
    {
        Database handle(database(), Database::Mode::read_write);
        handle.execute(
            "INSERT INTO config (name, content) VALUES "
            "('system/keys/config.toml', 'next_id = 2\n'), "
            "('system/keys/api_key_1/config.toml', '"
            + std::string(merge_r2_key_toml) + "')");
    }
    test::TestWorkspace source_workspace;
    write_key_next_id(source_workspace.root(), 3);
    write_saved_key(source_workspace.root(), "api_key_2", merge_r2_key_toml);
    const std::filesystem::path source_database =
        import_source_database(source_workspace);
    const auto store = open_store();
    ASSERT_TRUE(getws()->r2_storage());
    EXPECT_EQ(getws()->r2_storage()->id, "api_key_1");

    merge_from(*store, source_database);

    ASSERT_TRUE(getws()->r2_storage());
    EXPECT_EQ(getws()->r2_storage()->id, "api_key_2");
    EXPECT_TRUE(
        stored_config(database(), "system/keys/api_key_1/config.toml").empty());
    EXPECT_EQ(
        stored_config(database(), "system/keys/config.toml"),
        "next_id = 3\n");
}

TEST_F(
    RuntimeWorkspaceConfigStoreTest,
    MergeKeepsDestinationR2WhenSourceHasNone) {
    {
        Database handle(database(), Database::Mode::read_write);
        handle.execute(
            "INSERT INTO config (name, content) VALUES "
            "('system/keys/config.toml', 'next_id = 2\n'), "
            "('system/keys/api_key_1/config.toml', '"
            + std::string(merge_r2_key_toml) + "')");
    }
    test::TestWorkspace source_workspace;
    source_workspace.add_persona("beta", "Beta");
    const std::filesystem::path source_database =
        import_source_database(source_workspace);
    const auto store = open_store();
    merge_from(*store, source_database);

    ASSERT_TRUE(getws()->r2_storage());
    EXPECT_EQ(getws()->r2_storage()->id, "api_key_1");
    EXPECT_NE(getws()->find_persona("beta"), nullptr);
}

TEST_F(
    RuntimeWorkspaceConfigStoreTest,
    MergeReplacesDestinationR2WithSourceModelKeyAtTheSamePath) {
    {
        Database handle(database(), Database::Mode::read_write);
        handle.execute(
            "INSERT INTO config (name, content) VALUES "
            "('system/keys/config.toml', 'next_id = 2\n'), "
            "('system/keys/api_key_1/config.toml', '"
            + std::string(merge_r2_key_toml) + "')");
    }
    test::TestWorkspace source_workspace;
    write_key_next_id(source_workspace.root(), 2);
    write_saved_key(
        source_workspace.root(), "api_key_1", merge_model_key_toml);
    const std::filesystem::path source_database =
        import_source_database(source_workspace);
    const auto store = open_store();
    merge_from(*store, source_database);

    EXPECT_FALSE(getws()->r2_storage());
    ASSERT_NE(getws()->find_api_key("api_key_1"), nullptr);
    EXPECT_EQ(getws()->find_api_key("api_key_1")->value, "secret");
}

TEST_F(
    RuntimeWorkspaceConfigStoreTest,
    MergeRejectsWrongPasswordInvalidSchemaInactiveLeaseAndSelfPath) {
    const auto before = config_contents(database());
    const auto store = open_store();
    const std::shared_ptr<const Workspace> published = getws();

    {
        test::TestWorkspace source_workspace;
        const std::filesystem::path source_database =
            import_source_database(source_workspace, "secret");
        const SessionLease lease =
            SessionLease::acquire(source_database, "test");
        EXPECT_THROW(
            store->merge(source_database, lease, "wrong"),
            std::runtime_error);
    }
    {
        test::TestWorkspace source_workspace;
        const std::filesystem::path source_database =
            source_workspace.root() / "v1.sqlite3";
        make_v1_database(source_database);
        const SessionLease lease =
            SessionLease::acquire(source_database, "test");
        EXPECT_THROW(
            store->merge(source_database, lease), std::runtime_error);
    }
    {
        test::TestWorkspace source_workspace;
        const std::filesystem::path source_database =
            import_source_database(source_workspace);
        SessionLease lease = SessionLease::acquire(source_database, "test");
        SessionLease moved = std::move(lease);
        EXPECT_FALSE(lease.active());
        EXPECT_THROW(
            store->merge(source_database, lease), std::runtime_error);
        (void)moved;
    }
    {
        test::TestWorkspace source_workspace;
        const std::filesystem::path source_database =
            import_source_database(source_workspace);
        const SessionLease lease =
            SessionLease::acquire(source_database, "test");
        EXPECT_THROW(
            store->merge(store->database_path(), lease), std::runtime_error);
    }

    EXPECT_EQ(config_contents(database()), before);
    EXPECT_EQ(getws().get(), published.get());
}

TEST_F(
    RuntimeWorkspaceConfigStoreTest,
    MergeSkipsSqliteWhenRowsAreIdentical) {
    test::TestWorkspace source_workspace;
    const std::filesystem::path source_database =
        import_source_database(source_workspace);
    const auto store = open_store();
    merge_from(*store, source_database);
    const auto rowids_before = config_rowids(database());
    const std::shared_ptr<const Workspace> published = getws();

    force_next_workspace_config_fault(WorkspaceConfigFault::sqlite_begin);
    merge_from(*store, source_database);
    EXPECT_EQ(config_rowids(database()), rowids_before);
    EXPECT_NE(getws().get(), published.get());
    EXPECT_EQ(getws()->root(), store->workspace_path());

    EXPECT_THROW(
        (void)store->apply_character_settings("guide", "second", std::nullopt),
        std::runtime_error);
    EXPECT_EQ(getws()->find_character("guide")->provider_id, "test");
}

TEST_F(
    RuntimeWorkspaceConfigStoreTest,
    MergeSqliteFailuresRestoreDestinationTree) {
    test::TestWorkspace source_workspace;
    source_workspace.add_persona("beta", "Beta");
    const std::filesystem::path source_database =
        import_source_database(source_workspace);
    const auto store = open_store();
    const std::shared_ptr<const Workspace> published = getws();
    const auto before = config_contents(database());
    const std::filesystem::path workspace = store->workspace_path();
    const WorkspaceConfigFault faults[]{
        WorkspaceConfigFault::collect_rows,
        WorkspaceConfigFault::sqlite_begin,
        WorkspaceConfigFault::sqlite_write,
        WorkspaceConfigFault::sqlite_commit,
    };
    for (const WorkspaceConfigFault fault : faults) {
#ifndef _WIN32
        struct stat before_stat {};
        ASSERT_EQ(::stat(workspace.c_str(), &before_stat), 0);
#endif
        force_next_workspace_config_fault(fault);
        EXPECT_THROW(merge_from(*store, source_database), std::runtime_error)
            << static_cast<int>(fault);
#ifndef _WIN32
        expect_same_directory(workspace, before_stat);
#endif
        EXPECT_EQ(config_contents(database()), before);
        EXPECT_EQ(getws().get(), published.get());
        EXPECT_EQ(getws()->find_persona("beta"), nullptr);
        EXPECT_EQ(getws()->root(), workspace);
    }
}

TEST_F(
    RuntimeWorkspaceConfigStoreTest,
    MergeRestorationFailureRequiresRestartWithoutPublishing) {
    test::TestWorkspace source_workspace;
    std::filesystem::remove_all(
        source_workspace.root() / "characters" / "guide");
    write_bytes(
        source_workspace.root() / "characters" / "other" / "guide"
            / "character.toml",
        "display_name = \"Other Guide\"\nprovider = \"test\"\n");
    write_bytes(
        source_workspace.root() / "characters" / "other" / "guide"
            / "CHARACTER.md",
        "Other guide\n");
    const std::filesystem::path source_database =
        import_source_database(source_workspace);
    const auto store = open_store();
    const std::shared_ptr<const Workspace> published = getws();
    const auto before = config_contents(database());
    force_next_workspace_config_fault(WorkspaceConfigFault::restore);
    try {
        merge_from(*store, source_database);
        FAIL() << "expected restart-required restoration failure";
    } catch (const WorkspaceRestartRequiredError& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("Failed to restore"), std::string::npos)
            << message;
        EXPECT_NE(message.find("Restart is required"), std::string::npos)
            << message;
    }
    EXPECT_EQ(config_contents(database()), before);
    EXPECT_EQ(getws().get(), published.get());
}

TEST_F(
    RuntimeWorkspaceConfigStoreTest,
    MergePublicationFailureAfterCommitRequiresRestart) {
    test::TestWorkspace source_workspace;
    source_workspace.add_persona("beta", "Beta");
    const std::filesystem::path source_database =
        import_source_database(source_workspace);
    {
        const auto store = open_store();
        const std::shared_ptr<const Workspace> published = getws();
        force_next_workspace_config_fault(WorkspaceConfigFault::publication);
        try {
            merge_from(*store, source_database);
            FAIL() << "expected restart-required publication failure";
        } catch (const WorkspaceRestartRequiredError& error) {
            const std::string message = error.what();
            EXPECT_NE(message.find("committed"), std::string::npos) << message;
            EXPECT_NE(message.find("Restart is required"), std::string::npos)
                << message;
        }
        EXPECT_EQ(getws().get(), published.get());
        EXPECT_EQ(published->find_persona("beta"), nullptr);
        EXPECT_NE(
            stored_config(database(), "personas/beta/persona.toml").find("Beta"),
            std::string::npos);
        EXPECT_EQ(getws()->root(), store->workspace_path());
    }

    const auto restarted = open_store();
    EXPECT_NE(getws()->find_persona("beta"), nullptr);
    EXPECT_EQ(getws()->root(), restarted->workspace_path());
}

void expect_package_seed_subscription(const ModelBackendConfig& config) {
    EXPECT_EQ(config.auth, ProviderAuth::openai_subscription);
    EXPECT_EQ(config.host, "chatgpt.com");
    EXPECT_EQ(config.port, 443);
    EXPECT_TRUE(config.https);
    EXPECT_EQ(config.base_path, "/backend-api/codex");
    EXPECT_EQ(config.mode, Mode::net);
    EXPECT_EQ(config.api, ProviderApi::responses);
    EXPECT_EQ(config.model, "gpt-5.6-terra");
    EXPECT_TRUE(config.stream);
    EXPECT_TRUE(config.api_key_id.empty());
    EXPECT_FALSE(config.temperature);
    EXPECT_FALSE(config.max_tokens);
    EXPECT_EQ(config.web_search, WebSearchMode::off);
    EXPECT_EQ(config.cache_retention, CacheRetention::off);
}

TEST(WorkspaceConfigStore, ImportsPackageSeedWithoutApiKey) {
    test::TestWorkspace fixture;
    const std::filesystem::path database = fixture.root() / "imported.sqlite3";
    EXPECT_NO_THROW(
        (void)import_workspace_configuration(
            std::filesystem::path(CHA_IMPORT_SEED_DIRECTORY), database));
    EXPECT_EQ(
        inspect_workspace_session_database(database),
        WorkspaceDatabaseState::valid_v2);

    {
        Database imported(database, Database::Mode::read_only);
        const std::vector<ConfigFile> rows =
            read_workspace_config_files(imported);
        const auto voice = std::ranges::find(
            rows, "characters/character-voice.md", &ConfigFile::name);
        ASSERT_NE(voice, rows.end());
        EXPECT_NE(
            voice->content.find("# Character Voice System Prompt"),
            std::string::npos);
        EXPECT_NE(
            voice->content.find("## Response discipline"),
            std::string::npos);
    }

    const auto store = WorkspaceConfigStore::open(database);
    const auto workspace = getws();
    ASSERT_NE(workspace, nullptr);

    const WorkspaceProvider* const chatgpt = workspace->find_provider("chatgpt");
    ASSERT_NE(chatgpt, nullptr);
    expect_package_seed_subscription(chatgpt->config);

    EXPECT_EQ(
        workspace->find_character(workspace_assistant_id)->provider_id,
        "chatgpt");
    EXPECT_EQ(workspace->find_character("epictetus")->provider_id, "chatgpt");
    EXPECT_EQ(
        workspace->find_character("markus_aurelius")->provider_id, "chatgpt");
    EXPECT_EQ(workspace->find_character("seneca")->provider_id, "chatgpt");

    expect_package_seed_subscription(
        workspace->character_definition(
            workspace_entrance_id, workspace_assistant_id)
            .provider.config);
    expect_package_seed_subscription(
        workspace->character_definition("stoics", "epictetus").provider.config);
    expect_package_seed_subscription(
        workspace->character_definition("stoics", "markus_aurelius")
            .provider.config);
    expect_package_seed_subscription(
        workspace->character_definition("stoics", "seneca").provider.config);
    (void)store;
}

} // namespace
} // namespace cha
