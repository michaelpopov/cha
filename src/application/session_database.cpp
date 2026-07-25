#include "application/session_database.h"

#include <sqlite3.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace cha {
namespace {

constexpr int session_application_id = 0x43484131; // "CHA1"
constexpr int session_database_version = 2;

static_assert(static_cast<std::int64_t>(EntryKind::human) == 0);
static_assert(static_cast<std::int64_t>(EntryKind::agent) == 1);
static_assert(static_cast<std::int64_t>(EntryKind::notice) == 2);
static_assert(static_cast<std::int64_t>(EntryKind::error) == 3);
static_assert(static_cast<std::int64_t>(CompletionStatus::complete) == 0);
static_assert(static_cast<std::int64_t>(CompletionStatus::streaming) == 1);
static_assert(static_cast<std::int64_t>(CompletionStatus::cancelled) == 2);
static_assert(static_cast<std::int64_t>(CompletionStatus::failed) == 3);

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

class Database {
public:
    enum class Mode { read_only, read_write, read_write_create };

    Database(const std::filesystem::path& path, Mode mode)
        : path_(path.string()) {
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
            room TEXT NOT NULL CHECK (room <> ''),
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
        unsigned_id(state.integer(1), "next conversation entry ID"),
        unsigned_id(state.integer(2), "next request ID"),
    };
}

void validate_database_identity(Database& database) {
    if (database.pragma_integer("application_id") != session_application_id
        || database.pragma_integer("user_version") != session_database_version) {
        throw std::runtime_error(
            "Unsupported session database '" + database.path() + "'");
    }
}

void validate_database(Database& database) {
    validate_database_identity(database);
    (void)read_state(database);
    Statement turns_without_prompt = database.prepare(
        "SELECT t.request_id FROM turns AS t LEFT JOIN entries AS e "
        "ON e.request_id = t.request_id AND e.epoch = t.epoch AND e.kind = 0 "
        "GROUP BY t.request_id HAVING COUNT(e.entry_id) <> 1 LIMIT 1");
    if (turns_without_prompt.step()) {
        throw std::runtime_error("Session database '" + database.path() + "' has a turn without exactly one prompt");
    }
}

EntryKind parse_kind(std::int64_t value) {
    if (value < 0 || value > 3) {
        throw std::runtime_error("Session database contains an unknown entry kind");
    }
    return static_cast<EntryKind>(value);
}

CompletionStatus parse_status(std::int64_t value) {
    if (value != 0 && value != 2 && value != 3) {
        throw std::runtime_error("Session database contains an unknown entry status");
    }
    return static_cast<CompletionStatus>(value);
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
    const ConversationEntry& entry) {

    EntryKind expected_kind = EntryKind::human;
    CompletionStatus expected_status = CompletionStatus::complete;
    switch (record_kind) {
    case TurnRecordKind::started:
        break;
    case TurnRecordKind::completed:
        expected_kind = EntryKind::agent;
        break;
    case TurnRecordKind::cancelled:
        expected_kind = EntryKind::agent;
        expected_status = CompletionStatus::cancelled;
        break;
    case TurnRecordKind::failed:
        expected_kind = EntryKind::error;
        expected_status = CompletionStatus::failed;
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
    const std::int64_t value = sqlite_id(entry_id, "Conversation entry ID");
    Statement statement = database.prepare(
        "UPDATE state SET next_entry_id = ?1 + 1 "
        "WHERE singleton = 1 AND next_entry_id <= ?1",
        value);
    statement.run();
    if (database.changes() != 1) {
        throw std::invalid_argument(
            "Conversation entry IDs must be strictly increasing");
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
    const ConversationEntry& entry) {

    try {
        require_storable_conversation_entry(entry);
    } catch (const std::invalid_argument& error) {
        throw std::runtime_error(
            "Cannot store invalid conversation entry: "
            + std::string(error.what()));
    }
    advance_entry_id(database, entry.id);

    const std::optional<std::int64_t> request_id = entry.request_id
        ? std::optional(sqlite_id(*entry.request_id, "Request ID"))
        : std::nullopt;
    Statement statement = database.prepare(
        "INSERT INTO entries ("
        "entry_id, epoch, request_id, kind, participant_id, display_name, addressed_to, addressed_to_name, text, status"
        ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)",
        sqlite_id(entry.id, "Conversation entry ID"),
        epoch,
        request_id,
        static_cast<std::int64_t>(entry.kind),
        std::string_view(entry.participant_id),
        std::string_view(entry.display_name),
        std::string_view(entry.addressed_to),
        std::string_view(entry.addressed_to_name),
        std::string_view(entry.text),
        static_cast<std::int64_t>(entry.status));
    statement.run();
}

ConversationEntry read_entry(Statement& statement) {
    ConversationEntry entry{
        .id = unsigned_id(statement.integer(0), "conversation entry ID"),
        .kind = parse_kind(statement.integer(2)),
        .participant_id = statement.text(3),
        .display_name = statement.text(4),
        .addressed_to = statement.text(5),
        .addressed_to_name = statement.text(6),
        .text = statement.text(7),
        .status = parse_status(statement.integer(8)),
    };
    if (!statement.is_null(1)) {
        entry.request_id = unsigned_id(statement.integer(1), "request ID");
    }
    try {
        require_storable_conversation_entry(entry);
    } catch (const std::invalid_argument& error) {
        throw std::runtime_error(
            "Session database contains an invalid conversation entry: "
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
    const ConversationEntry* entry) {

    Transaction transaction(database);
    transition_turn(database, request_id, state);
    if (entry) {
        insert_entry(database, current_epoch(database), *entry);
    }
    transaction.commit();
}

std::filesystem::path create_temporary_database_path(
    const std::filesystem::path& path) {

    const std::filesystem::path pattern_path =
        path.parent_path()
        / ("." + path.filename().string() + ".tmp.XXXXXX");
    const std::string pattern = pattern_path.string();
    std::vector<char> writable_pattern(pattern.begin(), pattern.end());
    writable_pattern.push_back('\0');

    const int descriptor = ::mkstemp(writable_pattern.data());
    if (descriptor == -1) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "Failed to create a temporary session database for '"
                + path.string() + "'");
    }
    const std::filesystem::path temporary_path(writable_pattern.data());
    if (::close(descriptor) == -1) {
        const int error = errno;
        std::error_code ignored;
        std::filesystem::remove(temporary_path, ignored);
        throw std::system_error(
            error,
            std::generic_category(),
            "Failed to close temporary session database '"
                + temporary_path.string() + "'");
    }
    return temporary_path;
}

bool publish_database_path(
    const std::filesystem::path& temporary_path,
    const std::filesystem::path& path) {

    if (::link(temporary_path.c_str(), path.c_str()) == 0) {
        return true;
    }
    if (errno == EEXIST) {
        return false;
    }
    throw std::system_error(
        errno,
        std::generic_category(),
        "Failed to publish session database '" + path.string() + "'");
}

} // namespace

bool create_session_database(
    const std::filesystem::path& path,
    const SessionDatabaseMetadata& metadata) {

    if (metadata.id.empty() || metadata.room.empty()
        || metadata.label.empty()) {
        throw std::invalid_argument(
            "Session database metadata fields cannot be empty");
    }
    const std::filesystem::path temporary_path =
        create_temporary_database_path(path);

    try {
        {
            Database database(
                temporary_path,
                Database::Mode::read_write_create);
            Transaction transaction(database);
            create_schema(database);
            Statement statement = database.prepare(
                "INSERT INTO session (singleton, id, room, label) "
                "VALUES (1, ?1, ?2, ?3)",
                std::string_view(metadata.id),
                std::string_view(metadata.room),
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

SessionDatabaseMetadata read_session_database_metadata(
    const std::filesystem::path& path) {

    Database database(path, Database::Mode::read_only);
    // Listing sessions needs only stable identity metadata; opening one later
    // performs the full transcript validation below.
    validate_database_identity(database);
    Statement statement = database.prepare(
        "SELECT id, room, label FROM session WHERE singleton = 1");
    if (!statement.step()) {
        throw std::runtime_error(
            "Session database '" + path.string() + "' has no metadata");
    }
    return {
        .id = statement.text(0),
        .room = statement.text(1),
        .label = statement.text(2),
    };
}

ConversationRestore load_conversation_state(
    const std::filesystem::path& path) {

    Database database(path, Database::Mode::read_only);
    validate_database(database);

    const DurableState state = read_state(database);
    ConversationRestore result{
        .next_request_id = state.next_request_id,
        .next_entry_id = state.next_entry_id,
    };

    Statement entries = database.prepare(
        "SELECT entry_id, request_id, kind, participant_id, display_name, addressed_to, addressed_to_name, text, status "
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
        ConversationEntry error = make_error_entry(
            result.next_entry_id++,
            "Response interrupted before completion",
            request_id,
            interrupted.text(1));
        result.interrupted_turns.push_back({request_id, std::move(error)});
    }
    return result;
}

std::vector<ConversationEntry> load_conversation_entries(
    const std::filesystem::path& path) {
    return load_conversation_state(path).entries;
}

class ConversationJournal::Impl {
public:
    explicit Impl(const std::filesystem::path& path)
        : database(path, Database::Mode::read_write) {
        validate_database(database);
    }

    Database database;
};

ConversationJournal::ConversationJournal(std::filesystem::path path)
    : impl_(std::make_unique<Impl>(path)) {
}

ConversationJournal::~ConversationJournal() = default;

void ConversationJournal::append(const ConversationEntry& entry) {
    Transaction transaction(impl_->database);
    insert_entry(impl_->database, current_epoch(impl_->database), entry);
    transaction.commit();
}

void ConversationJournal::start_turn(
    RequestId request_id,
    const ConversationEntry& prompt) {
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

void ConversationJournal::complete_turn(
    RequestId request_id,
    const ConversationEntry& response) {

    validate_turn_entry(TurnRecordKind::completed, request_id, response);
    finish_turn(
        impl_->database,
        request_id,
        TurnState::completed,
        &response);
}

void ConversationJournal::cancel_turn(
    RequestId request_id,
    std::optional<ConversationEntry> response) {

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

void ConversationJournal::fail_turn(
    RequestId request_id,
    const ConversationEntry& error) {

    validate_turn_entry(TurnRecordKind::failed, request_id, error);
    finish_turn(
        impl_->database,
        request_id,
        TurnState::failed,
        &error);
}

void ConversationJournal::clear() {
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

} // namespace cha
