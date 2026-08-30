#include "session/workspace_session_database.h"

#include "util/path_name.h"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace cha {
namespace {

using Database = storage::SqliteDatabase;
using Statement = storage::SqliteStatement;

std::int64_t scalar(Database& database, std::string_view sql) {
    Statement statement = database.prepare(sql);
    if (!statement.step()) {
        throw std::runtime_error(
            "SQLite query returned no value for '" + database.path() + "'");
    }
    const std::int64_t value = statement.integer(0);
    if (statement.step()) {
        throw std::runtime_error(
            "SQLite query returned more than one value for '"
            + database.path() + "'");
    }
    return value;
}

void validate_required_objects(Database& database) {
    constexpr std::array required{
        std::pair{"table", "forums"},
        std::pair{"table", "sessions"},
        std::pair{"table", "turns"},
        std::pair{"table", "entries"},
        std::pair{"index", "active_sessions_by_update"},
        std::pair{"index", "one_started_turn_per_session"},
        std::pair{"index", "entries_by_session_and_epoch"},
        std::pair{"index", "one_prompt_per_session_turn"},
    };
    for (const auto& [type, name] : required) {
        Statement found = database.prepare(
            "SELECT COUNT(*) FROM sqlite_schema WHERE type = ?1 AND name = ?2",
            std::string_view(type),
            std::string_view(name));
        if (!found.step() || found.integer(0) != 1) {
            throw std::runtime_error(
                "Workspace session database '" + database.path()
                + "' is missing required " + type + " '" + name + "'");
        }
    }
}

void validate_integrity(Database& database) {
    Statement integrity = database.prepare("PRAGMA integrity_check");
    bool saw_row = false;
    while (integrity.step()) {
        saw_row = true;
        const std::string result = integrity.text(0);
        if (result != "ok") {
            throw std::runtime_error(
                "SQLite integrity check failed for '" + database.path()
                + "': " + result);
        }
    }
    if (!saw_row) {
        throw std::runtime_error(
            "SQLite integrity check returned no result for '"
            + database.path() + "'");
    }

    Statement foreign_keys = database.prepare("PRAGMA foreign_key_check");
    if (foreign_keys.step()) {
        throw std::runtime_error(
            "SQLite foreign-key check failed for '" + database.path() + "'");
    }
}

} // namespace

void create_workspace_session_schema(Database& database) {
    database.execute("PRAGMA journal_mode = DELETE");
    database.execute(R"sql(
        CREATE TABLE forums (
            forum_key INTEGER PRIMARY KEY,
            forum_id TEXT NOT NULL UNIQUE CHECK (forum_id <> '')
        ) STRICT;

        CREATE TABLE sessions (
            session_key INTEGER PRIMARY KEY,
            forum_key INTEGER NOT NULL REFERENCES forums(forum_key),
            session_id TEXT NOT NULL CHECK (session_id <> ''),
            label TEXT NOT NULL CHECK (label <> ''),
            updated_at INTEGER NOT NULL CHECK (updated_at >= 0),
            archived_at INTEGER CHECK (archived_at IS NULL OR archived_at >= 0),
            history_epoch INTEGER NOT NULL CHECK (history_epoch > 0),
            next_entry_id INTEGER NOT NULL CHECK (next_entry_id > 0),
            next_request_id INTEGER NOT NULL CHECK (next_request_id > 0),
            UNIQUE (forum_key, session_id)
        ) STRICT;

        CREATE INDEX active_sessions_by_update
            ON sessions (updated_at DESC, forum_key, session_id)
            WHERE archived_at IS NULL;

        CREATE TABLE turns (
            session_key INTEGER NOT NULL
                REFERENCES sessions(session_key) ON DELETE CASCADE,
            request_id INTEGER NOT NULL CHECK (request_id > 0),
            epoch INTEGER NOT NULL CHECK (epoch > 0),
            state INTEGER NOT NULL CHECK (state BETWEEN 0 AND 3),
            PRIMARY KEY (session_key, request_id)
        ) STRICT;

        CREATE UNIQUE INDEX one_started_turn_per_session
            ON turns (session_key)
            WHERE state = 0;

        CREATE TABLE entries (
            session_key INTEGER NOT NULL
                REFERENCES sessions(session_key) ON DELETE CASCADE,
            entry_id INTEGER NOT NULL CHECK (entry_id > 0),
            epoch INTEGER NOT NULL CHECK (epoch > 0),
            request_id INTEGER,
            kind INTEGER NOT NULL CHECK (kind BETWEEN 0 AND 3),
            participant_id TEXT NOT NULL,
            display_name TEXT NOT NULL CHECK (display_name <> ''),
            addressed_to TEXT NOT NULL DEFAULT '',
            addressed_to_name TEXT NOT NULL DEFAULT '',
            text TEXT NOT NULL,
            status INTEGER NOT NULL CHECK (status IN (0, 2, 3)),
            created_at INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY (session_key, entry_id),
            FOREIGN KEY (session_key, request_id)
                REFERENCES turns(session_key, request_id),
            CHECK (kind NOT IN (0, 1) OR participant_id <> ''),
            CHECK (kind <> 0 OR (addressed_to <> '' AND addressed_to_name <> '')),
            CHECK (kind = 0 OR (addressed_to = '' AND addressed_to_name = '')),
            CHECK (kind <> 3 OR status = 3),
            CHECK (kind NOT IN (0, 2) OR status = 0),
            CHECK (kind <> 1 OR status <> 3),
            CHECK (kind <> 1 OR status <> 0 OR text <> '')
        ) STRICT;

        CREATE INDEX entries_by_session_and_epoch
            ON entries (session_key, epoch, entry_id);

        CREATE UNIQUE INDEX one_prompt_per_session_turn
            ON entries (session_key, request_id)
            WHERE kind = 0 AND request_id IS NOT NULL;
    )sql");
}

void set_workspace_session_database_identity(Database& database) {
    database.execute(
        "PRAGMA application_id = "
        + std::to_string(workspace_session_application_id));
    database.execute(
        "PRAGMA user_version = "
        + std::to_string(workspace_session_database_version));
}

void validate_workspace_session_contents(Database& database) {
    validate_required_objects(database);
    validate_integrity(database);

    Statement missing_prompt = database.prepare(
        "SELECT t.request_id FROM turns AS t LEFT JOIN entries AS e ON "
        "t.session_key = e.session_key AND e.request_id = t.request_id "
        "AND e.epoch = t.epoch AND e.kind = 0 "
        "GROUP BY t.session_key, t.request_id "
        "HAVING COUNT(e.entry_id) <> 1 LIMIT 1");
    if (missing_prompt.step()) {
        throw std::runtime_error(
            "Workspace session database '" + database.path()
            + "' has a turn without exactly one prompt");
    }

    Statement wrong_epoch = database.prepare(
        "SELECT e.entry_id FROM entries AS e JOIN turns AS t ON "
        "t.session_key = e.session_key AND t.request_id = e.request_id "
        "WHERE e.request_id IS NOT NULL AND e.epoch <> t.epoch LIMIT 1");
    if (wrong_epoch.step()) {
        throw std::runtime_error(
            "Workspace session database '" + database.path()
            + "' has an entry in a different epoch from its turn");
    }

    Statement invalid_counter = database.prepare(
        "SELECT s.session_key FROM sessions AS s "
        "WHERE COALESCE((SELECT MAX(e.entry_id) FROM entries AS e "
        "WHERE e.session_key = s.session_key), 0) >= s.next_entry_id OR "
        "COALESCE((SELECT MAX(t.request_id) FROM turns AS t "
        "WHERE t.session_key = s.session_key), 0) >= s.next_request_id LIMIT 1");
    if (invalid_counter.step()) {
        throw std::runtime_error(
            "Workspace session database '" + database.path()
            + "' has a durable ID counter that does not follow its rows");
    }
}

WorkspaceSessionDatabaseCounts validate_workspace_session_database(
    const std::filesystem::path& path) {
    Database database(path, Database::Mode::read_only);
    if (database.pragma_integer("application_id")
            != workspace_session_application_id
        || database.pragma_integer("user_version")
            != workspace_session_database_version) {
        throw std::runtime_error(
            "Workspace session database '" + utf8_path(path)
            + "' has an unsupported schema");
    }
    validate_workspace_session_contents(database);
    return {
        .forums = static_cast<std::uint64_t>(
            scalar(database, "SELECT COUNT(*) FROM forums")),
        .active_sessions = static_cast<std::uint64_t>(scalar(
            database,
            "SELECT COUNT(*) FROM sessions WHERE archived_at IS NULL")),
        .archived_sessions = static_cast<std::uint64_t>(scalar(
            database,
            "SELECT COUNT(*) FROM sessions WHERE archived_at IS NOT NULL")),
        .turns = static_cast<std::uint64_t>(
            scalar(database, "SELECT COUNT(*) FROM turns")),
        .entries = static_cast<std::uint64_t>(
            scalar(database, "SELECT COUNT(*) FROM entries")),
    };
}

} // namespace cha
