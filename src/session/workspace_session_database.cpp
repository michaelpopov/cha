#include "session/workspace_session_database.h"

#include "util/path_name.h"
#include "util/private_filesystem.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace cha {
namespace {

using Database = storage::SqliteDatabase;
using Statement = storage::SqliteStatement;

constexpr std::array required_session_objects{
    std::pair{"table", "forums"},
    std::pair{"table", "sessions"},
    std::pair{"table", "turns"},
    std::pair{"table", "entries"},
    std::pair{"index", "active_sessions_by_update"},
    std::pair{"index", "one_started_turn_per_session"},
    std::pair{"index", "entries_by_session_and_epoch"},
    std::pair{"index", "one_prompt_per_session_turn"},
};

constexpr std::string_view config_table_sql = R"sql(
CREATE TABLE config (
    name TEXT PRIMARY KEY CHECK (
        name <> ''
        AND substr(name, 1, 1) <> '/'
        AND instr(name, char(92)) = 0
        AND instr('/' || name || '/', '/../') = 0
        AND name NOT IN ('app.toml', 'workspace.toml')
    ),
    content TEXT NOT NULL
) STRICT;
)sql";

constexpr std::array sidecar_suffixes{
    std::string_view("-journal"),
    std::string_view("-wal"),
    std::string_view("-shm"),
};

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

bool has_schema_object(
    Database& database,
    std::string_view type,
    std::string_view name) {
    Statement found = database.prepare(
        "SELECT COUNT(*) FROM sqlite_schema WHERE type = ?1 AND name = ?2",
        type,
        name);
    return found.step() && found.integer(0) == 1;
}

bool has_required_session_objects(Database& database) {
    for (const auto& [type, name] : required_session_objects) {
        if (!has_schema_object(
                database, std::string_view(type), std::string_view(name))) {
            return false;
        }
    }
    return true;
}

bool is_abandoned_empty_creation(Database& database) {
    return database.pragma_integer("application_id") == 0
        && database.pragma_integer("user_version") == 0
        && scalar(database,
            "SELECT COUNT(*) FROM sqlite_schema "
            "WHERE name NOT LIKE 'sqlite_%'") == 0;
}

bool is_valid_v1_identity(Database& database) {
    return database.pragma_integer("application_id")
            == workspace_session_application_id
        && database.pragma_integer("user_version")
            == workspace_session_database_version_v1
        && has_required_session_objects(database);
}

[[noreturn]] void throw_schema_v1_requires_import(std::string_view path) {
    throw std::runtime_error(
        "Workspace session database '" + std::string(path)
        + "' is a valid CHA schema-1 database. Stop CHA and run:\n"
        "chaweb --config=CONFIG --import WORKSPACE");
}

void remove_database_files_noexcept(
    const std::filesystem::path& path) noexcept {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    for (const std::string_view suffix : sidecar_suffixes) {
        std::filesystem::path sidecar = path;
        sidecar += suffix;
        std::filesystem::remove(sidecar, ignored);
    }
}

void create_config_table(Database& database) {
    database.execute(config_table_sql);
}

void validate_required_session_objects(Database& database) {
    for (const auto& [type, name] : required_session_objects) {
        if (!has_schema_object(
                database, std::string_view(type), std::string_view(name))) {
            throw std::runtime_error(
                "Workspace session database '" + database.path()
                + "' is missing required " + type + " '" + name + "'");
        }
    }
}

void validate_required_config_object(Database& database) {
    if (!has_schema_object(database, "table", "config")) {
        throw std::runtime_error(
            "Workspace session database '" + database.path()
            + "' is missing required table 'config'");
    }

    Statement info = database.prepare("PRAGMA table_info(config)");
    struct Column {
        std::string name;
        std::string type;
        std::int64_t notnull{};
        std::int64_t pk{};
    };
    std::vector<Column> columns;
    while (info.step()) {
        columns.push_back({
            info.text(1),
            info.text(2),
            info.integer(3),
            info.integer(5),
        });
    }
    if (columns.size() != 2
        || columns[0].name != "name"
        || columns[0].type != "TEXT"
        || columns[0].pk != 1
        || columns[1].name != "content"
        || columns[1].type != "TEXT"
        || columns[1].notnull != 1
        || columns[1].pk != 0) {
        throw std::runtime_error(
            "Workspace session database '" + database.path()
            + "' has a malformed config table");
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

void validate_session_row_invariants(Database& database) {
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

void validate_v1_contents(Database& database) {
    validate_required_session_objects(database);
    validate_integrity(database);
    validate_session_row_invariants(database);
}

bool contents_are_valid_v1(Database& database) {
    try {
        validate_v1_contents(database);
        return true;
    } catch (const std::runtime_error&) {
        return false;
    }
}

bool contents_are_valid_v2(Database& database) {
    try {
        validate_workspace_session_contents(database);
        return true;
    } catch (const std::runtime_error&) {
        return false;
    }
}

void insert_config_rows(
    Database& database,
    const std::vector<ConfigFile>& rows) {
    for (const auto& row : rows) {
        validate_stored_config_name(row.name);
        Statement insert = database.prepare(
            "INSERT INTO config (name, content) VALUES (?1, ?2)",
            std::string_view(row.name),
            std::string_view(row.content));
        insert.run();
    }
}

void secure_workspace_database_files(const std::filesystem::path& path) {
    const auto tighten_if_present = [](const std::filesystem::path& file) {
        std::error_code error;
        const std::filesystem::file_status status =
            std::filesystem::symlink_status(file, error);
        if (error) {
            if (error == std::errc::no_such_file_or_directory) return;
            throw std::filesystem::filesystem_error(
                "Failed to inspect workspace session database file",
                file,
                error);
        }
        if (!std::filesystem::exists(status)) return;
        tighten_private_file(file);
    };

    tighten_if_present(path);
    for (const std::string_view suffix : sidecar_suffixes) {
        std::filesystem::path sidecar = path;
        sidecar += suffix;
        tighten_if_present(sidecar);
    }
}

void enable_wal_and_secure(Database& database, const std::filesystem::path& path) {
    database.execute("PRAGMA journal_mode = WAL");
    secure_workspace_database_files(path);
}

} // namespace

void create_workspace_session_schema(Database& database) {
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
    create_config_table(database);
}

void set_workspace_session_database_identity(Database& database) {
    database.execute(
        "PRAGMA application_id = "
        + std::to_string(workspace_session_application_id));
    database.execute(
        "PRAGMA user_version = "
        + std::to_string(workspace_session_database_version));
}

void validate_workspace_session_database_identity(Database& database) {
    if (is_valid_v1_identity(database)) {
        throw_schema_v1_requires_import(database.path());
    }
    if (database.pragma_integer("application_id")
            != workspace_session_application_id
        || database.pragma_integer("user_version")
            != workspace_session_database_version) {
        throw std::runtime_error(
            "Workspace session database '" + database.path()
            + "' has an unsupported schema");
    }
}

void validate_workspace_session_contents(Database& database) {
    validate_required_session_objects(database);
    validate_required_config_object(database);
    validate_integrity(database);
    validate_session_row_invariants(database);
}

WorkspaceDatabaseState inspect_workspace_session_database(
    const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(path, error);
    if (error) {
        if (error == std::errc::no_such_file_or_directory) {
            return WorkspaceDatabaseState::missing;
        }
        return WorkspaceDatabaseState::corrupt;
    }
    if (!std::filesystem::exists(status)) {
        return WorkspaceDatabaseState::missing;
    }
    if (!std::filesystem::is_regular_file(status)) {
        return WorkspaceDatabaseState::corrupt;
    }

    try {
        Database database(path, Database::Mode::read_only);
        const std::int64_t application_id =
            database.pragma_integer("application_id");
        const std::int64_t version = database.pragma_integer("user_version");
        if (application_id != workspace_session_application_id) {
            return WorkspaceDatabaseState::wrong_application_id;
        }
        if (version == workspace_session_database_version_v1) {
            return contents_are_valid_v1(database)
                ? WorkspaceDatabaseState::valid_v1
                : WorkspaceDatabaseState::corrupt;
        }
        if (version == workspace_session_database_version) {
            return contents_are_valid_v2(database)
                ? WorkspaceDatabaseState::valid_v2
                : WorkspaceDatabaseState::corrupt;
        }
        return WorkspaceDatabaseState::unsupported_version;
    } catch (const std::runtime_error&) {
        return WorkspaceDatabaseState::corrupt;
    }
}

void validate_stored_config_name(std::string_view name) {
    if (name.empty()) {
        throw std::runtime_error("Configuration name is empty");
    }
    if (name.front() == '/'
        || name.find(':') != std::string_view::npos) {
        throw std::runtime_error(
            "Configuration name '" + std::string(name) + "' must be relative");
    }
    if (name.find('\\') != std::string_view::npos) {
        throw std::runtime_error(
            "Configuration name '" + std::string(name)
            + "' contains a backslash");
    }
    if (name == "app.toml" || name == "workspace.toml") {
        throw std::runtime_error(
            "External application config '" + std::string(name)
            + "' must not be stored in the database");
    }

    std::string_view rest = name;
    for (;;) {
        const auto slash = rest.find('/');
        const std::string_view component = rest.substr(0, slash);
        if (component.empty() || component == "." || component == "..") {
            throw std::runtime_error(
                "Configuration name '" + std::string(name)
                + "' contains an invalid path component");
        }
        if (slash == std::string_view::npos) break;
        rest = rest.substr(slash + 1);
        if (rest.empty()) {
            throw std::runtime_error(
                "Configuration name '" + std::string(name)
                + "' contains an invalid path component");
        }
    }

    if (name != ".env"
        && !name.ends_with(".toml")
        && !name.ends_with(".md")) {
        throw std::runtime_error(
            "Configuration name '" + std::string(name)
            + "' is not a stored configuration file");
    }
}

std::vector<ConfigFile> read_workspace_config_files(Database& database) {
    Statement statement = database.prepare(
        "SELECT name, content FROM config ORDER BY name");
    std::vector<ConfigFile> rows;
    while (statement.step()) {
        ConfigFile row{statement.text(0), statement.text(1)};
        validate_stored_config_name(row.name);
        rows.push_back(std::move(row));
    }
    return rows;
}

void replace_workspace_config_files(
    Database& database,
    const std::vector<ConfigFile>& rows) {
    for (const auto& row : rows) {
        validate_stored_config_name(row.name);
    }
    database.execute("DELETE FROM config");
    insert_config_rows(database, rows);
}

void upgrade_workspace_session_database_from_v1(
    Database& database,
    const std::vector<ConfigFile>& rows) {
    if (database.pragma_integer("application_id")
            != workspace_session_application_id
        || database.pragma_integer("user_version")
            != workspace_session_database_version_v1) {
        throw std::runtime_error(
            "Workspace session database '" + database.path()
            + "' is not a valid CHA schema-1 database");
    }
    validate_v1_contents(database);
    for (const auto& row : rows) {
        validate_stored_config_name(row.name);
    }

    storage::SqliteTransaction transaction(database);
    create_config_table(database);
    insert_config_rows(database, rows);
    database.execute(
        "PRAGMA user_version = "
        + std::to_string(workspace_session_database_version));
    transaction.commit();
}

void secure_workspace_session_database_files(
    const std::filesystem::path& path) {
    secure_workspace_database_files(path);
}

void create_empty_workspace_session_database(
    const std::filesystem::path& path) {
    if (std::filesystem::exists(path)) {
        throw std::runtime_error(
            "Workspace session database already exists at '"
            + utf8_path(path) + "'");
    }
    try {
        Database database(path, Database::Mode::read_write_create);
        database.execute("PRAGMA journal_mode = DELETE");
        storage::SqliteTransaction transaction(database);
        create_workspace_session_schema(database);
        set_workspace_session_database_identity(database);
        transaction.commit();
    } catch (...) {
        remove_database_files_noexcept(path);
        throw;
    }
}

void initialize_workspace_session_database_runtime(
    const std::filesystem::path& path) {
    secure_workspace_database_files(path);
    // Tests and helpers may still create a missing disposable database at v2.
    // Normal runtime never calls this on a missing path; a valid v1 database
    // is never upgraded or deleted here.
    if (!std::filesystem::exists(path)) {
        create_empty_workspace_session_database(path);
    }

    {
        Database database(path, Database::Mode::read_write);
        if (!is_abandoned_empty_creation(database)) {
            validate_workspace_session_database_identity(database);
            validate_workspace_session_contents(database);
            enable_wal_and_secure(database, path);
            return;
        }
    }

    remove_database_files_noexcept(path);
    create_empty_workspace_session_database(path);
    Database database(path, Database::Mode::read_write);
    validate_workspace_session_database_identity(database);
    validate_workspace_session_contents(database);
    enable_wal_and_secure(database, path);
}

void checkpoint_workspace_session_database(
    const std::filesystem::path& path) {
    Database database(path, Database::Mode::read_write);
    validate_workspace_session_database_identity(database);
    Statement checkpoint = database.prepare("PRAGMA wal_checkpoint(TRUNCATE)");
    if (!checkpoint.step()) {
        throw std::runtime_error(
            "Failed to checkpoint workspace session database '"
            + utf8_path(path) + "'");
    }
    if (checkpoint.integer(0) != 0) {
        throw std::runtime_error(
            "Workspace session database checkpoint remained busy for '"
            + utf8_path(path) + "'");
    }
}

} // namespace cha
