#include "session/session_migration.h"

#include "chat/transcript.h"
#include "session/session_database.h"
#include "support/test_transcript.h"
#include "support/test_workspace.h"
#include "workspace/workspace.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cha {
namespace {

class SessionMigrationTest : public testing::Test {
protected:
    std::filesystem::path sessions(std::string_view forum = "lobby") const {
        return fixture_.root() / "forums" / std::string(forum) / "sessions";
    }

    std::filesystem::path create_source(
        std::string_view forum,
        std::string_view id,
        std::string_view label) const {
        const std::filesystem::path path =
            sessions(forum) / (std::string(id) + ".sqlite3");
        std::filesystem::create_directories(path.parent_path());
        if (!create_session_database(path, {
                .id = std::string(id),
                .forum = std::string(forum),
                .label = std::string(label),
            })) {
            throw std::runtime_error("Failed to create legacy test database");
        }
        return path;
    }

    static void execute(
        const std::filesystem::path& path,
        std::string_view sql) {
        sqlite3* database = nullptr;
        ASSERT_EQ(sqlite3_open(path.string().c_str(), &database), SQLITE_OK);
        char* error = nullptr;
        const int result = sqlite3_exec(
            database, std::string(sql).c_str(), nullptr, nullptr, &error);
        const std::string message = error ? error : "";
        sqlite3_free(error);
        sqlite3_close(database);
        ASSERT_EQ(result, SQLITE_OK) << message;
    }

    static std::int64_t integer(
        const std::filesystem::path& path,
        std::string_view sql) {
        sqlite3* database = nullptr;
        if (sqlite3_open_v2(
                path.string().c_str(), &database, SQLITE_OPEN_READONLY, nullptr)
            != SQLITE_OK) {
            throw std::runtime_error("Failed to open test database");
        }
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(
                database, std::string(sql).c_str(), -1, &statement, nullptr)
            != SQLITE_OK
            || sqlite3_step(statement) != SQLITE_ROW) {
            sqlite3_finalize(statement);
            sqlite3_close(database);
            throw std::runtime_error("Failed to query test database");
        }
        const std::int64_t value = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        sqlite3_close(database);
        return value;
    }

    static std::string text(
        const std::filesystem::path& path,
        std::string_view sql) {
        sqlite3* database = nullptr;
        if (sqlite3_open_v2(
                path.string().c_str(), &database, SQLITE_OPEN_READONLY, nullptr)
            != SQLITE_OK) {
            throw std::runtime_error("Failed to open test database");
        }
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(
                database, std::string(sql).c_str(), -1, &statement, nullptr)
            != SQLITE_OK
            || sqlite3_step(statement) != SQLITE_ROW) {
            sqlite3_finalize(statement);
            sqlite3_close(database);
            throw std::runtime_error("Failed to query test database");
        }
        const unsigned char* value = sqlite3_column_text(statement, 0);
        const std::string result = value
            ? reinterpret_cast<const char*>(value) : "";
        sqlite3_finalize(statement);
        sqlite3_close(database);
        return result;
    }

    static std::vector<char> bytes(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return {
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>(),
        };
    }

    test::TestWorkspace fixture_;
};

TEST_F(SessionMigrationTest, CopiesActiveArchivedAndAllHistoryIntoOneDatabase) {
    fixture_.add_forum("second", "Second", "guide");
    const std::filesystem::path active =
        create_source("lobby", "active", "Active session");
    {
        SessionJournal journal(active);
        const TranscriptEntry first = test::human_entry(
            1, {"human", "You"}, {"guide", "Guide"}, "First prompt", 1);
        journal.start_turn(1, first);
        TranscriptEntry response = make_character_entry(
            2, "guide", "Guide", "First response",
            EntryStatus::complete, 1);
        response.created_at = 123;
        journal.complete_turn(1, response);
        journal.clear();
        journal.start_turn(
            2,
            test::human_entry(
                3, {"human", "You"}, {"guide", "Guide"},
                "Interrupted prompt", 2));
    }

    const std::filesystem::path old =
        create_source("second", "archived", "Archived session");
    {
        SessionJournal journal(old);
        journal.start_turn(
            1,
            test::human_entry(
                1, {"human", "You"}, {"guide", "Guide"},
                "Old prompt", 1));
        journal.cancel_turn(1, std::nullopt);
    }
    execute(old, "ALTER TABLE entries DROP COLUMN created_at; PRAGMA user_version = 2");
    const std::filesystem::path archived =
        sessions("second") / "deleted" / old.filename();
    std::filesystem::create_directories(archived.parent_path());
    std::filesystem::rename(old, archived);

    const std::vector<char> active_before = bytes(active);
    const std::vector<char> archived_before = bytes(archived);
    const SessionMigrationSummary summary =
        migrate_sessions(Workspace::load(fixture_.root()));

    EXPECT_EQ(summary.database_path, fixture_.root() / "sessions.sqlite3");
    EXPECT_EQ(summary.forums, 2U);
    EXPECT_EQ(summary.active_sessions, 1U);
    EXPECT_EQ(summary.archived_sessions, 1U);
    EXPECT_EQ(summary.turns, 3U);
    EXPECT_EQ(summary.entries, 4U);
    EXPECT_EQ(integer(summary.database_path, "PRAGMA application_id"), 0x43484157);
    EXPECT_EQ(integer(summary.database_path, "PRAGMA user_version"), 1);
    EXPECT_EQ(integer(summary.database_path,
        "SELECT COUNT(DISTINCT epoch) FROM entries WHERE session_key = "
        "(SELECT session_key FROM sessions WHERE session_id = 'active')"), 2);
    EXPECT_EQ(integer(summary.database_path,
        "SELECT history_epoch FROM sessions WHERE session_id = 'active'"), 2);
    EXPECT_EQ(integer(summary.database_path,
        "SELECT next_entry_id FROM sessions WHERE session_id = 'active'"), 4);
    EXPECT_EQ(integer(summary.database_path,
        "SELECT next_request_id FROM sessions WHERE session_id = 'active'"), 3);
    EXPECT_EQ(integer(summary.database_path,
        "SELECT COUNT(*) FROM turns WHERE state = 0"), 1);
    EXPECT_EQ(integer(summary.database_path,
        "SELECT created_at FROM entries JOIN sessions USING(session_key) "
        "WHERE session_id = 'active' AND entry_id = 2"), 123);
    EXPECT_EQ(integer(summary.database_path,
        "SELECT created_at FROM entries JOIN sessions USING(session_key) "
        "WHERE session_id = 'archived'"), 0);
    EXPECT_EQ(text(summary.database_path,
        "SELECT forum_id FROM forums JOIN sessions USING(forum_key) "
        "WHERE session_id = 'archived'"), "second");
    EXPECT_EQ(integer(summary.database_path,
        "SELECT archived_at IS NOT NULL FROM sessions "
        "WHERE session_id = 'archived'"), 1);

    EXPECT_TRUE(std::filesystem::is_regular_file(active));
    EXPECT_TRUE(std::filesystem::is_regular_file(archived));
    EXPECT_EQ(bytes(active), active_before);
    EXPECT_EQ(bytes(archived), archived_before);
    EXPECT_FALSE(std::filesystem::exists(
        fixture_.root() / ".sessions.sqlite3.migrating"));
}

TEST_F(SessionMigrationTest, RefusesNoSourcesAndDoesNotCreateATarget) {
    EXPECT_THROW(
        (void)migrate_sessions(Workspace::load(fixture_.root())),
        std::runtime_error);
    EXPECT_FALSE(std::filesystem::exists(fixture_.root() / "sessions.sqlite3"));
    EXPECT_FALSE(std::filesystem::exists(
        fixture_.root() / ".sessions.sqlite3.migrating"));
}

TEST_F(SessionMigrationTest, RefusesAnExistingTargetWithoutChangingIt) {
    (void)create_source("lobby", "source", "Source");
    const std::filesystem::path target = fixture_.root() / "sessions.sqlite3";
    std::ofstream(target, std::ios::binary) << "existing";
    const std::vector<char> before = bytes(target);

    EXPECT_THROW(
        (void)migrate_sessions(Workspace::load(fixture_.root())),
        std::runtime_error);
    EXPECT_EQ(bytes(target), before);
}

TEST_F(SessionMigrationTest, RefusesStaleGeneratedOutputWithoutChangingIt) {
    (void)create_source("lobby", "source", "Source");
    const std::filesystem::path temporary =
        fixture_.root() / ".sessions.sqlite3.migrating";
    std::ofstream(temporary, std::ios::binary) << "unfinished";
    const std::vector<char> before = bytes(temporary);

    EXPECT_THROW(
        (void)migrate_sessions(Workspace::load(fixture_.root())),
        std::runtime_error);
    EXPECT_EQ(bytes(temporary), before);
    EXPECT_FALSE(std::filesystem::exists(fixture_.root() / "sessions.sqlite3"));
}

TEST_F(SessionMigrationTest, RejectsDuplicateActiveAndArchivedIdentity) {
    const std::filesystem::path active =
        create_source("lobby", "duplicate", "Duplicate");
    const std::filesystem::path archived =
        sessions() / "deleted" / active.filename();
    std::filesystem::create_directories(archived.parent_path());
    std::filesystem::copy_file(active, archived);

    EXPECT_THROW(
        (void)migrate_sessions(Workspace::load(fixture_.root())),
        std::runtime_error);
    EXPECT_FALSE(std::filesystem::exists(fixture_.root() / "sessions.sqlite3"));
}

TEST_F(SessionMigrationTest, RejectsUnsupportedSourceBeforeCreatingATarget) {
    const std::filesystem::path source =
        create_source("lobby", "future", "Future");
    execute(source, "PRAGMA user_version = 99");

    EXPECT_THROW(
        (void)migrate_sessions(Workspace::load(fixture_.root())),
        std::runtime_error);
    EXPECT_TRUE(std::filesystem::is_regular_file(source));
    EXPECT_FALSE(std::filesystem::exists(fixture_.root() / "sessions.sqlite3"));
    EXPECT_FALSE(std::filesystem::exists(
        fixture_.root() / ".sessions.sqlite3.migrating"));
}

} // namespace
} // namespace cha
