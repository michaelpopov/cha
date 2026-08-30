#include "session/sqlite_storage.h"

#include "util/path_name.h"

#include <mutex>
#include <stdexcept>
#include <utility>

namespace cha::storage {
namespace {

void initialize_sqlite_once() {
    static std::once_flag flag;
    static int status = SQLITE_OK;
    std::call_once(flag, [] {
        status = sqlite3_initialize();
        if (status != SQLITE_OK) return;
        unsigned char seed = 0;
        sqlite3_randomness(1, &seed);
    });
    if (status != SQLITE_OK) {
        throw std::runtime_error(
            std::string("Failed to initialize SQLite: ")
            + sqlite3_errstr(status));
    }
}

} // namespace

SqliteDatabase::SqliteDatabase(
    const std::filesystem::path& path,
    Mode mode)
    : path_(utf8_path(path)) {
    initialize_sqlite_once();
    int flags = mode == Mode::read_only
        ? SQLITE_OPEN_READONLY
        : SQLITE_OPEN_READWRITE;
    if (mode == Mode::read_write_create) flags |= SQLITE_OPEN_CREATE;
    const int result = sqlite3_open_v2(
        path_.c_str(),
        &handle_,
        flags | SQLITE_OPEN_NOMUTEX,
        nullptr);
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
        if (mode != Mode::read_only) execute("PRAGMA synchronous = FULL");
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

SqliteDatabase::~SqliteDatabase() {
    sqlite3_close_v2(handle_);
}

void SqliteDatabase::execute(std::string_view sql) {
    char* error = nullptr;
    const int result = sqlite3_exec(
        handle_, std::string(sql).c_str(), nullptr, nullptr, &error);
    if (result != SQLITE_OK) {
        const std::string detail = error ? error : sqlite3_errmsg(handle_);
        sqlite3_free(error);
        fail(result, detail);
    }
}

int SqliteDatabase::changes() const noexcept {
    return sqlite3_changes(handle_);
}

std::int64_t SqliteDatabase::last_insert_rowid() const noexcept {
    return sqlite3_last_insert_rowid(handle_);
}

void SqliteDatabase::rollback_noexcept() noexcept {
    (void)sqlite3_exec(handle_, "ROLLBACK", nullptr, nullptr, nullptr);
}

[[noreturn]] void SqliteDatabase::fail(
    int code,
    std::string_view detail) const {
    const std::string message = detail.empty()
        ? sqlite3_errmsg(handle_)
        : std::string(detail);
    throw std::runtime_error(
        "Session database '" + path_ + "': " + message
        + " (SQLite code " + std::to_string(code) + ")");
}

std::int64_t SqliteDatabase::pragma_integer(std::string_view name) {
    SqliteStatement statement = prepare("PRAGMA " + std::string(name));
    if (!statement.step()) {
        throw std::runtime_error(
            "PRAGMA " + std::string(name) + " returned no value for '"
            + path_ + "'");
    }
    return statement.integer(0);
}

SqliteStatement::SqliteStatement(
    SqliteDatabase& database,
    std::string_view sql)
    : database_(&database) {
    const std::string source(sql);
    const int result = sqlite3_prepare_v2(
        database.handle(), source.c_str(), -1, &statement_, nullptr);
    if (result != SQLITE_OK) database.fail(result);
}

SqliteStatement::~SqliteStatement() {
    sqlite3_finalize(statement_);
}

SqliteStatement::SqliteStatement(SqliteStatement&& other) noexcept
    : database_(other.database_),
      statement_(other.statement_) {
    other.database_ = nullptr;
    other.statement_ = nullptr;
}

SqliteStatement& SqliteStatement::operator=(SqliteStatement&& other) noexcept {
    if (this != &other) {
        sqlite3_finalize(statement_);
        database_ = other.database_;
        statement_ = other.statement_;
        other.database_ = nullptr;
        other.statement_ = nullptr;
    }
    return *this;
}

void SqliteStatement::bind(int index, std::int64_t value) {
    require(sqlite3_bind_int64(statement_, index, value));
}

void SqliteStatement::bind(int index, std::string_view value) {
    require(sqlite3_bind_text64(
        statement_,
        index,
        value.data(),
        static_cast<sqlite3_uint64>(value.size()),
        SQLITE_TRANSIENT,
        SQLITE_UTF8));
}

void SqliteStatement::bind_null(int index) {
    require(sqlite3_bind_null(statement_, index));
}

void SqliteStatement::bind(
    int index,
    const std::optional<std::int64_t>& value) {
    value ? bind(index, *value) : bind_null(index);
}

bool SqliteStatement::step() {
    const int result = sqlite3_step(statement_);
    if (result == SQLITE_ROW) return true;
    if (result == SQLITE_DONE) return false;
    database_->fail(result);
}

void SqliteStatement::run() {
    if (step()) {
        throw std::runtime_error(
            "SQLite statement unexpectedly returned a row for '"
            + database_->path() + "'");
    }
}

std::int64_t SqliteStatement::integer(int column) const {
    return sqlite3_column_int64(statement_, column);
}

std::string SqliteStatement::text(int column) const {
    const unsigned char* value = sqlite3_column_text(statement_, column);
    if (!value) {
        throw std::runtime_error(
            "Unexpected NULL text in session database '"
            + database_->path() + "'");
    }
    const int size = sqlite3_column_bytes(statement_, column);
    return {
        reinterpret_cast<const char*>(value),
        static_cast<std::size_t>(size),
    };
}

bool SqliteStatement::is_null(int column) const {
    return sqlite3_column_type(statement_, column) == SQLITE_NULL;
}

void SqliteStatement::require(int result) const {
    if (result != SQLITE_OK) database_->fail(result);
}

SqliteTransaction::SqliteTransaction(SqliteDatabase& database)
    : database_(&database) {
    database_->execute("BEGIN IMMEDIATE");
}

SqliteTransaction::~SqliteTransaction() {
    if (database_) database_->rollback_noexcept();
}

void SqliteTransaction::commit() {
    database_->execute("COMMIT");
    database_ = nullptr;
}

} // namespace cha::storage
