#include "session/session_database.h"

#include "session/not_found_error.h"
#include "session/session_timestamp.h"
#include "session/sqlite_storage.h"
#include "session/workspace_session_database.h"
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
#include <utility>
#include <vector>

namespace cha {
namespace {

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

enum class TurnRecordKind { started, completed, cancelled, failed };

using Database = storage::SqliteDatabase;
using Statement = storage::SqliteStatement;
using Transaction = storage::SqliteTransaction;

class ReadSnapshot {
public:
    explicit ReadSnapshot(Database& database) : database_(&database) {
        database_->execute("BEGIN");
    }
    ~ReadSnapshot() {
        if (database_) database_->rollback_noexcept();
    }

    ReadSnapshot(const ReadSnapshot&) = delete;
    ReadSnapshot& operator=(const ReadSnapshot&) = delete;

private:
    Database* database_;
};

std::int64_t sqlite_id(std::uint64_t value, std::string_view name) {
    if (value == 0
        || value >= static_cast<std::uint64_t>(
            std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument(
            std::string(name) + " is outside SQLite's positive integer range");
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

void require_session_key(SessionKey session_key) {
    if (session_key <= 0) {
        throw std::invalid_argument("Session key must be positive");
    }
}

struct DurableState {
    std::int64_t epoch{};
    EntryId next_entry_id{};
    RequestId next_request_id{};
};

DurableState read_state(Database& database, SessionKey session_key) {
    Statement state = database.prepare(
        "SELECT history_epoch, next_entry_id, next_request_id "
        "FROM sessions WHERE session_key = ?1 AND archived_at IS NULL",
        session_key);
    if (!state.step()) {
        throw std::runtime_error(
            "Session database '" + database.path()
            + "' has no active session for the selected key");
    }
    return {
        state.integer(0),
        unsigned_id(state.integer(1), "next transcript entry ID"),
        unsigned_id(state.integer(2), "next request ID"),
    };
}

SessionDatabaseMetadata read_metadata(
    Database& database,
    SessionKey session_key) {
    Statement statement = database.prepare(
        "SELECT s.session_id, f.forum_id, s.label "
        "FROM sessions AS s JOIN forums AS f USING (forum_key) "
        "WHERE s.session_key = ?1 AND s.archived_at IS NULL",
        session_key);
    if (!statement.step()) {
        throw std::runtime_error(
            "Session database '" + database.path()
            + "' has no active session for the selected key");
    }
    return {
        .id = statement.text(0),
        .forum = statement.text(1),
        .label = statement.text(2),
    };
}

SessionKey only_session_key(Database& database) {
    validate_workspace_session_database_identity(database);
    Statement statement = database.prepare(
        "SELECT session_key FROM sessions WHERE archived_at IS NULL "
        "ORDER BY session_key");
    if (!statement.step()) {
        throw std::runtime_error(
            "Session database '" + database.path() + "' has no active session");
    }
    const SessionKey result = statement.integer(0);
    if (statement.step()) {
        throw std::runtime_error(
            "Session database '" + database.path()
            + "' contains more than one active session; a session key is required");
    }
    return result;
}

SessionKey resolve_session_key(
    Database& database,
    const FullSessionId& identity) {
    Statement statement = database.prepare(
        "SELECT s.session_key FROM sessions AS s "
        "JOIN forums AS f USING (forum_key) "
        "WHERE f.forum_id = ?1 AND s.session_id = ?2 "
        "AND s.archived_at IS NULL",
        std::string_view(identity.forum_id),
        std::string_view(identity.session_id));
    if (!statement.step()) {
        throw missing_session_error(identity.session_id);
    }
    return statement.integer(0);
}

void validate_session_database_identity(
    const std::filesystem::path& path,
    const FullSessionId& expected_identity,
    const SessionDatabaseMetadata& metadata) {
    if (metadata.id != expected_identity.session_id
        || metadata.forum != expected_identity.forum_id) {
        throw std::runtime_error(
            "Session database '" + utf8_path(path)
            + "' does not contain requested session '"
            + expected_identity.forum_id + '/' + expected_identity.session_id
            + "'");
    }
}

void validate_session_contents(Database& database, SessionKey session_key) {
    (void)read_state(database, session_key);
    Statement missing_prompt = database.prepare(
        "SELECT t.request_id FROM turns AS t LEFT JOIN entries AS e ON "
        "e.session_key = t.session_key AND e.request_id = t.request_id "
        "AND e.epoch = t.epoch AND e.kind = 0 "
        "WHERE t.session_key = ?1 GROUP BY t.request_id "
        "HAVING COUNT(e.entry_id) <> 1 LIMIT 1",
        session_key);
    if (missing_prompt.step()) {
        throw std::runtime_error(
            "Session database '" + database.path()
            + "' has a turn without exactly one prompt");
    }
}

EntryKind parse_kind(std::int64_t value) {
    if (value < 0 || value > 3) {
        throw std::runtime_error(
            "Session database contains an unknown entry kind");
    }
    return static_cast<EntryKind>(value);
}

EntryStatus parse_status(std::int64_t value) {
    if (value != 0 && value != 2 && value != 3) {
        throw std::runtime_error(
            "Session database contains an unknown entry status");
    }
    return static_cast<EntryStatus>(value);
}

void validate_turn_entry(
    TurnRecordKind record_kind,
    RequestId request_id,
    const TranscriptEntry& entry) {
    EntryKind expected_kind = EntryKind::human;
    EntryStatus expected_status = EntryStatus::complete;
    switch (record_kind) {
    case TurnRecordKind::started: break;
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
    if (request_id == 0 || entry.kind != expected_kind
        || entry.status != expected_status || !entry.request_id
        || *entry.request_id != request_id) {
        throw std::invalid_argument(
            "Turn entry does not match its record type and request ID");
    }
}

std::int64_t current_epoch(Database& database, SessionKey session_key) {
    return read_state(database, session_key).epoch;
}

void touch_session(Database& database, SessionKey session_key) {
    Statement touch = database.prepare(
        "UPDATE sessions SET updated_at = ?1 "
        "WHERE session_key = ?2 AND archived_at IS NULL",
        session_timestamp(), session_key);
    touch.run();
    if (database.changes() != 1) {
        throw std::runtime_error("Failed to update active session timestamp");
    }
}

void advance_entry_id(
    Database& database,
    SessionKey session_key,
    EntryId entry_id) {
    const std::int64_t value = sqlite_id(entry_id, "Transcript entry ID");
    Statement statement = database.prepare(
        "UPDATE sessions SET next_entry_id = ?1 + 1 "
        "WHERE session_key = ?2 AND archived_at IS NULL "
        "AND next_entry_id <= ?1",
        value, session_key);
    statement.run();
    if (database.changes() != 1) {
        throw std::invalid_argument(
            "Transcript entry IDs must be strictly increasing");
    }
}

void advance_request_id(
    Database& database,
    SessionKey session_key,
    RequestId request_id) {
    const std::int64_t value = sqlite_id(request_id, "Request ID");
    Statement statement = database.prepare(
        "UPDATE sessions SET next_request_id = ?1 + 1 "
        "WHERE session_key = ?2 AND archived_at IS NULL "
        "AND next_request_id <= ?1",
        value, session_key);
    statement.run();
    if (database.changes() != 1) {
        throw std::invalid_argument("Request IDs must be strictly increasing");
    }
}

void insert_entry(
    Database& database,
    SessionKey session_key,
    std::int64_t epoch,
    const TranscriptEntry& entry) {
    try {
        require_storable_transcript_entry(entry);
    } catch (const std::invalid_argument& error) {
        throw std::runtime_error(
            "Cannot store invalid transcript entry: "
            + std::string(error.what()));
    }
    advance_entry_id(database, session_key, entry.id);
    const std::optional<std::int64_t> request_id = entry.request_id
        ? std::optional(sqlite_id(*entry.request_id, "Request ID"))
        : std::nullopt;
    Statement statement = database.prepare(
        "INSERT INTO entries (session_key, entry_id, epoch, request_id, kind, "
        "participant_id, display_name, addressed_to, addressed_to_name, text, "
        "status, created_at) VALUES "
        "(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)",
        session_key,
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

std::vector<TranscriptEntry> read_current_entries(
    Database& database,
    SessionKey session_key,
    std::int64_t epoch) {
    std::vector<TranscriptEntry> result;
    Statement entries = database.prepare(
        "SELECT entry_id, request_id, kind, participant_id, display_name, "
        "addressed_to, addressed_to_name, text, status, created_at "
        "FROM entries WHERE session_key = ?1 AND epoch = ?2 "
        "ORDER BY entry_id",
        session_key, epoch);
    while (entries.step()) result.push_back(read_entry(entries));
    return result;
}

SessionRestore build_restore(Database& database, SessionKey session_key) {
    const DurableState state = read_state(database, session_key);
    SessionRestore result{
        .entries = read_current_entries(database, session_key, state.epoch),
        .next_request_id = state.next_request_id,
        .next_entry_id = state.next_entry_id,
    };
    Statement interrupted = database.prepare(
        "SELECT t.request_id, e.addressed_to FROM turns AS t "
        "JOIN entries AS e ON e.session_key = t.session_key "
        "AND e.request_id = t.request_id AND e.epoch = t.epoch "
        "AND e.kind = 0 WHERE t.session_key = ?1 AND t.state = 0 "
        "ORDER BY t.request_id",
        session_key);
    while (interrupted.step()) {
        const RequestId request_id = unsigned_id(
            interrupted.integer(0), "interrupted request ID");
        TranscriptEntry error = make_error_entry(
            result.next_entry_id++,
            "Response interrupted before it finished",
            request_id,
            interrupted.text(1));
        result.interrupted_turns.push_back({request_id, std::move(error)});
    }
    return result;
}

void transition_turn(
    Database& database,
    SessionKey session_key,
    RequestId request_id,
    TurnState state) {
    Statement statement = database.prepare(
        "UPDATE turns SET state = ?1 WHERE session_key = ?2 "
        "AND request_id = ?3 AND state = 0",
        static_cast<std::int64_t>(state),
        session_key,
        sqlite_id(request_id, "Request ID"));
    statement.run();
    if (database.changes() != 1) {
        throw std::invalid_argument(
            "Turn does not identify the active request");
    }
}

void finish_turn(
    Database& database,
    SessionKey session_key,
    RequestId request_id,
    TurnState state,
    const TranscriptEntry* entry) {
    Transaction transaction(database);
    transition_turn(database, session_key, request_id, state);
    if (entry) {
        insert_entry(
            database, session_key,
            current_epoch(database, session_key), *entry);
    }
    touch_session(database, session_key);
    transaction.commit();
}

std::filesystem::path create_temporary_path(
    const std::filesystem::path& path) {
    const std::filesystem::path pattern_path = path.parent_path()
        / path_from_utf8("." + utf8_path(path.filename()) + ".tmp.XXXXXX");
    const std::string pattern = utf8_path(pattern_path);
    std::vector<char> writable_pattern(pattern.begin(), pattern.end());
    writable_pattern.push_back('\0');
    uv_fs_t create_request{};
    const int descriptor = uv_fs_mkstemp(
        nullptr, &create_request, writable_pattern.data(), nullptr);
    if (descriptor < 0) {
        uv_fs_req_cleanup(&create_request);
        throw std::runtime_error(
            "Failed to create a temporary file for '" + utf8_path(path)
            + "': " + uv_strerror(descriptor));
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
            "Failed to close temporary file '" + utf8_path(temporary_path)
            + "': " + uv_strerror(close_status));
    }
    return temporary_path;
}

bool publish_database_path(
    const std::filesystem::path& temporary_path,
    const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::create_hard_link(temporary_path, path, error);
    if (!error) return true;
    if (error == std::errc::file_exists) return false;
    throw std::filesystem::filesystem_error(
        "Failed to publish session database", temporary_path, path, error);
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
    const std::filesystem::path temporary_path = create_temporary_path(path);
    try {
        {
            Database database(temporary_path, Database::Mode::read_write_create);
            database.execute("PRAGMA journal_mode = DELETE");
            Transaction transaction(database);
            create_workspace_session_schema(database);
            Statement forum = database.prepare(
                "INSERT INTO forums (forum_id) VALUES (?1)",
                std::string_view(metadata.forum));
            forum.run();
            Statement session = database.prepare(
                "INSERT INTO sessions (forum_key, session_id, label, updated_at, "
                "history_epoch, next_entry_id, next_request_id) "
                "VALUES (?1, ?2, ?3, ?4, 1, 1, 1)",
                database.last_insert_rowid(),
                std::string_view(metadata.id),
                std::string_view(metadata.label),
                session_timestamp());
            session.run();
            set_workspace_session_database_identity(database);
            transaction.commit();
        }
        const bool published = publish_database_path(temporary_path, path);
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
        return published;
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
        throw;
    }
}

SessionDatabaseMetadata read_session_database_metadata(
    const std::filesystem::path& path) {
    Database database(path, Database::Mode::read_only);
    const SessionKey session_key = only_session_key(database);
    return read_metadata(database, session_key);
}

SessionRestore load_session_state(const std::filesystem::path& path) {
    Database database(path, Database::Mode::read_only);
    ReadSnapshot snapshot(database);
    const SessionKey session_key = only_session_key(database);
    validate_session_contents(database, session_key);
    return build_restore(database, session_key);
}

LoadedSessionDatabase load_session_database(
    const std::filesystem::path& path,
    const FullSessionId& expected_identity) {
    Database database(path, Database::Mode::read_only);
    validate_workspace_session_database_identity(database);
    ReadSnapshot snapshot(database);
    const SessionKey session_key = resolve_session_key(database, expected_identity);
    SessionDatabaseMetadata metadata = read_metadata(database, session_key);
    validate_session_database_identity(path, expected_identity, metadata);
    validate_session_contents(database, session_key);
    return {
        .session_key = session_key,
        .metadata = std::move(metadata),
        .restore = build_restore(database, session_key),
    };
}

std::vector<TranscriptEntry> load_session_history(
    const std::filesystem::path& path,
    const FullSessionId& expected_identity) {
    Database database(path, Database::Mode::read_only);
    validate_workspace_session_database_identity(database);
    ReadSnapshot snapshot(database);
    const SessionKey session_key =
        resolve_session_key(database, expected_identity);
    const DurableState state = read_state(database, session_key);
    return read_current_entries(database, session_key, state.epoch);
}

class SessionJournal::Impl {
public:
    Impl(const std::filesystem::path& path, SessionKey selected_session_key)
        : session_key(selected_session_key),
          database(path, Database::Mode::read_write) {
        require_session_key(session_key);
        validate_workspace_session_database_identity(database);
        ReadSnapshot snapshot(database);
        validate_session_contents(database, session_key);
    }

    SessionKey session_key;
    Database database;
};

SessionJournal::SessionJournal(
    std::filesystem::path path,
    SessionKey session_key)
    : impl_(std::make_unique<Impl>(path, session_key)) {
}

SessionJournal::SessionJournal(std::filesystem::path path) {
    Database database(path, Database::Mode::read_only);
    const SessionKey session_key = only_session_key(database);
    impl_ = std::make_unique<Impl>(path, session_key);
}

SessionJournal::~SessionJournal() = default;

void SessionJournal::start_turn(
    RequestId request_id,
    const TranscriptEntry& prompt) {
    validate_turn_entry(TurnRecordKind::started, request_id, prompt);
    Transaction transaction(impl_->database);
    advance_request_id(impl_->database, impl_->session_key, request_id);
    const std::int64_t epoch =
        current_epoch(impl_->database, impl_->session_key);
    Statement turn = impl_->database.prepare(
        "INSERT INTO turns (session_key, request_id, epoch, state) "
        "VALUES (?1, ?2, ?3, 0)",
        impl_->session_key,
        sqlite_id(request_id, "Request ID"),
        epoch);
    turn.run();
    insert_entry(impl_->database, impl_->session_key, epoch, prompt);
    touch_session(impl_->database, impl_->session_key);
    transaction.commit();
}

void SessionJournal::record_entry(const TranscriptEntry& entry) {
    Transaction transaction(impl_->database);
    insert_entry(
        impl_->database, impl_->session_key,
        current_epoch(impl_->database, impl_->session_key), entry);
    touch_session(impl_->database, impl_->session_key);
    transaction.commit();
}

void SessionJournal::complete_turn(
    RequestId request_id,
    const TranscriptEntry& response) {
    validate_turn_entry(TurnRecordKind::completed, request_id, response);
    finish_turn(
        impl_->database, impl_->session_key, request_id,
        TurnState::completed, &response);
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
        impl_->database, impl_->session_key, request_id,
        TurnState::cancelled, response ? &*response : nullptr);
}

void SessionJournal::fail_turn(
    RequestId request_id,
    const TranscriptEntry& error) {
    validate_turn_entry(TurnRecordKind::failed, request_id, error);
    finish_turn(
        impl_->database, impl_->session_key, request_id,
        TurnState::failed, &error);
}

void SessionJournal::clear() {
    Transaction transaction(impl_->database);
    Statement active = impl_->database.prepare(
        "SELECT EXISTS(SELECT 1 FROM turns "
        "WHERE session_key = ?1 AND state = 0)",
        impl_->session_key);
    if (!active.step()) {
        throw std::runtime_error("Failed to inspect active session turn");
    }
    if (active.integer(0) != 0) {
        throw std::logic_error(
            "Cannot clear a session while a turn is active");
    }
    Statement clear = impl_->database.prepare(
        "UPDATE sessions SET history_epoch = history_epoch + 1, "
        "updated_at = ?1 WHERE session_key = ?2 AND archived_at IS NULL",
        session_timestamp(), impl_->session_key);
    clear.run();
    if (impl_->database.changes() != 1) {
        throw std::runtime_error("Failed to clear active session");
    }
    transaction.commit();
}

void SessionJournal::rename(std::string_view label) {
    Transaction transaction(impl_->database);
    Statement update = impl_->database.prepare(
        "UPDATE sessions SET label = ?1, updated_at = ?2 "
        "WHERE session_key = ?3 AND archived_at IS NULL",
        label, session_timestamp(), impl_->session_key);
    update.run();
    if (impl_->database.changes() != 1) {
        throw std::runtime_error("Failed to rename live session");
    }
    transaction.commit();
}

} // namespace cha
