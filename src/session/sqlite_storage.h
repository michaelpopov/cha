#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace cha::storage {

class SqliteStatement;

// Small internal RAII wrapper shared by the legacy session database and the
// workspace migration database. A handle belongs to one thread and therefore
// uses SQLITE_OPEN_NOMUTEX.
class SqliteDatabase final {
public:
    enum class Mode { read_only, read_write, read_write_create };

    SqliteDatabase(const std::filesystem::path& path, Mode mode);
    ~SqliteDatabase();

    SqliteDatabase(const SqliteDatabase&) = delete;
    SqliteDatabase& operator=(const SqliteDatabase&) = delete;
    SqliteDatabase(SqliteDatabase&&) = delete;
    SqliteDatabase& operator=(SqliteDatabase&&) = delete;

    void execute(std::string_view sql);
    [[nodiscard]] SqliteStatement prepare(std::string_view sql);
    template<typename First, typename... Rest>
    [[nodiscard]] SqliteStatement prepare(
        std::string_view sql,
        const First& first,
        const Rest&... rest);

    [[nodiscard]] int changes() const noexcept;
    [[nodiscard]] std::int64_t last_insert_rowid() const noexcept;
    [[nodiscard]] sqlite3* handle() const noexcept { return handle_; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    void rollback_noexcept() noexcept;
    [[nodiscard]] std::int64_t pragma_integer(std::string_view name);

    [[noreturn]] void fail(
        int code,
        std::string_view detail = {}) const;

private:
    sqlite3* handle_{};
    std::string path_;
};

class SqliteStatement final {
public:
    SqliteStatement(SqliteDatabase& database, std::string_view sql);
    template<typename First, typename... Rest>
    SqliteStatement(
        SqliteDatabase& database,
        std::string_view sql,
        const First& first,
        const Rest&... rest)
        : SqliteStatement(database, sql) {
        int index = 0;
        (bind(++index, first), ..., bind(++index, rest));
    }
    ~SqliteStatement();

    SqliteStatement(const SqliteStatement&) = delete;
    SqliteStatement& operator=(const SqliteStatement&) = delete;
    SqliteStatement(SqliteStatement&& other) noexcept;
    SqliteStatement& operator=(SqliteStatement&& other) noexcept;

    void bind(int index, std::int64_t value);
    void bind(int index, std::string_view value);
    void bind_null(int index);
    void bind(int index, const std::optional<std::int64_t>& value);

    [[nodiscard]] bool step();
    void run();
    [[nodiscard]] std::int64_t integer(int column) const;
    [[nodiscard]] std::string text(int column) const;
    [[nodiscard]] bool is_null(int column) const;

private:
    void require(int result) const;

    SqliteDatabase* database_{};
    sqlite3_stmt* statement_{};
};

inline SqliteStatement SqliteDatabase::prepare(std::string_view sql) {
    return SqliteStatement(*this, sql);
}

template<typename First, typename... Rest>
SqliteStatement SqliteDatabase::prepare(
    std::string_view sql,
    const First& first,
    const Rest&... rest) {
    return SqliteStatement(*this, sql, first, rest...);
}

class SqliteTransaction final {
public:
    explicit SqliteTransaction(SqliteDatabase& database);
    ~SqliteTransaction();

    SqliteTransaction(const SqliteTransaction&) = delete;
    SqliteTransaction& operator=(const SqliteTransaction&) = delete;

    void commit();

private:
    SqliteDatabase* database_{};
};

} // namespace cha::storage
