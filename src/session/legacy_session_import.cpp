#include "session/legacy_session_import.h"

#include "util/path_name.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace cha {
namespace {

using Database = storage::SqliteDatabase;
using Statement = storage::SqliteStatement;

constexpr std::int64_t legacy_application_id = 0x43484131; // "CHA1"
constexpr std::int64_t legacy_version_without_timestamps = 2;
constexpr std::int64_t legacy_version = 3;

[[noreturn]] void fail_source(
    const std::filesystem::path& path,
    std::string_view reason) {
    throw std::runtime_error(
        "Invalid legacy session database '" + utf8_path(path) + "': "
        + std::string(reason));
}

std::int64_t scalar(Database& database, std::string_view sql) {
    Statement statement = database.prepare(sql);
    if (!statement.step()) {
        throw std::runtime_error(
            "SQLite query returned no value for '" + database.path() + "'");
    }
    return statement.integer(0);
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

void validate_turn_relationships(Database& database) {
    Statement missing_prompt = database.prepare(
        "SELECT t.request_id FROM turns AS t LEFT JOIN entries AS e ON "
        "e.request_id = t.request_id AND e.epoch = t.epoch AND e.kind = 0 "
        "GROUP BY t.request_id HAVING COUNT(e.entry_id) <> 1 LIMIT 1");
    if (missing_prompt.step()) {
        throw std::runtime_error(
            "Database '" + database.path()
            + "' has a turn without exactly one prompt");
    }

    Statement wrong_epoch = database.prepare(
        "SELECT e.entry_id FROM entries AS e JOIN turns AS t ON "
        "t.request_id = e.request_id "
        "WHERE e.request_id IS NOT NULL AND e.epoch <> t.epoch LIMIT 1");
    if (wrong_epoch.step()) {
        throw std::runtime_error(
            "Database '" + database.path()
            + "' has an entry in a different epoch from its turn");
    }
}

std::int64_t unix_seconds(std::filesystem::file_time_type time) {
    const auto system_time = std::chrono::time_point_cast<
        std::chrono::system_clock::duration>(
            time - std::filesystem::file_time_type::clock::now()
            + std::chrono::system_clock::now());
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        system_time.time_since_epoch()).count();
    return std::max<std::int64_t>(0, seconds);
}

} // namespace

ValidatedLegacySession validate_legacy_session_source(
    LegacySessionSource source) {
    try {
        require_url_safe_identifier(
            source.expected_identity.session_id, source.path);
        Database database(source.path, Database::Mode::read_only);
        const std::int64_t application_id =
            database.pragma_integer("application_id");
        const std::int64_t version = database.pragma_integer("user_version");
        if (application_id != legacy_application_id
            || (version != legacy_version
                && version != legacy_version_without_timestamps)) {
            fail_source(
                source.path,
                "unsupported application ID or schema version "
                    + std::to_string(version));
        }

        Statement metadata = database.prepare(
            "SELECT id, forum, label FROM session WHERE singleton = 1");
        if (!metadata.step()) fail_source(source.path, "metadata is missing");
        const std::string session_id = metadata.text(0);
        const std::string forum_id = metadata.text(1);
        const std::string label = metadata.text(2);
        if (session_id != source.expected_identity.session_id
            || forum_id != source.expected_identity.forum_id) {
            fail_source(
                source.path,
                "embedded identity is '" + forum_id + '/' + session_id
                    + "', expected '" + source.expected_identity.forum_id
                    + '/' + source.expected_identity.session_id + "'");
        }
        if (label.empty()) fail_source(source.path, "label is empty");

        Statement state = database.prepare(
            "SELECT history_epoch, next_entry_id, next_request_id "
            "FROM state WHERE singleton = 1");
        if (!state.step()) fail_source(source.path, "durable state is missing");
        const std::int64_t history_epoch = state.integer(0);
        const std::int64_t next_entry_id = state.integer(1);
        const std::int64_t next_request_id = state.integer(2);
        if (history_epoch <= 0 || next_entry_id <= 0 || next_request_id <= 0) {
            fail_source(source.path, "durable state contains invalid counters");
        }

        validate_integrity(database);
        validate_turn_relationships(database);
        Statement turns = database.prepare(
            "SELECT request_id, epoch, state FROM turns ORDER BY request_id");
        while (turns.step()) {
            (void)turns.integer(0);
            (void)turns.integer(1);
            (void)turns.integer(2);
        }
        const std::string entry_query = version == legacy_version
            ? "SELECT entry_id, epoch, request_id, kind, participant_id, "
              "display_name, addressed_to, addressed_to_name, text, status, "
              "created_at FROM entries ORDER BY entry_id"
            : "SELECT entry_id, epoch, request_id, kind, participant_id, "
              "display_name, addressed_to, addressed_to_name, text, status "
              "FROM entries ORDER BY entry_id";
        Statement entries = database.prepare(entry_query);
        while (entries.step()) {
            (void)entries.integer(0);
            (void)entries.integer(1);
            if (!entries.is_null(2)) (void)entries.integer(2);
            (void)entries.integer(3);
            (void)entries.text(4);
            (void)entries.text(5);
            (void)entries.text(6);
            (void)entries.text(7);
            (void)entries.text(8);
            (void)entries.integer(9);
            if (version == legacy_version) (void)entries.integer(10);
        }
        if (scalar(database,
                "SELECT COUNT(*) FROM entries WHERE entry_id >= "
                "(SELECT next_entry_id FROM state WHERE singleton = 1)") != 0
            || scalar(database,
                "SELECT COUNT(*) FROM turns WHERE request_id >= "
                "(SELECT next_request_id FROM state WHERE singleton = 1)") != 0) {
            fail_source(source.path, "durable ID counters do not follow stored rows");
        }

        return {
            .source = source,
            .label = label,
            .version = version,
            .history_epoch = history_epoch,
            .next_entry_id = next_entry_id,
            .next_request_id = next_request_id,
            .updated_at = unix_seconds(
                std::filesystem::last_write_time(source.path)),
            .turns = static_cast<std::uint64_t>(
                scalar(database, "SELECT COUNT(*) FROM turns")),
            .entries = static_cast<std::uint64_t>(
                scalar(database, "SELECT COUNT(*) FROM entries")),
        };
    } catch (const std::filesystem::filesystem_error&) {
        throw;
    } catch (const std::exception& error) {
        const std::string prefix =
            "Invalid legacy session database '" + utf8_path(source.path) + "': ";
        const std::string message = error.what();
        if (message.starts_with(prefix)) throw;
        fail_source(source.path, message);
    }
}

void import_legacy_session(
    Database& target,
    const ValidatedLegacySession& source,
    std::int64_t forum_key,
    std::int64_t archived_at) {
    Statement insert_session = target.prepare(
        "INSERT INTO sessions (forum_key, session_id, label, updated_at, "
        "archived_at, history_epoch, next_entry_id, next_request_id) "
        "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)");
    insert_session.bind(1, forum_key);
    insert_session.bind(2, source.source.expected_identity.session_id);
    insert_session.bind(3, source.label);
    insert_session.bind(4, source.updated_at);
    if (source.source.archived) insert_session.bind(5, archived_at);
    else insert_session.bind_null(5);
    insert_session.bind(6, source.history_epoch);
    insert_session.bind(7, source.next_entry_id);
    insert_session.bind(8, source.next_request_id);
    insert_session.run();
    const std::int64_t session_key = target.last_insert_rowid();

    Database legacy(source.source.path, Database::Mode::read_only);
    Statement turns = legacy.prepare(
        "SELECT request_id, epoch, state FROM turns ORDER BY request_id");
    while (turns.step()) {
        Statement insert = target.prepare(
            "INSERT INTO turns (session_key, request_id, epoch, state) "
            "VALUES (?1, ?2, ?3, ?4)",
            session_key,
            turns.integer(0),
            turns.integer(1),
            turns.integer(2));
        insert.run();
    }

    const std::string entry_query = source.version == legacy_version
        ? "SELECT entry_id, epoch, request_id, kind, participant_id, "
          "display_name, addressed_to, addressed_to_name, text, status, "
          "created_at FROM entries ORDER BY entry_id"
        : "SELECT entry_id, epoch, request_id, kind, participant_id, "
          "display_name, addressed_to, addressed_to_name, text, status, "
          "0 FROM entries ORDER BY entry_id";
    Statement entries = legacy.prepare(entry_query);
    while (entries.step()) {
        Statement insert = target.prepare(
            "INSERT INTO entries (session_key, entry_id, epoch, request_id, "
            "kind, participant_id, display_name, addressed_to, "
            "addressed_to_name, text, status, created_at) "
            "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)");
        insert.bind(1, session_key);
        insert.bind(2, entries.integer(0));
        insert.bind(3, entries.integer(1));
        if (entries.is_null(2)) insert.bind_null(4);
        else insert.bind(4, entries.integer(2));
        insert.bind(5, entries.integer(3));
        insert.bind(6, entries.text(4));
        insert.bind(7, entries.text(5));
        insert.bind(8, entries.text(6));
        insert.bind(9, entries.text(7));
        insert.bind(10, entries.text(8));
        insert.bind(11, entries.integer(9));
        insert.bind(12, entries.integer(10));
        insert.run();
    }
}

} // namespace cha
