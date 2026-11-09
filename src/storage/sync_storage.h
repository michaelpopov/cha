#pragma once

#include "storage/sqlite_storage.h"
#include "storage/workspace_session_database.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace cha {

// One new local table holds whole unsent change records. The D1 Worker in
// block 2 sends them; this unit only appends, reads, deletes, and clears.
// Piece splitting for D1 transport also lives here so notes, configuration,
// and chats share one implementation.
inline constexpr std::size_t sync_piece_bytes = 1900000;

// D1 credentials share the saved-key namespace as a key with
// type "D1" in system/keys/<id>/config.toml.
struct SyncD1Credentials {
    std::string key_id;
    std::string account_id;
    std::string database_id;
    std::string api_token;
};

// Caller owns the transaction. Creates sync_queue and adds note_items.changed
// when absent, preserving existing content.
void ensure_sync_schema(storage::SqliteDatabase& database);
// Caller owns the transaction.
void sync_queue_append(storage::SqliteDatabase& database, std::string_view record);

struct QueuedRecord {
    std::int64_t rowid{};
    std::string record;
};
// Oldest first.
[[nodiscard]] std::vector<QueuedRecord> sync_queue_list(storage::SqliteDatabase& database);
// Caller owns the transaction.
void sync_queue_delete(storage::SqliteDatabase& database, std::int64_t rowid);
// Caller owns the transaction. Deletes every record at or below last_rowid.
void sync_queue_delete_through(storage::SqliteDatabase& database, std::int64_t last_rowid);
// Caller owns the transaction. Empties the queue and clears all note flags.
void sync_clear_outgoing(storage::SqliteDatabase& database);
// True when unsent records or flagged notes exist. False on databases that
// predate the sync schema.
[[nodiscard]] bool sync_has_pending(storage::SqliteDatabase& database);

// D1 credential rows are found by their stored content, not by their path
// names, so an ordinary key that reuses a former D1 path is not mistaken
// for credentials.
[[nodiscard]] bool sync_is_d1_row(std::string_view name, std::string_view content);
[[nodiscard]] std::optional<SyncD1Credentials> sync_read_d1(storage::SqliteDatabase& database);
// A vault is connected when the machine is named and D1 credentials exist.
[[nodiscard]] bool sync_connected(storage::SqliteDatabase& database, std::string_view machine);

[[nodiscard]] std::string sync_make_record(
    std::string_view kind, const nlohmann::json& fields);
// Cuts serialized record text at UTF-8 character boundaries.
[[nodiscard]] std::vector<std::string> sync_split_record(std::string_view text);

// Queue table names look like q/<vault>/<machine>. Neither part may hold '/'.
[[nodiscard]] std::string sync_queue_table(
    std::string_view vault, std::string_view machine);
[[nodiscard]] std::string sync_escape_identifier(std::string_view name);
struct ParsedQueueTable {
    std::string vault;
    std::string machine;
};
[[nodiscard]] std::optional<ParsedQueueTable> sync_parse_queue_table(std::string_view table);
[[nodiscard]] bool sync_same_vault(std::string_view left, std::string_view right);
[[nodiscard]] bool sync_same_machine(std::string_view left, std::string_view right);

struct ConfigDelta {
    std::vector<ConfigFile> set;
    std::vector<std::string> names;
};
// Changed ordinary rows only. D1 rows are filtered out of the old and new
// sets separately, so an ordinary key at a former D1 path is still included.
[[nodiscard]] ConfigDelta sync_config_delta(
    const std::vector<ConfigFile>& old_rows,
    const std::vector<ConfigFile>& new_rows);

// Note paths are arrays of names from the top level.
[[nodiscard]] std::vector<std::string> sync_note_path(
    storage::SqliteDatabase& database, std::int64_t id);
[[nodiscard]] std::optional<std::int64_t> sync_note_find(
    storage::SqliteDatabase& database,
    const std::vector<std::string>& path);
// Turns every flagged note into a note.update record, one transaction per
// note, and clears its flag. Returns the number of records created.
std::size_t sync_collect_flagged_notes(
    storage::SqliteDatabase& database, std::string_view machine);

} // namespace cha
