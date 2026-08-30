#include "session/session_database.h"
#include "session/sqlite_storage.h"

#include "util/path_name.h"

#include <uv.h>

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace cha {
namespace {

constexpr int session_application_id = 0x43484131; // "CHA1"
constexpr int session_database_version = 3;
// The version before entries.created_at existed. Read-only paths accept it so
// an unmigrated database can still be listed and inspected; prepare()
// migrates it before any restore read.
constexpr int session_database_version_without_timestamps = 2;

static_assert(static_cast<std::int64_t>(EntryKind::human) == 0);
static_assert(static_cast<std::int64_t>(EntryKind::character) == 1);
static_assert(static_cast<std::int64_t>(EntryKind::notice) == 2);
static_assert(static_cast<std::int64_t>(EntryKind::error) == 3);
static_assert(static_cast<std::int64_t>(EntryStatus::complete) == 0);
static_assert(static_cast<std::int64_t>(EntryStatus::streaming) == 1);
static_assert(static_cast<std::int64_t>(EntryStatus::cancelled) == 2);
static_assert(static_cast<std::int64_t>(EntryStatus::failed) == 3);

enum class TurnState : std::int64_t {
    started = 0,
    completed = 1,
    cancelled = 2,
    failed = 3,
};

static_assert(static_cast<std::int64_t>(TurnState::started) == 0);
static_assert(static_cast<std::int64_t>(TurnState::completed) == 1);
static_assert(static_cast<std::int64_t>(TurnState::cancelled) == 2);
static_assert(static_cast<std::int64_t>(TurnState::failed) == 3);

using Database = storage::SqliteDatabase;
using Statement = storage::SqliteStatement;
using Transaction = storage::SqliteTransaction;

std::int64_t sqlite_id(std::uint64_t value, std::string_view name) {
    if (value == 0
        || value >= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument(std::string(name) + " is outside SQLite's positive integer range");
    }
    return static_cast<std::int64_t>(value);
}

std::uint64_t unsigned_id(std::int64_t value, std::string_view name) {
    if (value <= 0) {
        throw std::runtime_error(
            "Session database contains an invalid " + std::string(name));
    }
    return static_cast<std::uint64_t>(value);
}

void create_schema(Database& database) {
    database.execute(R"sql(
        CREATE TABLE session (
            singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
            id TEXT NOT NULL UNIQUE CHECK (id <> ''),
            forum TEXT NOT NULL CHECK (forum <> ''),
            label TEXT NOT NULL CHECK (label <> '')
        ) STRICT;

        CREATE TABLE state (
            singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
            history_epoch INTEGER NOT NULL CHECK (history_epoch > 0),
            next_entry_id INTEGER NOT NULL CHECK (next_entry_id > 0),
            next_request_id INTEGER NOT NULL CHECK (next_request_id > 0)
        ) STRICT;

        CREATE TABLE turns (
            request_id INTEGER PRIMARY KEY CHECK (request_id > 0),
            epoch INTEGER NOT NULL CHECK (epoch > 0),
            state INTEGER NOT NULL CHECK (state BETWEEN 0 AND 3)
        ) STRICT;

        CREATE UNIQUE INDEX one_started_turn
            -- A constant expression makes all started rows contend for one key.
            ON turns ((1))
            WHERE state = 0;

        CREATE TABLE entries (
            entry_id INTEGER PRIMARY KEY CHECK (entry_id > 0),
            epoch INTEGER NOT NULL CHECK (epoch > 0),
            request_id INTEGER REFERENCES turns(request_id),
            kind INTEGER NOT NULL CHECK (kind BETWEEN 0 AND 3),
            participant_id TEXT NOT NULL,
            display_name TEXT NOT NULL CHECK (display_name <> ''),
            addressed_to TEXT NOT NULL DEFAULT '',
            addressed_to_name TEXT NOT NULL DEFAULT '',
            text TEXT NOT NULL,
            status INTEGER NOT NULL CHECK (status IN (0, 2, 3)),
            created_at INTEGER NOT NULL DEFAULT 0,
            CHECK (kind NOT IN (0, 1) OR participant_id <> ''),
            CHECK (kind <> 0 OR (addressed_to <> '' AND addressed_to_name <> '')),
            CHECK (kind = 0 OR (addressed_to = '' AND addressed_to_name = '')),
            CHECK (kind <> 3 OR status = 3),
            CHECK (kind NOT IN (0, 2) OR status = 0),
            CHECK (kind <> 1 OR status <> 3),
            CHECK (kind <> 1 OR status <> 0 OR text <> '')
        ) STRICT;

        CREATE INDEX entries_by_epoch
            ON entries (epoch, entry_id);

        CREATE UNIQUE INDEX one_prompt_per_turn
            ON entries (request_id)
            WHERE kind = 0 AND request_id IS NOT NULL;

        INSERT INTO state (
            singleton,
            history_epoch,
            next_entry_id,
            next_request_id
        ) VALUES (1, 1, 1, 1);

    )sql");
    database.execute(
        "PRAGMA application_id = "
        + std::to_string(session_application_id));
    database.execute(
        "PRAGMA user_version = "
        + std::to_string(session_database_version));
}

struct DurableState {
    std::int64_t epoch{};
    EntryId next_entry_id{};
    RequestId next_request_id{};
};

DurableState read_state(Database& database) {
    Statement state = database.prepare(
        "SELECT history_epoch, next_entry_id, next_request_id "
        "FROM state WHERE singleton = 1");
    if (!state.step()) {
        throw std::runtime_error(
            "Session database '" + database.path() + "' has no durable state");
    }
    return {
        state.integer(0),
        unsigned_id(state.integer(1), "next transcript entry ID"),
        unsigned_id(state.integer(2), "next request ID"),
    };
}

void validate_database_identity(Database& database) {
    const std::int64_t version = database.pragma_integer("user_version");
    if (database.pragma_integer("application_id") != session_application_id
        || (version != session_database_version
            && version != session_database_version_without_timestamps)) {
        throw std::runtime_error(
            "Unsupported session database '" + database.path() + "'");
    }
}

SessionDatabaseMetadata read_metadata(Database& database) {
    Statement statement = database.prepare(
        "SELECT id, forum, label FROM session WHERE singleton = 1");
    if (!statement.step()) {
        throw std::runtime_error(
            "Session database '" + database.path() + "' has no metadata");
    }
    return {
        .id = statement.text(0),
        .forum = statement.text(1),
        .label = statement.text(2),
    };
}

void validate_database_contents(Database& database) {
    (void)read_state(database);
    Statement turns_without_prompt = database.prepare(
        "SELECT t.request_id FROM turns AS t LEFT JOIN entries AS e "
        "ON e.request_id = t.request_id AND e.epoch = t.epoch AND e.kind = 0 "
        "GROUP BY t.request_id HAVING COUNT(e.entry_id) <> 1 LIMIT 1");
    if (turns_without_prompt.step()) {
        throw std::runtime_error("Session database '" + database.path() + "' has a turn without exactly one prompt");
    }
}

void validate_database(Database& database) {
    validate_database_identity(database);
    validate_database_contents(database);
}

EntryKind parse_kind(std::int64_t value) {
    if (value < 0 || value > 3) {
        throw std::runtime_error("Session database contains an unknown entry kind");
    }
    return static_cast<EntryKind>(value);
}

EntryStatus parse_status(std::int64_t value) {
    if (value != 0 && value != 2 && value != 3) {
        throw std::runtime_error("Session database contains an unknown entry status");
    }
    return static_cast<EntryStatus>(value);
}

enum class TurnRecordKind {
    started,
    completed,
    cancelled,
    failed,
};

void validate_turn_entry(
    TurnRecordKind record_kind,
    RequestId request_id,
    const TranscriptEntry& entry) {

    EntryKind expected_kind = EntryKind::human;
    EntryStatus expected_status = EntryStatus::complete;
    switch (record_kind) {
    case TurnRecordKind::started:
        break;
    case TurnRecordKind::completed:
        expected_kind = EntryKind::character;
        break;
    case TurnRecordKind::cancelled:
        expected_kind = EntryKind::character;
        expected_status = EntryStatus::cancelled;
        break;
    case TurnRecordKind::failed:
        expected_kind = EntryKind::error;
        expected_status = EntryStatus::failed;
        break;
    }

    if (request_id == 0 || entry.kind != expected_kind || entry.status != expected_status
        || !entry.request_id || *entry.request_id != request_id) {
        throw std::invalid_argument(
            "Turn entry does not match its record type and request ID");
    }
}

std::int64_t current_epoch(Database& database) {
    return read_state(database).epoch;
}

void advance_entry_id(Database& database, EntryId entry_id) {
    const std::int64_t value = sqlite_id(entry_id, "Transcript entry ID");
    Statement statement = database.prepare(
        "UPDATE state SET next_entry_id = ?1 + 1 "
        "WHERE singleton = 1 AND next_entry_id <= ?1",
        value);
    statement.run();
    if (database.changes() != 1) {
        throw std::invalid_argument(
            "Transcript entry IDs must be strictly increasing");
    }
}

void advance_request_id(Database& database, RequestId request_id) {
    const std::int64_t value = sqlite_id(request_id, "Request ID");
    Statement statement = database.prepare(
        "UPDATE state SET next_request_id = ?1 + 1 "
        "WHERE singleton = 1 AND next_request_id <= ?1",
        value);
    statement.run();
    if (database.changes() != 1) {
        throw std::invalid_argument("Request IDs must be strictly increasing");
    }
}

void insert_entry(
    Database& database,
    std::int64_t epoch,
    const TranscriptEntry& entry) {

    try {
        require_storable_transcript_entry(entry);
    } catch (const std::invalid_argument& error) {
        throw std::runtime_error(
            "Cannot store invalid transcript entry: "
            + std::string(error.what()));
    }
    advance_entry_id(database, entry.id);

    const std::optional<std::int64_t> request_id = entry.request_id
        ? std::optional(sqlite_id(*entry.request_id, "Request ID"))
        : std::nullopt;
    Statement statement = database.prepare(
        "INSERT INTO entries ("
        "entry_id, epoch, request_id, kind, participant_id, display_name, addressed_to, addressed_to_name, text, status, created_at"
        ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11)",
        sqlite_id(entry.id, "Transcript entry ID"),
        epoch,
        request_id,
        static_cast<std::int64_t>(entry.kind),
        std::string_view(entry.participant_id),
        std::string_view(entry.display_name),
        std::string_view(entry.addressed_to),
        std::string_view(entry.addressed_to_name),
        std::string_view(entry.text),
        static_cast<std::int64_t>(entry.status),
        entry.created_at);
    statement.run();
}

TranscriptEntry read_entry(Statement& statement) {
    TranscriptEntry entry{
        .id = unsigned_id(statement.integer(0), "transcript entry ID"),
        .kind = parse_kind(statement.integer(2)),
        .participant_id = statement.text(3),
        .display_name = statement.text(4),
        .addressed_to = statement.text(5),
        .addressed_to_name = statement.text(6),
        .text = statement.text(7),
        .status = parse_status(statement.integer(8)),
        .created_at = statement.integer(9),
    };
    if (!statement.is_null(1)) {
        entry.request_id = unsigned_id(statement.integer(1), "request ID");
    }
    try {
        require_storable_transcript_entry(entry);
    } catch (const std::invalid_argument& error) {
        throw std::runtime_error(
            "Session database contains an invalid transcript entry: "
            + std::string(error.what()));
    }
    return entry;
}

void transition_turn(
    Database& database,
    RequestId request_id,
    TurnState state) {

    Statement statement = database.prepare(
        "UPDATE turns SET state = ?1 WHERE request_id = ?2 AND state = 0",
        static_cast<std::int64_t>(state),
        sqlite_id(request_id, "Request ID"));
    statement.run();
    if (database.changes() != 1) {
        throw std::invalid_argument(
            "Turn does not identify the active request");
    }
}

void finish_turn(
    Database& database,
    RequestId request_id,
    TurnState state,
    const TranscriptEntry* entry) {

    Transaction transaction(database);
    transition_turn(database, request_id, state);
    if (entry) {
        insert_entry(database, current_epoch(database), *entry);
    }
    transaction.commit();
}

std::filesystem::path create_temporary_path(
    const std::filesystem::path& path) {

    const std::filesystem::path pattern_path =
        path.parent_path()
        / path_from_utf8(
            "." + utf8_path(path.filename()) + ".tmp.XXXXXX");
    const std::string pattern = utf8_path(pattern_path);
    std::vector<char> writable_pattern(pattern.begin(), pattern.end());
    writable_pattern.push_back('\0');

    uv_fs_t create_request{};
    const int descriptor = uv_fs_mkstemp(
        nullptr,
        &create_request,
        writable_pattern.data(),
        nullptr);
    if (descriptor < 0) {
        uv_fs_req_cleanup(&create_request);
        throw std::runtime_error(
            "Failed to create a temporary file for '"
            + utf8_path(path) + "': " + uv_strerror(descriptor));
    }
    const std::filesystem::path temporary_path =
        path_from_utf8(create_request.path);
    uv_fs_req_cleanup(&create_request);

    uv_fs_t close_request{};
    const int close_status =
        uv_fs_close(nullptr, &close_request, descriptor, nullptr);
    uv_fs_req_cleanup(&close_request);
    if (close_status < 0) {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
        throw std::runtime_error(
            "Failed to close temporary file '"
            + utf8_path(temporary_path) + "': "
            + uv_strerror(close_status));
    }
    return temporary_path;
}

// The restore half of an already validated database, so opening reads it
// through the same connection the identity check used.
SessionRestore build_restore(Database& database) {
    const DurableState state = read_state(database);
    SessionRestore result{
        .next_request_id = state.next_request_id,
        .next_entry_id = state.next_entry_id,
    };

    Statement entries = database.prepare(
        "SELECT entry_id, request_id, kind, participant_id, display_name, addressed_to, addressed_to_name, text, status, created_at "
        "FROM entries WHERE epoch = ?1 ORDER BY entry_id",
        state.epoch);
    while (entries.step()) {
        result.entries.push_back(read_entry(entries));
    }

    Statement interrupted = database.prepare(
        "SELECT t.request_id, e.addressed_to FROM turns AS t JOIN entries AS e "
        "ON e.request_id = t.request_id AND e.epoch = t.epoch AND e.kind = 0 "
        "WHERE t.state = 0 ORDER BY t.request_id");
    while (interrupted.step()) {
        const RequestId request_id =
            unsigned_id(interrupted.integer(0), "interrupted request ID");
        TranscriptEntry error = make_error_entry(
            result.next_entry_id++,
            "Response interrupted before it finished",
            request_id,
            interrupted.text(1));
        result.interrupted_turns.push_back({request_id, std::move(error)});
    }
    return result;
}

bool publish_database_path(
    const std::filesystem::path& temporary_path,
    const std::filesystem::path& path) {

    std::error_code error;
    std::filesystem::create_hard_link(
        temporary_path,
        path,
        error);
    if (!error) {
        return true;
    }
    if (error == std::errc::file_exists) {
        return false;
    }
    throw std::filesystem::filesystem_error(
        "Failed to publish session database",
        temporary_path,
        path,
        error);
}

} // namespace

bool create_session_database(
    const std::filesystem::path& path,
    const SessionDatabaseMetadata& metadata) {

    if (metadata.id.empty() || metadata.forum.empty()
        || metadata.label.empty()) {
        throw std::invalid_argument(
            "Session database metadata fields cannot be empty");
    }
    const std::filesystem::path temporary_path =
        create_temporary_path(path);

    try {
        {
            Database database(
                temporary_path,
                Database::Mode::read_write_create);
            Transaction transaction(database);
            create_schema(database);
            Statement statement = database.prepare(
                "INSERT INTO session (singleton, id, forum, label) "
                "VALUES (1, ?1, ?2, ?3)",
                std::string_view(metadata.id),
                std::string_view(metadata.forum),
                std::string_view(metadata.label));
            statement.run();
            transaction.commit();
        }
        const bool published =
            publish_database_path(temporary_path, path);
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
        return published;
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
        throw;
    }
}

void validate_session_database_identity(
    const std::filesystem::path& path,
    const FullSessionId& expected_identity,
    const SessionDatabaseMetadata& metadata) {

    if (metadata.id != expected_identity.session_id) {
        throw std::runtime_error(
            "Session database '" + utf8_path(path)
            + "' does not match its filename");
    }
    if (metadata.forum != expected_identity.forum_id) {
        throw std::runtime_error(
            "Session database '" + utf8_path(path) + "' does not belong to forum '"
            + expected_identity.forum_id + "'");
    }
}

SessionDatabaseMetadata read_session_database_metadata(
    const std::filesystem::path& path) {

    Database database(path, Database::Mode::read_only);
    // Listing sessions needs only stable identity metadata; opening one later
    // performs the full transcript validation too.
    validate_database_identity(database);
    return read_metadata(database);
}

SessionRestore load_session_state(
    const std::filesystem::path& path) {

    Database database(path, Database::Mode::read_only);
    validate_database(database);
    return build_restore(database);
}

LoadedSessionDatabase load_session_database(
    const std::filesystem::path& path,
    const FullSessionId& expected_identity) {

    Database database(path, Database::Mode::read_only);
    // Identity first: a database that is not the one asked for is rejected
    // before its transcript is read, let alone trusted.
    validate_database_identity(database);
    SessionDatabaseMetadata metadata = read_metadata(database);
    validate_session_database_identity(path, expected_identity, metadata);
    validate_database_contents(database);
    return {
        .metadata = std::move(metadata),
        .restore = build_restore(database),
    };
}

void migrate_session_database(const std::filesystem::path& path) {
    Database database(path, Database::Mode::read_write);
    const std::int64_t version = database.pragma_integer("user_version");
    if (version == session_database_version) {
        return;
    }
    if (version != session_database_version_without_timestamps) {
        throw std::runtime_error(
            "Unsupported session database '" + database.path() + "'");
    }
    // The ALTER and the version bump are one transaction: a crash between
    // them would leave a database that has the column but still claims the
    // old version, and the next open would re-run the ALTER and fail.
    Transaction transaction(database);
    database.execute(
        "ALTER TABLE entries ADD COLUMN created_at INTEGER NOT NULL DEFAULT 0");
    database.execute(
        "PRAGMA user_version = " + std::to_string(session_database_version));
    transaction.commit();
}

void rename_session_database(
    const std::filesystem::path& path,
    const FullSessionId& expected_identity,
    std::string_view label) {

    Database database(path, Database::Mode::read_write);
    validate_database_identity(database);
    const SessionDatabaseMetadata metadata = read_metadata(database);
    validate_session_database_identity(path, expected_identity, metadata);
    validate_database_contents(database);

    Transaction transaction(database);
    Statement update = database.prepare(
        "UPDATE session SET label = ?1 WHERE singleton = 1",
        label);
    update.run();
    if (database.changes() != 1) {
        throw std::runtime_error("Failed to rename session database '"
            + utf8_path(path) + "'");
    }
    transaction.commit();
}

class SessionJournal::Impl {
public:
    explicit Impl(const std::filesystem::path& path)
        : database(path, Database::Mode::read_write) {
        validate_database(database);
    }

    Database database;
};

SessionJournal::SessionJournal(std::filesystem::path path)
    : impl_(std::make_unique<Impl>(path)) {
}

SessionJournal::~SessionJournal() = default;

void SessionJournal::start_turn(
    RequestId request_id,
    const TranscriptEntry& prompt) {
    validate_turn_entry(TurnRecordKind::started, request_id, prompt);

    Transaction transaction(impl_->database);
    advance_request_id(impl_->database, request_id);
    const std::int64_t epoch = current_epoch(impl_->database);
    Statement turn = impl_->database.prepare(
        "INSERT INTO turns (request_id, epoch, state) "
        "VALUES (?1, ?2, 0)",
        sqlite_id(request_id, "Request ID"),
        epoch);
    turn.run();
    insert_entry(impl_->database, epoch, prompt);
    transaction.commit();
}

void SessionJournal::record_entry(const TranscriptEntry& entry) {
    Transaction transaction(impl_->database);
    insert_entry(impl_->database, current_epoch(impl_->database), entry);
    transaction.commit();
}

void SessionJournal::complete_turn(
    RequestId request_id,
    const TranscriptEntry& response) {

    validate_turn_entry(TurnRecordKind::completed, request_id, response);
    finish_turn(
        impl_->database,
        request_id,
        TurnState::completed,
        &response);
}

void SessionJournal::cancel_turn(
    RequestId request_id,
    std::optional<TranscriptEntry> response) {

    if (request_id == 0) {
        throw std::invalid_argument(
            "A cancelled turn requires a positive request ID");
    }
    if (response) {
        validate_turn_entry(TurnRecordKind::cancelled, request_id, *response);
    }

    finish_turn(
        impl_->database,
        request_id,
        TurnState::cancelled,
        response ? &*response : nullptr);
}

void SessionJournal::fail_turn(
    RequestId request_id,
    const TranscriptEntry& error) {

    validate_turn_entry(TurnRecordKind::failed, request_id, error);
    finish_turn(
        impl_->database,
        request_id,
        TurnState::failed,
        &error);
}

void SessionJournal::clear() {
    Transaction transaction(impl_->database);
    Statement active = impl_->database.prepare(
        "SELECT EXISTS(SELECT 1 FROM turns WHERE state = 0)");
    if (!active.step()) {
        throw std::runtime_error("Failed to inspect active session turn");
    }
    if (active.integer(0) != 0) {
        throw std::logic_error(
            "Cannot clear a session while a turn is active");
    }
    impl_->database.execute(
        "UPDATE state SET history_epoch = history_epoch + 1 "
        "WHERE singleton = 1");
    transaction.commit();
}

void SessionJournal::rename(std::string_view label) {
    Transaction transaction(impl_->database);
    Statement update = impl_->database.prepare(
        "UPDATE session SET label = ?1 WHERE singleton = 1",
        label);
    update.run();
    if (impl_->database.changes() != 1) {
        throw std::runtime_error("Failed to rename live session");
    }
    transaction.commit();
}

} // namespace cha
