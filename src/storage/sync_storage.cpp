#include "storage/sync_storage.h"

#include "app/application_config.h"
#include "providers/credentials.h"
#include "storage/note_storage.h"
#include "util/base64.h"
#include "util/text.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace cha {
namespace {

bool has_table(storage::SqliteDatabase& database, std::string_view name) {
    auto check = database.prepare(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1", name);
    return check.step();
}

bool has_note_changed_column(storage::SqliteDatabase& database) {
    auto columns = database.prepare("PRAGMA table_info(note_items)");
    while (columns.step()) {
        if (columns.text(1) == "changed") return true;
    }
    return false;
}

bool d1_type(std::string_view content) {
    try {
        const toml::table table = toml::parse(std::string(content));
        const auto type = table["type"].value<std::string>();
        return type && *type == "D1";
    } catch (const toml::parse_error&) {
        return false;
    }
}

bool is_key_config(std::string_view name) {
    constexpr std::string_view prefix = "system/keys/";
    constexpr std::string_view suffix = "/config.toml";
    if (name.size() <= prefix.size() + suffix.size()
        || !name.starts_with(prefix) || !name.ends_with(suffix)) return false;
    const auto middle = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    return !middle.empty() && middle.find('/') == std::string_view::npos;
}

nlohmann::json note_fields(
    storage::SqliteDatabase& database, std::int64_t id) {
    auto query = database.prepare(
        "SELECT n.content_json, c.content_json, n.scroll_top, n.read_only "
        "FROM note_items n LEFT JOIN note_items c ON c.parent_id=n.id AND c.compressed=1 "
        "WHERE n.id=?1 AND n.kind='note' AND n.compressed=0",
        id);
    if (!query.step()) throw std::runtime_error("Sync note is missing.");
    nlohmann::json fields;
    fields["content"] = encode_base64(query.blob(0));
    if (query.is_null(1)) fields["compressed_content"] = nullptr;
    else fields["compressed_content"] = encode_base64(query.blob(1));
    fields["scroll_top"] = query.integer(2);
    fields["read_only"] = query.integer(3) != 0;
    return fields;
}

} // namespace

void ensure_sync_schema(storage::SqliteDatabase& database) {
    database.execute(
        "CREATE TABLE IF NOT EXISTS sync_queue (record TEXT NOT NULL) STRICT");
    if (has_table(database, "note_items") && !has_note_changed_column(database)) {
        database.execute(
            "ALTER TABLE note_items ADD COLUMN changed INTEGER NOT NULL "
            "DEFAULT 0 CHECK (changed IN (0, 1))");
    }
}

void sync_queue_append(storage::SqliteDatabase& database, std::string_view record) {
    database.prepare("INSERT INTO sync_queue (record) VALUES (?1)", record).run();
}

std::vector<QueuedRecord> sync_queue_list(storage::SqliteDatabase& database) {
    if (!has_table(database, "sync_queue")) return {};
    std::vector<QueuedRecord> rows;
    auto query = database.prepare("SELECT rowid, record FROM sync_queue ORDER BY rowid");
    while (query.step()) rows.push_back({query.integer(0), query.text(1)});
    return rows;
}

void sync_queue_delete(storage::SqliteDatabase& database, std::int64_t rowid) {
    database.prepare("DELETE FROM sync_queue WHERE rowid=?1", rowid).run();
}

void sync_clear_outgoing(storage::SqliteDatabase& database) {
    ensure_sync_schema(database);
    database.prepare("DELETE FROM sync_queue").run();
    if (has_table(database, "note_items"))
        database.prepare("UPDATE note_items SET changed=0 WHERE changed<>0").run();
}

bool sync_has_pending(storage::SqliteDatabase& database) {
    if (has_table(database, "sync_queue")) {
        auto queued = database.prepare("SELECT 1 FROM sync_queue LIMIT 1");
        if (queued.step()) return true;
    }
    if (has_table(database, "note_items") && has_note_changed_column(database)) {
        auto flagged = database.prepare("SELECT 1 FROM note_items WHERE changed<>0 LIMIT 1");
        if (flagged.step()) return true;
    }
    return false;
}

bool sync_is_d1_row(std::string_view name, std::string_view content) {
    return is_key_config(name) && d1_type(content);
}

std::optional<SyncD1Credentials> sync_read_d1(storage::SqliteDatabase& database) {
    if (!has_table(database, "config")) return std::nullopt;
    auto rows = database.prepare(
        "SELECT name, content FROM config WHERE name GLOB 'system/keys/*/config.toml'");
    while (rows.step()) {
        const auto name = rows.text(0);
        if (!is_key_config(name)) continue;
        try {
            const toml::table table = toml::parse(rows.text(1));
            const auto type = table["type"].value<std::string>();
            if (!type || *type != "D1") continue;
            SyncD1Credentials credentials;
            const auto slash = name.rfind('/');
            credentials.key_id = name.substr(
                std::string_view("system/keys/").size(),
                slash - std::string_view("system/keys/").size());
            credentials.account_id = table["account_id"].value_or(std::string{});
            credentials.database_id = table["database_id"].value_or(std::string{});
            credentials.api_token = table["api_token"].value_or(std::string{});
            if (!valid_d1_credential_field(credentials.account_id)
                || !valid_d1_credential_field(credentials.database_id)
                || !valid_d1_credential_field(credentials.api_token, false))
                return std::nullopt;
            return credentials;
        } catch (const toml::parse_error&) {
            continue;
        }
    }
    return std::nullopt;
}

bool sync_connected(storage::SqliteDatabase& database, std::string_view machine) {
    if (!valid_machine_name(machine)) return false;
    return sync_read_d1(database).has_value();
}

std::string sync_make_record(std::string_view kind, const nlohmann::json& fields) {
    nlohmann::json record = fields;
    record["kind"] = kind;
    return record.dump();
}

std::vector<std::string> sync_split_record(std::string_view text) {
    std::vector<std::string> pieces;
    std::size_t offset = 0;
    while (offset < text.size()) {
        std::size_t end = std::min(text.size(), offset + sync_piece_bytes);
        // A boundary is valid only before a character start. A UTF-8
        // continuation byte is 0x80-0xBF; back off past the whole character.
        while (end > offset && end < text.size()
            && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
        if (end == offset) end = std::min(text.size(), offset + sync_piece_bytes);
        pieces.emplace_back(text.substr(offset, end - offset));
        offset = end;
    }
    if (pieces.empty()) pieces.emplace_back("");
    return pieces;
}

std::string sync_escape_identifier(std::string_view name) {
    std::string escaped;
    escaped.reserve(name.size() + 2);
    escaped.push_back('"');
    for (const char c : name) {
        if (c == '"') escaped.push_back('"');
        escaped.push_back(c);
    }
    escaped.push_back('"');
    return escaped;
}

std::string sync_queue_table(std::string_view vault, std::string_view machine) {
    std::string name = "q/";
    name += vault;
    name += '/';
    name += machine;
    return name;
}

std::optional<ParsedQueueTable> sync_parse_queue_table(std::string_view table) {
    if (!table.starts_with("q/")) return std::nullopt;
    const auto rest = table.substr(2);
    const auto slash = rest.find('/');
    if (slash == std::string_view::npos) return std::nullopt;
    const auto vault = rest.substr(0, slash);
    const auto machine = rest.substr(slash + 1);
    if (vault.empty() || machine.empty()) return std::nullopt;
    if (vault.find('/') != std::string_view::npos
        || machine.find('/') != std::string_view::npos) return std::nullopt;
    return ParsedQueueTable{std::string(vault), std::string(machine)};
}

bool sync_same_vault(std::string_view left, std::string_view right) {
    return fold_ascii(left) == fold_ascii(right);
}

bool sync_same_machine(std::string_view left, std::string_view right) {
    return same_machine_name(left, right);
}

ConfigDelta sync_config_delta(
    const std::vector<ConfigFile>& old_rows,
    const std::vector<ConfigFile>& new_rows) {
    std::map<std::string_view, std::string_view> old_ordinary;
    for (const auto& row : old_rows) {
        if (sync_is_d1_row(row.name, row.content)) continue;
        old_ordinary.emplace(row.name, row.content);
    }
    std::map<std::string_view, std::string_view> new_ordinary;
    for (const auto& row : new_rows) {
        if (sync_is_d1_row(row.name, row.content)) continue;
        new_ordinary.emplace(row.name, row.content);
    }
    ConfigDelta delta;
    for (const auto& [name, content] : new_ordinary) {
        const auto found = old_ordinary.find(name);
        if (found == old_ordinary.end() || found->second != content)
            delta.set.push_back({std::string(name), std::string(content)});
    }
    for (const auto& [name, content] : old_ordinary) {
        if (!new_ordinary.contains(name)) delta.names.emplace_back(name);
    }
    return delta;
}

std::vector<std::string> sync_note_path(
    storage::SqliteDatabase& database, std::int64_t id) {
    std::vector<std::string> reversed;
    std::optional<std::int64_t> current = id;
    std::set<std::int64_t> visited;
    while (current) {
        if (!visited.insert(*current).second)
            throw std::runtime_error("Sync note path contains a cycle.");
        auto query = database.prepare(
            "SELECT parent_id, name FROM note_items WHERE id=?1 AND compressed=0", *current);
        if (!query.step()) throw std::runtime_error("Sync note path is missing.");
        reversed.push_back(query.text(1));
        current = query.is_null(0) ? std::optional<std::int64_t>{}
                                   : query.integer(0);
    }
    return {reversed.rbegin(), reversed.rend()};
}

std::optional<std::int64_t> sync_note_find(
    storage::SqliteDatabase& database,
    const std::vector<std::string>& path) {
    std::optional<std::int64_t> parent;
    std::optional<std::int64_t> found;
    for (const std::string& name : path) {
        auto query = parent
            ? database.prepare(
                "SELECT id FROM note_items WHERE parent_id=?1 AND name=?2 AND compressed=0", *parent, name)
            : database.prepare(
                "SELECT id FROM note_items WHERE parent_id IS NULL AND name=?1 AND compressed=0", name);
        if (!query.step()) return std::nullopt;
        found = query.integer(0);
        parent = found;
    }
    return found;
}

std::size_t sync_collect_flagged_notes(
    storage::SqliteDatabase& database, std::string_view machine) {
    if (!valid_machine_name(machine)) return 0;
    if (!has_table(database, "note_items") || !has_note_changed_column(database)) return 0;
    std::vector<std::int64_t> flagged;
    {
        auto query = database.prepare(
            "SELECT id FROM note_items WHERE changed<>0 AND kind='note' AND compressed=0 ORDER BY id");
        while (query.step()) flagged.push_back(query.integer(0));
    }
    std::size_t collected = 0;
    for (const std::int64_t id : flagged) {
        storage::SqliteTransaction transaction(database);
        auto check = database.prepare(
            "SELECT changed FROM note_items WHERE id=?1 AND kind='note' AND compressed=0", id);
        if (!check.step() || check.integer(0) == 0) continue;
        if (!sync_connected(database, machine)) continue;
        auto fields = note_fields(database, id);
        fields["path"] = sync_note_path(database, id);
        sync_queue_append(database, sync_make_record("note.update", fields));
        database.prepare("UPDATE note_items SET changed=0 WHERE id=?1", id).run();
        transaction.commit();
        ++collected;
    }
    return collected;
}

} // namespace cha
