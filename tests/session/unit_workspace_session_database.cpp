#include "session/session_lease.h"
#include "session/session_storage_layout.h"
#include "session/sqlite_storage.h"
#include "session/workspace_session_database.h"
#include "support/test_workspace.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace cha {
namespace {

using Database = storage::SqliteDatabase;
using Statement = storage::SqliteStatement;

void expect_config_rows(
    const std::vector<ConfigFile>& actual,
    const std::vector<ConfigFile>& expected) {
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t i = 0; i < actual.size(); ++i) {
        EXPECT_EQ(actual[i].name, expected[i].name) << i;
        EXPECT_EQ(actual[i].content, expected[i].content) << i;
    }
}

#ifndef _WIN32
mode_t posix_mode(const std::filesystem::path& path) {
    struct stat info {};
    EXPECT_EQ(::lstat(path.c_str(), &info), 0);
    return info.st_mode & 0777;
}
#endif

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
    Statement forums = database.prepare(
        "SELECT forum_id FROM forums ORDER BY forum_key");
    ASSERT_TRUE(forums.step());
    EXPECT_EQ(forums.text(0), "lobby");
    EXPECT_FALSE(forums.step());

    Statement sessions = database.prepare(
        "SELECT session_id, label, updated_at, archived_at, history_epoch, "
        "next_entry_id, next_request_id FROM sessions ORDER BY session_id");
    ASSERT_TRUE(sessions.step());
    EXPECT_EQ(sessions.text(0), "active");
    EXPECT_EQ(sessions.text(1), "Active");
    EXPECT_EQ(sessions.integer(2), 10);
    EXPECT_TRUE(sessions.is_null(3));
    EXPECT_EQ(sessions.integer(4), 1);
    EXPECT_EQ(sessions.integer(5), 3);
    EXPECT_EQ(sessions.integer(6), 2);
    ASSERT_TRUE(sessions.step());
    EXPECT_EQ(sessions.text(0), "old");
    EXPECT_EQ(sessions.text(1), "Archived");
    EXPECT_EQ(sessions.integer(2), 4);
    EXPECT_EQ(sessions.integer(3), 5);
    EXPECT_FALSE(sessions.step());

    Statement turns = database.prepare(
        "SELECT session_key, request_id, epoch, state FROM turns");
    ASSERT_TRUE(turns.step());
    EXPECT_EQ(turns.integer(0), 1);
    EXPECT_EQ(turns.integer(1), 1);
    EXPECT_EQ(turns.integer(2), 1);
    EXPECT_EQ(turns.integer(3), 1);
    EXPECT_FALSE(turns.step());

    Statement entries = database.prepare(
        "SELECT entry_id, text, created_at FROM entries ORDER BY entry_id");
    ASSERT_TRUE(entries.step());
    EXPECT_EQ(entries.integer(0), 1);
    EXPECT_EQ(entries.text(1), "Hello");
    EXPECT_EQ(entries.integer(2), 11);
    ASSERT_TRUE(entries.step());
    EXPECT_EQ(entries.integer(0), 2);
    EXPECT_EQ(entries.text(1), "Hi");
    EXPECT_EQ(entries.integer(2), 12);
    EXPECT_FALSE(entries.step());
}

std::string v1_import_command() {
    return "chaweb --data DATABASE --import WORKSPACE";
}

TEST(WorkspaceSessionDatabase, CreatesValidEmptyDatabaseAndEnablesWal) {
    test::TestWorkspace workspace;
    const std::filesystem::path path =
        workspace_session_database_path(workspace.root());

    create_empty_workspace_session_database(path);
    {
        Database database(path, Database::Mode::read_only);
        EXPECT_NO_THROW(validate_workspace_session_database_identity(database));
        EXPECT_NO_THROW(validate_workspace_session_contents(database));
        EXPECT_EQ(
            database.pragma_integer("application_id"),
            workspace_session_application_id);
        EXPECT_EQ(
            database.pragma_integer("user_version"),
            workspace_session_database_version);
        EXPECT_TRUE(read_workspace_config_files(database).empty());
        Statement journal_mode = database.prepare("PRAGMA journal_mode");
        ASSERT_TRUE(journal_mode.step());
        EXPECT_EQ(journal_mode.text(0), "delete");
    }

    initialize_workspace_session_database_runtime(path);
    Database database(path, Database::Mode::read_only);
    Statement journal_mode = database.prepare("PRAGMA journal_mode");
    ASSERT_TRUE(journal_mode.step());
    EXPECT_EQ(journal_mode.text(0), "wal");
    EXPECT_NO_THROW(checkpoint_workspace_session_database(path));
}

TEST(WorkspaceSessionDatabase, RecoversAnInterruptedFirstCreation) {
    test::TestWorkspace workspace;
    const std::filesystem::path path =
        workspace_session_database_path(workspace.root());
    {
        Database database(path, Database::Mode::read_write_create);
        database.execute("PRAGMA journal_mode = DELETE");
        storage::SqliteTransaction transaction(database);
        create_workspace_session_schema(database);
        set_workspace_session_database_identity(database);
        // A crash rolls this transaction back and leaves an empty file at the
        // final path. Omitting commit reproduces that recovered state.
    }
    ASSERT_TRUE(std::filesystem::is_regular_file(path));

    initialize_workspace_session_database_runtime(path);

    Database database(path, Database::Mode::read_only);
    EXPECT_NO_THROW(validate_workspace_session_database_identity(database));
    EXPECT_NO_THROW(validate_workspace_session_contents(database));
    EXPECT_EQ(
        database.pragma_integer("application_id"),
        workspace_session_application_id);
    Statement journal_mode = database.prepare("PRAGMA journal_mode");
    ASSERT_TRUE(journal_mode.step());
    EXPECT_EQ(journal_mode.text(0), "wal");
}

TEST(WorkspaceSessionDatabase, RejectsABusyCheckpoint) {
    test::TestWorkspace workspace;
    const std::filesystem::path path =
        workspace_session_database_path(workspace.root());
    create_empty_workspace_session_database(path);
    initialize_workspace_session_database_runtime(path);

    Database reader(path, Database::Mode::read_only);
    reader.execute("BEGIN");
    {
        Statement snapshot = reader.prepare("SELECT COUNT(*) FROM forums");
        ASSERT_TRUE(snapshot.step());
    }
    {
        Database writer(path, Database::Mode::read_write);
        writer.execute("INSERT INTO forums (forum_id) VALUES ('new')");
    }

    EXPECT_THROW(
        checkpoint_workspace_session_database(path),
        std::runtime_error);
    std::filesystem::path wal = path;
    wal += "-wal";
    EXPECT_GT(std::filesystem::file_size(wal), 0U);

    reader.execute("ROLLBACK");
    EXPECT_NO_THROW(checkpoint_workspace_session_database(path));
    EXPECT_EQ(std::filesystem::file_size(wal), 0U);
}

TEST(WorkspaceSessionDatabase, DoesNotReplaceAnUnknownNonemptyDatabase) {
    test::TestWorkspace workspace;
    const std::filesystem::path path =
        workspace_session_database_path(workspace.root());
    {
        Database database(path, Database::Mode::read_write_create);
        database.execute("CREATE TABLE foreign_data (value TEXT)");
        database.execute("INSERT INTO foreign_data VALUES ('keep')");
    }

    EXPECT_THROW(
        initialize_workspace_session_database_runtime(path),
        std::runtime_error);

    Database database(path, Database::Mode::read_only);
    Statement retained = database.prepare("SELECT value FROM foreign_data");
    ASSERT_TRUE(retained.step());
    EXPECT_EQ(retained.text(0), "keep");
}

TEST(WorkspaceSessionDatabase, RefusesAnExistingTargetAndUnknownVersion) {
    test::TestWorkspace workspace;
    const std::filesystem::path path =
        workspace_session_database_path(workspace.root());
    create_empty_workspace_session_database(path);

    EXPECT_THROW(
        create_empty_workspace_session_database(path), std::runtime_error);
    {
        Database database(path, Database::Mode::read_write);
        database.execute(
            "PRAGMA user_version = "
            + std::to_string(workspace_session_database_version + 1));
    }
    Database database(path, Database::Mode::read_only);
    EXPECT_THROW(validate_workspace_session_database_identity(database),
        std::runtime_error);
}

TEST(WorkspaceSessionDatabase, ValidationFindsMissingObjectsAndForeignKeys) {
    test::TestWorkspace workspace;
    const std::filesystem::path missing_index = workspace.root() / "missing.sqlite3";
    create_empty_workspace_session_database(missing_index);
    {
        Database database(missing_index, Database::Mode::read_write);
        database.execute("DROP INDEX active_sessions_by_update");
    }
    {
        Database database(missing_index, Database::Mode::read_only);
        EXPECT_THROW(validate_workspace_session_contents(database),
            std::runtime_error);
    }

    const std::filesystem::path broken_foreign_key =
        workspace.root() / "foreign-key.sqlite3";
    create_empty_workspace_session_database(broken_foreign_key);
    {
        Database database(broken_foreign_key, Database::Mode::read_write);
        database.execute("PRAGMA foreign_keys = OFF");
        database.execute(
            "INSERT INTO sessions (forum_key, session_id, label, updated_at, "
            "history_epoch, next_entry_id, next_request_id) "
            "VALUES (99, 'orphan', 'Orphan', 0, 1, 1, 1)");
    }
    Database database(broken_foreign_key, Database::Mode::read_only);
    EXPECT_THROW(validate_workspace_session_contents(database),
        std::runtime_error);
}

TEST(WorkspaceSessionDatabase, ScopesSessionIdentityByForumAndReservesArchives) {
    test::TestWorkspace workspace;
    const std::filesystem::path path =
        workspace_session_database_path(workspace.root());
    create_empty_workspace_session_database(path);
    Database database(path, Database::Mode::read_write);
    database.execute(
        "INSERT INTO forums (forum_id) VALUES ('first'), ('second')");
    database.execute(
        "INSERT INTO sessions (forum_key, session_id, label, updated_at, "
        "archived_at, history_epoch, next_entry_id, next_request_id) "
        "VALUES (1, 'same', 'First', 0, 0, 1, 1, 1), "
        "(2, 'same', 'Second', 0, NULL, 1, 1, 1)");

    EXPECT_THROW(
        database.execute(
            "INSERT INTO sessions (forum_key, session_id, label, updated_at, "
            "history_epoch, next_entry_id, next_request_id) "
            "VALUES (1, 'same', 'Duplicate', 0, 1, 1, 1)"),
        std::runtime_error);
    EXPECT_NO_THROW(validate_workspace_session_contents(database));
}

TEST(WorkspaceSessionDatabase, InspectsMissingValidAndInvalidIdentities) {
    test::TestWorkspace workspace;
    const std::filesystem::path missing = workspace.root() / "missing.sqlite3";
    EXPECT_EQ(
        inspect_workspace_session_database(missing),
        WorkspaceDatabaseState::missing);

    const std::filesystem::path v2 = workspace.root() / "v2.sqlite3";
    create_empty_workspace_session_database(v2);
    EXPECT_EQ(
        inspect_workspace_session_database(v2),
        WorkspaceDatabaseState::valid_v2);

    const std::filesystem::path v1 = workspace.root() / "v1.sqlite3";
    make_v1_database(v1);
    EXPECT_EQ(
        inspect_workspace_session_database(v1),
        WorkspaceDatabaseState::valid_v1);

    const std::filesystem::path foreign = workspace.root() / "foreign.sqlite3";
    {
        Database database(foreign, Database::Mode::read_write_create);
        database.execute("CREATE TABLE foreign_data (value TEXT)");
    }
    EXPECT_EQ(
        inspect_workspace_session_database(foreign),
        WorkspaceDatabaseState::wrong_application_id);

    const std::filesystem::path future = workspace.root() / "future.sqlite3";
    create_empty_workspace_session_database(future);
    {
        Database database(future, Database::Mode::read_write);
        database.execute(
            "PRAGMA user_version = "
            + std::to_string(workspace_session_database_version + 1));
    }
    EXPECT_EQ(
        inspect_workspace_session_database(future),
        WorkspaceDatabaseState::unsupported_version);

    const std::filesystem::path incomplete = workspace.root() / "incomplete.sqlite3";
    create_empty_workspace_session_database(incomplete);
    {
        Database database(incomplete, Database::Mode::read_write);
        database.execute("DROP TABLE config");
    }
    EXPECT_EQ(
        inspect_workspace_session_database(incomplete),
        WorkspaceDatabaseState::corrupt);
}

TEST(WorkspaceSessionDatabase, RejectsMissingAndMalformedV2ConfigObjects) {
    test::TestWorkspace workspace;
    const std::filesystem::path missing_config =
        workspace.root() / "missing-config.sqlite3";
    create_empty_workspace_session_database(missing_config);
    {
        Database database(missing_config, Database::Mode::read_write);
        database.execute("DROP TABLE config");
        EXPECT_THROW(
            validate_workspace_session_contents(database), std::runtime_error);
    }

    const std::filesystem::path extra_column =
        workspace.root() / "extra-column.sqlite3";
    create_empty_workspace_session_database(extra_column);
    {
        Database database(extra_column, Database::Mode::read_write);
        database.execute("DROP TABLE config");
        database.execute(
            "CREATE TABLE config ("
            "name TEXT PRIMARY KEY, content TEXT NOT NULL, extra TEXT"
            ") STRICT");
        EXPECT_THROW(
            validate_workspace_session_contents(database), std::runtime_error);
        EXPECT_EQ(
            inspect_workspace_session_database(extra_column),
            WorkspaceDatabaseState::corrupt);
    }
}

TEST(WorkspaceSessionDatabase, RuntimeInstructsImportForValidV1) {
    test::TestWorkspace workspace;
    const std::filesystem::path path =
        workspace_session_database_path(workspace.root());
    make_v1_database(path);
    {
        Database database(path, Database::Mode::read_write);
        seed_session_rows(database);
    }

    try {
        initialize_workspace_session_database_runtime(path);
        FAIL() << "runtime must not open or upgrade a valid v1 database";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("schema-1"), std::string::npos) << message;
        EXPECT_NE(message.find(v1_import_command()), std::string::npos)
            << message;
        EXPECT_EQ(message.find("unsupported schema"), std::string::npos)
            << message;
        EXPECT_EQ(message.find("corrupt"), std::string::npos) << message;
        EXPECT_EQ(message.find("foreign"), std::string::npos) << message;
    }

    Database database(path, Database::Mode::read_only);
    EXPECT_EQ(
        database.pragma_integer("user_version"),
        workspace_session_database_version_v1);
    expect_seeded_session_rows(database);
    Statement config = database.prepare(
        "SELECT COUNT(*) FROM sqlite_schema WHERE type = 'table' AND name = 'config'");
    ASSERT_TRUE(config.step());
    EXPECT_EQ(config.integer(0), 0);
}

TEST(WorkspaceSessionDatabase, DistinguishesFutureSchemaFromV1ImportInstruction) {
    test::TestWorkspace workspace;
    const std::filesystem::path path =
        workspace_session_database_path(workspace.root());
    create_empty_workspace_session_database(path);
    {
        Database database(path, Database::Mode::read_write);
        database.execute(
            "PRAGMA user_version = "
            + std::to_string(workspace_session_database_version + 1));
    }

    try {
        initialize_workspace_session_database_runtime(path);
        FAIL() << "runtime must refuse an unsupported schema version";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("unsupported schema"), std::string::npos)
            << message;
        EXPECT_EQ(message.find(v1_import_command()), std::string::npos)
            << message;
    }
}

TEST(WorkspaceSessionDatabase, TableCheckRejectsUnsafeConfigNames) {
    test::TestWorkspace workspace;
    const std::filesystem::path path =
        workspace_session_database_path(workspace.root());
    create_empty_workspace_session_database(path);
    Database database(path, Database::Mode::read_write);

    const auto insert = [&](std::string_view sql) {
        database.execute(sql);
    };
    EXPECT_THROW(
        insert("INSERT INTO config (name, content) VALUES ('', 'x')"),
        std::runtime_error);
    EXPECT_THROW(
        insert("INSERT INTO config (name, content) VALUES ('/abs', 'x')"),
        std::runtime_error);
    EXPECT_THROW(
        insert("INSERT INTO config (name, content) VALUES "
               "('a' || char(92) || 'b', 'x')"),
        std::runtime_error);
    EXPECT_THROW(
        insert("INSERT INTO config (name, content) VALUES ('a/../b', 'x')"),
        std::runtime_error);
    EXPECT_NO_THROW(
        insert("INSERT INTO config (name, content) VALUES ('app.toml', 'x')"));

    EXPECT_THROW(validate_stored_config_name(""), std::runtime_error);
    EXPECT_THROW(validate_stored_config_name("/abs"), std::runtime_error);
    EXPECT_THROW(validate_stored_config_name("a\\b"), std::runtime_error);
    EXPECT_THROW(validate_stored_config_name("a/../b"), std::runtime_error);
    EXPECT_THROW(validate_stored_config_name("."), std::runtime_error);
    EXPECT_THROW(validate_stored_config_name("foo//bar"), std::runtime_error);
    EXPECT_THROW(validate_stored_config_name("C:app.toml"), std::runtime_error);
    EXPECT_THROW(validate_stored_config_name("notes.txt"), std::runtime_error);
    EXPECT_THROW(validate_stored_config_name("secret/.env"), std::runtime_error);
    EXPECT_NO_THROW(validate_stored_config_name("app.toml"));
    EXPECT_NO_THROW(validate_stored_config_name(".env"));
    EXPECT_NO_THROW(validate_stored_config_name("characters/guide/CHARACTER.md"));
}

TEST(WorkspaceSessionDatabase, ReadsSortedConfigRowsAndReplacesAtomically) {
    test::TestWorkspace workspace;
    const std::filesystem::path path =
        workspace_session_database_path(workspace.root());
    create_empty_workspace_session_database(path);
    Database database(path, Database::Mode::read_write);
    const std::vector<ConfigFile> initial{
        {"z.toml", "z"},
        {"a.toml", "a"},
        {"m.toml", "m"},
    };
    {
        storage::SqliteTransaction transaction(database);
        replace_workspace_config_files(database, initial);
        transaction.commit();
    }
    expect_config_rows(
        read_workspace_config_files(database),
        {{"a.toml", "a"}, {"m.toml", "m"}, {"z.toml", "z"}});
    EXPECT_EQ(
        database.pragma_integer("user_version"),
        workspace_session_database_version);

    const std::vector<ConfigFile> next{{"only.toml", "kept"}};
    {
        storage::SqliteTransaction transaction(database);
        replace_workspace_config_files(database, next);
    }
    expect_config_rows(
        read_workspace_config_files(database),
        {{"a.toml", "a"}, {"m.toml", "m"}, {"z.toml", "z"}});

    {
        storage::SqliteTransaction transaction(database);
        replace_workspace_config_files(database, next);
        transaction.commit();
    }
    expect_config_rows(read_workspace_config_files(database), next);
    EXPECT_EQ(
        database.pragma_integer("user_version"),
        workspace_session_database_version);
}

TEST(WorkspaceSessionDatabase, RoundTripsExactConfigBytes) {
    test::TestWorkspace workspace;
    const std::filesystem::path path =
        workspace_session_database_path(workspace.root());
    create_empty_workspace_session_database(path);
    Database database(path, Database::Mode::read_write);

    std::string binary{"a"};
    binary.push_back('\0');
    binary.push_back(static_cast<char>(0xFF));
    binary.push_back('\n');
    const std::vector<ConfigFile> rows{
        {".env", ""},
        {"notes.md", binary},
    };
    {
        storage::SqliteTransaction transaction(database);
        replace_workspace_config_files(database, rows);
        transaction.commit();
    }

    const std::vector<ConfigFile> restored = read_workspace_config_files(database);
    expect_config_rows(restored, rows);
    ASSERT_EQ(restored.size(), 2U);
    EXPECT_EQ(restored[0].content.size(), 0U);
    EXPECT_EQ(restored[1].content.size(), 4U);
    EXPECT_EQ(restored[1].content[1], '\0');
    EXPECT_EQ(
        static_cast<unsigned char>(restored[1].content[2]),
        static_cast<unsigned char>(0xFF));
}

TEST(WorkspaceSessionDatabase, UpgradesV1WhilePreservingSessions) {
    test::TestWorkspace workspace;
    const std::filesystem::path path =
        workspace_session_database_path(workspace.root());
    make_v1_database(path);
    Database database(path, Database::Mode::read_write);
    seed_session_rows(database);

    const std::vector<ConfigFile> rows{
        {"app.toml", "host = \"127.0.0.1\"\n"},
        {"workspace.toml", "name = \"Test\"\n"},
    };
    upgrade_workspace_session_database_from_v1(database, rows);

    EXPECT_EQ(
        database.pragma_integer("user_version"),
        workspace_session_database_version);
    EXPECT_EQ(
        inspect_workspace_session_database(path),
        WorkspaceDatabaseState::valid_v2);
    expect_seeded_session_rows(database);
    expect_config_rows(read_workspace_config_files(database), rows);
    EXPECT_NO_THROW(validate_workspace_session_database_identity(database));
    EXPECT_NO_THROW(validate_workspace_session_contents(database));
}

TEST(WorkspaceSessionDatabase, FailedUpgradeLeavesValidV1Unchanged) {
    test::TestWorkspace workspace;
    const std::filesystem::path path =
        workspace_session_database_path(workspace.root());
    make_v1_database(path);
    Database database(path, Database::Mode::read_write);
    seed_session_rows(database);

    sqlite3_commit_hook(
        database.handle(),
        [](void*) -> int { return 1; },
        nullptr);
    EXPECT_THROW(
        upgrade_workspace_session_database_from_v1(
            database,
            {{"app.toml", "host = \"127.0.0.1\"\n"},
             {"workspace.toml", "name = \"Test\"\n"}}),
        std::runtime_error);
    sqlite3_commit_hook(database.handle(), nullptr, nullptr);

    EXPECT_EQ(
        database.pragma_integer("user_version"),
        workspace_session_database_version_v1);
    expect_seeded_session_rows(database);
    Statement config = database.prepare(
        "SELECT COUNT(*) FROM sqlite_schema WHERE type = 'table' AND name = 'config'");
    ASSERT_TRUE(config.step());
    EXPECT_EQ(config.integer(0), 0);
    EXPECT_EQ(
        inspect_workspace_session_database(path),
        WorkspaceDatabaseState::valid_v1);
}

TEST(WorkspaceSessionDatabase, TightensExistingDatabaseAndSidecars) {
    test::TestWorkspace workspace;
    const std::filesystem::path path =
        workspace_session_database_path(workspace.root());
    create_empty_workspace_session_database(path);
    std::filesystem::path journal = path;
    journal += "-journal";
    std::ofstream(journal) << "leftover";
#ifndef _WIN32
    ASSERT_EQ(::chmod(path.c_str(), 0644), 0);
    ASSERT_EQ(::chmod(journal.c_str(), 0644), 0);
    ASSERT_EQ(posix_mode(path), static_cast<mode_t>(0644));
    ASSERT_EQ(posix_mode(journal), static_cast<mode_t>(0644));
#endif
    initialize_workspace_session_database_runtime(path);
#ifndef _WIN32
    EXPECT_EQ(posix_mode(path), static_cast<mode_t>(0600));
    if (std::filesystem::exists(journal)) {
        EXPECT_EQ(posix_mode(journal), static_cast<mode_t>(0600));
    }
#endif

    Database writer(path, Database::Mode::read_write);
    writer.execute("INSERT INTO forums (forum_id) VALUES ('keep-wal')");
    std::filesystem::path wal = path;
    wal += "-wal";
    std::filesystem::path shm = path;
    shm += "-shm";
    ASSERT_TRUE(std::filesystem::is_regular_file(wal));
    ASSERT_TRUE(std::filesystem::is_regular_file(shm));
#ifndef _WIN32
    ASSERT_EQ(::chmod(path.c_str(), 0644), 0);
    ASSERT_EQ(::chmod(wal.c_str(), 0644), 0);
    ASSERT_EQ(::chmod(shm.c_str(), 0644), 0);
    ASSERT_EQ(posix_mode(path), static_cast<mode_t>(0644));
    ASSERT_EQ(posix_mode(wal), static_cast<mode_t>(0644));
    ASSERT_EQ(posix_mode(shm), static_cast<mode_t>(0644));
#endif
    initialize_workspace_session_database_runtime(path);
#ifndef _WIN32
    EXPECT_EQ(posix_mode(path), static_cast<mode_t>(0600));
    EXPECT_EQ(posix_mode(wal), static_cast<mode_t>(0600));
    EXPECT_EQ(posix_mode(shm), static_cast<mode_t>(0600));
#endif
}

TEST(WorkspaceSessionDatabase, CreatesOwnerOnlyDatabaseWalAndShm) {
    test::TestWorkspace workspace;
    const std::filesystem::path path =
        workspace_session_database_path(workspace.root());
    create_empty_workspace_session_database(path);
#ifndef _WIN32
    EXPECT_EQ(posix_mode(path), static_cast<mode_t>(0600));
#endif
    initialize_workspace_session_database_runtime(path);
    Database writer(path, Database::Mode::read_write);
    writer.execute("INSERT INTO forums (forum_id) VALUES ('keep-wal')");
    std::filesystem::path wal = path;
    wal += "-wal";
    std::filesystem::path shm = path;
    shm += "-shm";
    EXPECT_TRUE(std::filesystem::is_regular_file(wal));
    EXPECT_TRUE(std::filesystem::is_regular_file(shm));
#ifndef _WIN32
    EXPECT_EQ(posix_mode(path), static_cast<mode_t>(0600));
    EXPECT_EQ(posix_mode(wal), static_cast<mode_t>(0600));
    EXPECT_EQ(posix_mode(shm), static_cast<mode_t>(0600));
#endif
}

TEST(WorkspaceSessionDatabase, SharedOpenAcquiresLeaseAndCreatesMissingV2) {
    test::TestWorkspace workspace;
    const std::filesystem::path path =
        workspace_session_database_path(workspace.root());
    {
        SessionLease lease = open_workspace_session_database(
            path, "Workspace already in use");
        EXPECT_TRUE(lease.active());
        EXPECT_EQ(
            inspect_workspace_session_database(path),
            WorkspaceDatabaseState::valid_v2);
        EXPECT_THROW(
            (void)open_workspace_session_database(
                path, "Workspace already in use"),
            SessionBusyError);
    }
    EXPECT_NO_THROW(
        (void)open_workspace_session_database(
            path, "Workspace already in use"));
}

TEST(WorkspaceSessionDatabase, RejectsSymlinkDatabasePath) {
    test::TestWorkspace workspace;
    const std::filesystem::path target = workspace.root() / "target.sqlite3";
    create_empty_workspace_session_database(target);
    const std::filesystem::path link = workspace.root() / "link.sqlite3";
    std::error_code error;
    std::filesystem::create_symlink(target, link, error);
    if (error) {
        GTEST_SKIP() << "symbolic links are not available";
    }
    EXPECT_THROW(
        initialize_workspace_session_database_runtime(link),
        std::runtime_error);
    EXPECT_EQ(
        inspect_workspace_session_database(link),
        WorkspaceDatabaseState::corrupt);
#ifndef _WIN32
    EXPECT_EQ(posix_mode(target), static_cast<mode_t>(0600));
#endif
}

TEST(SessionStorageLayout, DetectsOnlyDirectLegacyDatabaseFiles) {
    test::TestWorkspace workspace;
    const std::filesystem::path sessions =
        workspace.root() / "forums" / "lobby" / "sessions";
    const std::filesystem::path deleted = sessions / "deleted";
    std::filesystem::create_directories(deleted / "nested");

    std::ofstream(deleted / "nested" / "nested.sqlite3") << "ignored";
    std::ofstream(sessions / "session.db") << "ignored";
    std::ofstream(sessions / "session.sqlite3-wal") << "ignored";
    std::filesystem::create_directory(sessions / "directory.sqlite3");
    EXPECT_FALSE(has_legacy_session_databases(workspace.root()));

    const std::filesystem::path active = sessions / "active.sqlite3";
    std::ofstream(active) << "legacy";
    EXPECT_TRUE(has_legacy_session_databases(workspace.root()));

    std::filesystem::remove(active);
    const std::filesystem::path archived = deleted / "archived.sqlite3";
    std::ofstream(archived) << "legacy";
    EXPECT_TRUE(has_legacy_session_databases(workspace.root()));
}

TEST(SessionStorageLayout, AcceptsAWorkspaceWithoutLegacyDirectories) {
    test::TestWorkspace workspace;
    EXPECT_EQ(
        workspace_session_database_path(workspace.root()),
        workspace.root() / "workspace.sqlite3");
    EXPECT_FALSE(has_legacy_session_databases(workspace.root()));
}

} // namespace
} // namespace cha
