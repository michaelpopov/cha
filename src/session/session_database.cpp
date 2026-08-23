#include "session/session_database.h"

#include "util/path_name.h"

#include <sqlite3.h>
#include <uv.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
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

class Statement;

// SQLite runs one-time library initialization on the first open, and that path
// writes its configuration globals before the mutex protecting them exists.
// Two session owner threads creating their first database concurrently
// therefore race, which ThreadSanitizer reports against sqlite3Config inside
// sqlite3MutexInit. SQLite's own guidance is to initialize once before any
// other interface is used; call_once both serializes the first call and gives
// every later thread the happens-before edge the library's fast path omits.
void initialize_sqlite_once() {
    static std::once_flag flag;
    static int status = SQLITE_OK;
    std::call_once(flag, [] {
        status = sqlite3_initialize();
        if (status != SQLITE_OK) return;
        // The unix VFS records the owning process id the first time it draws
        // randomness, and every open re-checks it to detect a fork. Left
        // unseeded, concurrent first opens both see the stale value and both
        // write it, which SQLite documents as a harmless race but which is
        // still a race. Drawing one byte here, while call_once still
        // serializes us, leaves every later open with a read-only check.
        // The zero-length form would return before reaching the VFS.
        unsigned char seed = 0;
        sqlite3_randomness(1, &seed);
    });
    if (status != SQLITE_OK) {
        throw std::runtime_error(
            std::string("Failed to initialize SQLite: ")
            + sqlite3_errstr(status));
    }
}

class Database {
public:
    enum class Mode { read_only, read_write, read_write_create };

    Database(const std::filesystem::path& path, Mode mode)
        : path_(utf8_path(path)) {
        initialize_sqlite_once();
        int flags = mode == Mode::read_only ? SQLITE_OPEN_READONLY : SQLITE_OPEN_READWRITE;
        if (mode == Mode::read_write_create) {
            flags |= SQLITE_OPEN_CREATE;
        }
        const int result = sqlite3_open_v2(
            path_.c_str(), &handle_, flags | SQLITE_OPEN_NOMUTEX, nullptr);
        if (result != SQLITE_OK) {
            const std::string detail =
                handle_ ? sqlite3_errmsg(handle_) : sqlite3_errstr(result);
            if (handle_) {
                sqlite3_close_v2(handle_);
                handle_ = nullptr;
            }
            throw std::runtime_error(
                "Failed to open session database '" + path_ + "': " + detail
                + " (SQLite code " + std::to_string(result) + ")");
        }
        try {
            sqlite3_extended_result_codes(handle_, 1);
            sqlite3_busy_timeout(handle_, 5000);
            execute("PRAGMA foreign_keys = ON");
            if (mode != Mode::read_only) {
                execute("PRAGMA synchronous = FULL");
            }
            if (pragma_integer("foreign_keys") != 1) {
                throw std::runtime_error(
                    "Failed to enable foreign keys for session database '"
                    + path_ + "'");
            }
        } catch (...) {
            sqlite3_close_v2(handle_);
            handle_ = nullptr;
            throw;
        }
    }

    ~Database() { sqlite3_close_v2(handle_); }

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    Database(Database&&) = delete;
    Database& operator=(Database&&) = delete;

    void execute(std::string_view sql) {
        char* error = nullptr;
        const int result = sqlite3_exec(
            handle_, std::string(sql).c_str(), nullptr, nullptr, &error);
        if (result != SQLITE_OK) {
            const std::string detail = error ? error : sqlite3_errmsg(handle_);
            sqlite3_free(error);
            fail(result, detail);
        }
    }

    Statement prepare(std::string_view sql);
    template<typename First, typename... Rest>
    Statement prepare(
        std::string_view sql,
        const First& first,
        const Rest&... rest);
    int changes() const noexcept { return sqlite3_changes(handle_); }
    sqlite3* handle() const noexcept { return handle_; }
    const std::string& path() const noexcept { return path_; }
    void rollback_noexcept() noexcept {
        (void)sqlite3_exec(handle_, "ROLLBACK", nullptr, nullptr, nullptr);
    }

    [[noreturn]] void fail(int code, std::string_view detail = {}) const {
        const std::string message = detail.empty()
            ? sqlite3_errmsg(handle_)
            : std::string(detail);
        throw std::runtime_error(
            "Session database '" + path_ + "': " + message
            + " (SQLite code " + std::to_string(code) + ")");
    }

    std::int64_t pragma_integer(std::string_view name);

private:
    sqlite3* handle_{};
    std::string path_;
};

class Statement {
public:
    Statement(Database& database, std::string_view sql)
        : database_(&database) {
        const std::string source(sql);
        const int result = sqlite3_prepare_v2(
            database.handle(), source.c_str(), -1, &statement_, nullptr);
        if (result != SQLITE_OK) {
            database.fail(result);
        }
    }

    ~Statement() { sqlite3_finalize(statement_); }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&& other) noexcept
        : database_(other.database_),
          statement_(other.statement_) {
        other.database_ = nullptr;
        other.statement_ = nullptr;
    }
    Statement& operator=(Statement&& other) noexcept {
        if (this != &other) {
            sqlite3_finalize(statement_);
            database_ = other.database_;
            statement_ = other.statement_;
            other.database_ = nullptr;
            other.statement_ = nullptr;
        }
        return *this;
    }

    template<typename First, typename... Rest>
    Statement(
        Database& database,
        std::string_view sql,
        const First& first,
        const Rest&... rest)
        : Statement(database, sql) {
        int index = 0;
        (bind(++index, first), ..., bind(++index, rest));
    }

    void bind(int index, std::int64_t value) {
        require(sqlite3_bind_int64(statement_, index, value));
    }

    void bind(int index, std::string_view value) {
        require(sqlite3_bind_text64(
            statement_,
            index,
            value.data(),
            static_cast<sqlite3_uint64>(value.size()),
            SQLITE_TRANSIENT,
            SQLITE_UTF8));
    }

    void bind_null(int index) { require(sqlite3_bind_null(statement_, index)); }
    void bind(int index, const std::optional<std::int64_t>& value) {
        value ? bind(index, *value) : bind_null(index);
    }

    bool step() {
        const int result = sqlite3_step(statement_);
        if (result == SQLITE_ROW) return true;
        if (result == SQLITE_DONE) return false;
        database_->fail(result);
    }

    void run() {
        if (step()) {
            throw std::runtime_error(
                "SQLite statement unexpectedly returned a row for '" + database_->path() + "'");
        }
    }

    std::int64_t integer(int column) const {
        return sqlite3_column_int64(statement_, column);
    }

    std::string text(int column) const {
        const unsigned char* value = sqlite3_column_text(statement_, column);
        if (!value) {
            throw std::runtime_error(
                "Unexpected NULL text in session database '" + database_->path() + "'");
        }
        const int size = sqlite3_column_bytes(statement_, column);
        return {
            reinterpret_cast<const char*>(value),
            static_cast<std::size_t>(size),
        };
    }

    bool is_null(int column) const {
        return sqlite3_column_type(statement_, column) == SQLITE_NULL;
    }

    int column_count() const noexcept {
        return sqlite3_column_count(statement_);
    }

    int column_type(int column) const noexcept {
        return sqlite3_column_type(statement_, column);
    }

    double real(int column) const noexcept {
        return sqlite3_column_double(statement_, column);
    }

    std::string_view text_view(int column) const {
        const unsigned char* value = sqlite3_column_text(statement_, column);
        const int size = sqlite3_column_bytes(statement_, column);
        if (size == 0) return {};
        if (!value) database_->fail(SQLITE_NOMEM);
        return {
            reinterpret_cast<const char*>(value),
            static_cast<std::size_t>(size),
        };
    }

    std::string_view blob_view(int column) const {
        const void* value = sqlite3_column_blob(statement_, column);
        const int size = sqlite3_column_bytes(statement_, column);
        if (size == 0) return {};
        if (!value) database_->fail(SQLITE_NOMEM);
        return {
            static_cast<const char*>(value),
            static_cast<std::size_t>(size),
        };
    }

private:
    void require(int result) const {
        if (result != SQLITE_OK) {
            database_->fail(result);
        }
    }

    Database* database_{};
    sqlite3_stmt* statement_{};
};

Statement Database::prepare(std::string_view sql) {
    return Statement(*this, sql);
}

template<typename First, typename... Rest>
Statement Database::prepare(
    std::string_view sql,
    const First& first,
    const Rest&... rest) {
    return Statement(*this, sql, first, rest...);
}

std::int64_t Database::pragma_integer(std::string_view name) {
    Statement statement = prepare("PRAGMA " + std::string(name));
    if (!statement.step()) {
        throw std::runtime_error(
            "PRAGMA " + std::string(name) + " returned no value for '" + path_ + "'");
    }
    return statement.integer(0);
}

class Transaction {
public:
    explicit Transaction(Database& database)
        : database_(&database) {
        database_->execute("BEGIN IMMEDIATE");
    }
    ~Transaction() {
        if (database_) {
            database_->rollback_noexcept();
        }
    }

    void commit() {
        database_->execute("COMMIT");
        database_ = nullptr;
    }

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

private:
    Database* database_{};
};

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

std::string quoted_identifier(std::string_view identifier) {
    std::string result = "\"";
    for (const char character : identifier) {
        result += character;
        if (character == '"') result += '"';
    }
    return result + "\"";
}

std::string hex_literal(std::string_view value, bool as_text) {
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string result = as_text ? "CAST(X'" : "X'";
    result.reserve(result.size() + value.size() * 2 + (as_text ? 10 : 1));
    for (const unsigned char byte : value) {
        result += digits[byte >> 4];
        result += digits[byte & 0x0f];
    }
    return result + (as_text ? "' AS TEXT)" : "'");
}

std::string quoted_text(std::string_view value) {
    if (value.find('\0') != std::string_view::npos) {
        return hex_literal(value, true);
    }
    std::string result = "'";
    for (const char character : value) {
        result += character;
        if (character == '\'') result += '\'';
    }
    return result + "'";
}

std::string sql_value(const Statement& statement, int column) {
    switch (statement.column_type(column)) {
    case SQLITE_NULL:
        return "NULL";
    case SQLITE_INTEGER:
        return std::to_string(statement.integer(column));
    case SQLITE_FLOAT: {
        const double value = statement.real(column);
        if (std::isnan(value)) return "NULL";
        if (std::isinf(value)) return value < 0 ? "-9.0e+999" : "9.0e+999";
        char buffer[64];
        sqlite3_snprintf(sizeof buffer, buffer, "%!.17g", value);
        return buffer;
    }
    case SQLITE_TEXT:
        return quoted_text(statement.text_view(column));
    case SQLITE_BLOB:
        return hex_literal(statement.blob_view(column), false);
    default:
        throw std::runtime_error("SQLite returned an unknown value type while exporting a session");
    }
}

struct SchemaObject {
    std::string type;
    std::string name;
    std::string sql;
};

std::string dump_session_database(
    Database& database,
    const FullSessionId& expected_identity) {
    database.execute("BEGIN");
    try {
        validate_database(database);
        const SessionDatabaseMetadata metadata = read_metadata(database);
        validate_session_database_identity(
            path_from_utf8(database.path()), expected_identity, metadata);
        const std::int64_t application_id =
            database.pragma_integer("application_id");
        const std::int64_t user_version =
            database.pragma_integer("user_version");

        std::vector<SchemaObject> objects;
        Statement schema = database.prepare(
            "SELECT type, name, sql FROM sqlite_schema "
            "WHERE sql IS NOT NULL AND name NOT LIKE 'sqlite_%' "
            "ORDER BY CASE type WHEN 'table' THEN 0 WHEN 'index' THEN 1 "
            "WHEN 'trigger' THEN 2 WHEN 'view' THEN 3 ELSE 4 END, rowid");
        while (schema.step()) {
            objects.push_back({schema.text(0), schema.text(1), schema.text(2)});
        }

        std::string dump =
            "PRAGMA foreign_keys = OFF;\n"
            "BEGIN TRANSACTION;\n";
        for (const SchemaObject& object : objects) {
            if (object.type == "table") dump += object.sql + ";\n";
        }
        for (const SchemaObject& object : objects) {
            if (object.type != "table") continue;
            const std::string table = quoted_identifier(object.name);
            Statement rows = database.prepare(
                "SELECT * FROM " + table + " ORDER BY rowid");
            while (rows.step()) {
                dump += "INSERT INTO " + table + " VALUES(";
                for (int column = 0; column != rows.column_count(); ++column) {
                    if (column != 0) dump += ',';
                    dump += sql_value(rows, column);
                }
                dump += ");\n";
            }
        }
        for (const SchemaObject& object : objects) {
            if (object.type != "table") dump += object.sql + ";\n";
        }
        dump += "PRAGMA application_id = "
            + std::to_string(application_id) + ";\n";
        dump += "PRAGMA user_version = "
            + std::to_string(user_version) + ";\n";
        dump += "COMMIT;\nPRAGMA foreign_keys = ON;\n";
        database.execute("COMMIT");
        return dump;
    } catch (...) {
        database.rollback_noexcept();
        throw;
    }
}

void replace_path(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) {
    uv_fs_t request{};
    const std::string source_text = utf8_path(source);
    const std::string destination_text = utf8_path(destination);
    const int status = uv_fs_rename(
        nullptr,
        &request,
        source_text.c_str(),
        destination_text.c_str(),
        nullptr);
    uv_fs_req_cleanup(&request);
    if (status < 0) {
        throw std::runtime_error(
            "Failed to replace SQL snapshot '" + destination_text
            + "': " + uv_strerror(status));
    }
}

void write_file_atomically(
    const std::filesystem::path& path,
    std::string_view contents) {
    const std::filesystem::path temporary_path = create_temporary_path(path);
    try {
        {
            std::ofstream output(
                temporary_path,
                std::ios::binary | std::ios::trunc);
            output.write(
                contents.data(),
                static_cast<std::streamsize>(contents.size()));
            output.close();
            if (!output) {
                throw std::runtime_error(
                    "Failed to write SQL snapshot '" + utf8_path(path) + "'");
            }
        }
        replace_path(temporary_path, path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
        throw;
    }
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "Failed to read SQL snapshot '" + utf8_path(path) + "'");
    }
    std::string result{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    if (input.bad()) {
        throw std::runtime_error(
            "Failed to read SQL snapshot '" + utf8_path(path) + "'");
    }
    return result;
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

void export_session_database_sql(
    const std::filesystem::path& database_path,
    const std::filesystem::path& sql_path,
    const FullSessionId& expected_identity) {
    Database database(database_path, Database::Mode::read_only);
    write_file_atomically(
        sql_path,
        dump_session_database(database, expected_identity));
}

bool import_session_database_sql(
    const std::filesystem::path& sql_path,
    const std::filesystem::path& database_path,
    const FullSessionId& expected_identity) {
    const std::filesystem::path temporary_path =
        create_temporary_path(database_path);
    try {
        {
            Database database(
                temporary_path,
                Database::Mode::read_write_create);
            database.execute(read_file(sql_path));
        }
        migrate_session_database(temporary_path);
        (void)load_session_database(temporary_path, expected_identity);
        const bool published =
            publish_database_path(temporary_path, database_path);
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
