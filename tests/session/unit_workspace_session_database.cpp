#include "session/session_storage_layout.h"
#include "session/sqlite_storage.h"
#include "session/workspace_session_database.h"
#include "support/test_workspace.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string_view>

namespace cha {
namespace {

using Database = storage::SqliteDatabase;
using Statement = storage::SqliteStatement;

TEST(WorkspaceSessionDatabase, CreatesValidEmptyDatabaseAndEnablesWal) {
    test::TestWorkspace workspace;
    const std::filesystem::path path =
        workspace_session_database_path(workspace.root());

    create_empty_workspace_session_database(path);
    {
        Database database(path, Database::Mode::read_only);
        EXPECT_NO_THROW(validate_workspace_session_database_identity(database));
        EXPECT_NO_THROW(validate_workspace_session_contents(database));
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
        database.execute("PRAGMA user_version = 2");
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
