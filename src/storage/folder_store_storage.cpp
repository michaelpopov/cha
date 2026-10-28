#include "storage/folder_store_storage.h"

#include "storage/session_timestamp.h"
#include "util/text.h"

#include <algorithm>
#include <map>
#include <new>
#include <stdexcept>
#include <utility>

namespace cha {
namespace {

constexpr std::string_view store_missing = "That folder store was not found.";
constexpr std::string_view not_folder = "A store can only belong to a real folder.";
constexpr std::string_view empty_upload = "This folder has no notes to upload.";

[[noreturn]] void fail_store(std::string_view message) {
    throw FolderStoreError(std::string(message));
}

void require_positive_id(std::int64_t id, std::string_view name) {
    if (id <= 0 || id > note_id_limit) {
        throw FolderStoreError(std::string(name) + " is not valid.");
    }
}

bool table_exists(storage::SqliteDatabase& database, std::string_view name) {
    auto statement = database.prepare(
        "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ?1 LIMIT 1");
    statement.bind(1, name);
    return statement.step();
}

FolderStoreRecord read_record(storage::SqliteStatement& row) {
    FolderStoreRecord record;
    record.id = row.integer(0);
    record.folder_id = row.integer(1);
    record.service = parse_folder_store_service(row.text(2));
    record.api_key_id = row.text(3);
    if (!row.is_null(4)) record.remote_store_id = row.text(4);
    record.state = row.text(5) == "ready" ? FolderStoreState::ready
                                          : FolderStoreState::incomplete;
    record.note_count = row.integer(6);
    if (!row.is_null(7)) record.uploaded_at = row.integer(7);
    record.remote_outcome_unknown = row.integer(8) != 0;
    record.folder_name = row.is_null(9) ? std::string{} : row.text(9);
    return record;
}

void load_file_ids(storage::SqliteDatabase& database, FolderStoreRecord& record) {
    auto files = database.prepare(
        "SELECT file_id FROM folder_store_files WHERE store_id = ?1 ORDER BY file_id",
        record.id);
    while (files.step()) record.file_ids.push_back(files.text(0));
}

std::optional<FolderStoreRecord> read_optional(
    storage::SqliteDatabase& database, storage::SqliteStatement& row) {
    if (!row.step()) return std::nullopt;
    auto record = read_record(row);
    load_file_ids(database, record);
    return record;
}

constexpr std::string_view record_select =
    "SELECT fs.id, fs.folder_id, fs.service, fs.api_key_id, fs.remote_store_id, "
    "fs.state, fs.note_count, fs.uploaded_at, fs.remote_outcome_unknown, "
    "IFNULL(n.name, '') "
    "FROM folder_stores fs LEFT JOIN note_items n ON n.id = fs.folder_id ";

void require_current_store(storage::SqliteDatabase& database, std::int64_t store_id) {
    auto row = database.prepare(
        "SELECT 1 FROM folder_stores WHERE id = ?1", store_id);
    if (!row.step()) fail_store(store_missing);
}

std::string escape_metadata(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const unsigned char c : text) {
        if (c == '\n' || c == '\r' || c == '\t') out.push_back(' ');
        else if (c >= 32 && c != 127) {
            if (std::string_view("\\`*_{}[]<>()#!|~").find(static_cast<char>(c)) != std::string_view::npos) out.push_back('\\');
            out.push_back(static_cast<char>(c));
        }
    }
    return out;
}

bool node_has_exportable_text(const nlohmann::json& node) {
    if (!node.is_object()) return false;
    const auto type = node.value("type", std::string{});
    if (type == "text") {
        const auto text = node.value("text", std::string{});
        return !trim_view(text).empty();
    }
    if (type == "image") {
        const auto attrs = node.find("attrs");
        if (attrs == node.end() || !attrs->is_object()) return false;
        const auto alt = attrs->find("alt");
        if (alt == attrs->end() || !alt->is_string()) return false;
        const auto& text = alt->get_ref<const std::string&>();
        return !trim_view(text).empty();
    }
    const auto content = node.find("content");
    if (content == node.end() || !content->is_array()) return false;
    for (const auto& child : *content) {
        if (node_has_exportable_text(child)) return true;
    }
    return false;
}

std::string ancestor_path(
    const std::map<std::int64_t, std::optional<std::int64_t>>& parents,
    const std::map<std::int64_t, std::string>& names,
    std::int64_t folder_id) {
    std::vector<std::string> parts;
    std::optional<std::int64_t> current = folder_id;
    while (current) {
        const auto name = names.find(*current);
        if (name == names.end()) break;
        parts.push_back(name->second);
        const auto parent = parents.find(*current);
        if (parent == parents.end()) break;
        current = parent->second;
    }
    std::reverse(parts.begin(), parts.end());
    std::string path;
    for (const auto& part : parts) {
        if (!path.empty()) path += '/';
        path += part;
    }
    return path;
}

} // namespace

void create_folder_store_schema(storage::SqliteDatabase& database) {
    database.execute(R"sql(
CREATE TABLE IF NOT EXISTS folder_stores (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    folder_id INTEGER NOT NULL UNIQUE
        REFERENCES note_items(id) ON DELETE RESTRICT,
    service TEXT NOT NULL CHECK (service IN ('openai', 'google')),
    api_key_id TEXT NOT NULL,
    remote_store_id TEXT,
    state TEXT NOT NULL CHECK (state IN ('incomplete', 'ready')),
    note_count INTEGER NOT NULL CHECK (note_count > 0),
    uploaded_at INTEGER,
    remote_outcome_unknown INTEGER NOT NULL DEFAULT 0
        CHECK (remote_outcome_unknown IN (0, 1)),
    CHECK (state <> 'ready' OR
        (remote_store_id IS NOT NULL AND uploaded_at IS NOT NULL
         AND remote_outcome_unknown = 0))
) STRICT;

CREATE TABLE IF NOT EXISTS folder_store_files (
    store_id INTEGER NOT NULL REFERENCES folder_stores(id) ON DELETE CASCADE,
    file_id TEXT NOT NULL,
    PRIMARY KEY (store_id, file_id)
) STRICT;

CREATE TABLE IF NOT EXISTS session_folders (
    session_key INTEGER NOT NULL REFERENCES sessions(session_key) ON DELETE CASCADE,
    folder_id INTEGER NOT NULL CHECK (folder_id > 0),
    PRIMARY KEY (session_key, folder_id)
) STRICT;
)sql");
}

std::string_view folder_store_service_name(FolderStoreService service) {
    switch (service) {
    case FolderStoreService::openai: return "openai";
    case FolderStoreService::google: return "google";
    }
    fail_store("Unknown folder store service.");
}

FolderStoreService parse_folder_store_service(std::string_view value) {
    if (value == "openai") return FolderStoreService::openai;
    if (value == "google") return FolderStoreService::google;
    fail_store("Unknown folder store service.");
}

std::string note_item_ref_id(std::int64_t id) {
    require_positive_id(id, "Item ID");
    constexpr std::string_view alphabet =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::string encoded;
    auto value = id;
    while (value > 0) {
        encoded.insert(encoded.begin(), alphabet[static_cast<std::size_t>(value % 62)]);
        value /= 62;
    }
    if (encoded.size() < 6) encoded.insert(0, 6 - encoded.size(), '0');
    return "##" + encoded;
}

std::string folder_store_filename(std::string_view name, std::int64_t note_id) {
    std::string title;
    title.reserve(std::min<std::size_t>(name.size(), 84));
    bool dash = false;
    for (const unsigned char c : name) {
        const bool separator = c < 32 || c == 127 || c == '/' || c == '\\'
            || c == ':' || c == '*' || c == '?' || c == '"' || c == '<'
            || c == '>' || c == '|' || is_space(static_cast<char>(c));
        if (separator) {
            if (!title.empty()) dash = true;
            continue;
        }
        if (dash) {
            title.push_back('-');
            dash = false;
        }
        title.push_back(static_cast<char>(c));
        if (title.size() >= 84) break;
    }
    title = utf8_prefix(title, 80);
    while (!title.empty() && title.back() == '-') title.pop_back();
    if (title.empty()) title = "note";
    return title + "-" + std::to_string(note_id) + ".md";
}

bool folder_store_document_nonempty(const nlohmann::json& document) {
    return node_has_exportable_text(document);
}

std::size_t folder_store_document_byte_limit(FolderStoreService service) {
    return service == FolderStoreService::openai
        ? folder_store_openai_file_byte_limit
        : folder_store_google_document_byte_limit;
}

void folder_store_validate_document_size(FolderStoreService service, std::size_t bytes,
    std::string_view note_name, std::int64_t note_id) {
    if (bytes > folder_store_document_byte_limit(service)) {
        throw FolderStoreError("Note “" + std::string(note_name) + "” (" + std::to_string(note_id)
            + ") is larger than the " + std::string(folder_store_service_name(service)) + " document limit.");
    }
}

bool folder_store_tables_exist(storage::SqliteDatabase& database) {
    return table_exists(database, "folder_stores")
        && table_exists(database, "folder_store_files")
        && table_exists(database, "session_folders");
}

std::optional<NoteItem> folder_store_require_folder(
    storage::SqliteDatabase& database, std::int64_t folder_id) {
    require_positive_id(folder_id, "Folder ID");
    auto row = database.prepare(
        "SELECT id, parent_id, kind, name, compressed_text IS NOT NULL, read_only "
        "FROM note_items WHERE id = ?1",
        folder_id);
    if (!row.step()) return std::nullopt;
    NoteItem item;
    item.id = row.integer(0);
    if (!row.is_null(1)) item.parent_id = row.integer(1);
    const auto kind = row.text(2);
    if (kind == "folder") item.kind = NoteItemKind::folder;
    else if (kind == "note") item.kind = NoteItemKind::note;
    else item.kind = NoteItemKind::annotation;
    item.name = row.text(3);
    item.has_compressed = row.integer(4) != 0;
    item.read_only = row.integer(5) != 0;
    if (item.kind != NoteItemKind::folder) return std::nullopt;
    return item;
}

bool folder_store_folder_is_ready(
    storage::SqliteDatabase& database, std::int64_t folder_id) {
    if (!folder_store_tables_exist(database)) return false;
    auto row = database.prepare(
        "SELECT 1 FROM folder_stores fs "
        "JOIN note_items n ON n.id = fs.folder_id AND n.kind = 'folder' "
        "WHERE fs.folder_id = ?1 AND fs.state = 'ready'",
        folder_id);
    return row.step();
}

std::optional<FolderStoreRecord> folder_store_get(
    storage::SqliteDatabase& database, std::int64_t store_id) {
    if (!folder_store_tables_exist(database)) return std::nullopt;
    auto row = database.prepare(std::string(record_select) + "WHERE fs.id = ?1", store_id);
    return read_optional(database, row);
}

std::optional<FolderStoreRecord> folder_store_get_by_folder(
    storage::SqliteDatabase& database, std::int64_t folder_id) {
    if (!folder_store_tables_exist(database)) return std::nullopt;
    auto row = database.prepare(
        std::string(record_select) + "WHERE fs.folder_id = ?1", folder_id);
    return read_optional(database, row);
}

std::vector<FolderStoreRecord> folder_store_list(storage::SqliteDatabase& database) {
    std::vector<FolderStoreRecord> records;
    if (!folder_store_tables_exist(database)) return records;
    auto row = database.prepare(std::string(record_select) + "ORDER BY fs.id");
    while (row.step()) {
        auto record = read_record(row);
        load_file_ids(database, record);
        records.push_back(std::move(record));
    }
    return records;
}

std::vector<std::int64_t> folder_store_folder_ids_in_subtree(
    storage::SqliteDatabase& database, std::int64_t folder_id) {
    std::vector<std::int64_t> ids;
    auto row = database.prepare(
        "WITH RECURSIVE tree(id) AS ("
        "SELECT id FROM note_items WHERE id = ?1 AND kind = 'folder' "
        "UNION ALL SELECT n.id FROM note_items n JOIN tree t ON n.parent_id = t.id "
        "WHERE n.kind = 'folder') "
        "SELECT id FROM tree ORDER BY id",
        folder_id);
    while (row.step()) ids.push_back(row.integer(0));
    return ids;
}

std::vector<FolderStoreRecord> folder_store_records_in_subtree(
    storage::SqliteDatabase& database, std::int64_t folder_id) {
    std::vector<FolderStoreRecord> records;
    if (!folder_store_tables_exist(database)) return records;
    auto row = database.prepare(
        std::string(record_select)
            + "WHERE fs.folder_id IN ("
              "WITH RECURSIVE tree(id) AS ("
              "SELECT id FROM note_items WHERE id = ?1 AND kind = 'folder' "
              "UNION ALL SELECT n.id FROM note_items n JOIN tree t ON n.parent_id = t.id "
              "WHERE n.kind = 'folder') SELECT id FROM tree) "
              "ORDER BY fs.id",
        folder_id);
    while (row.step()) {
        auto record = read_record(row);
        load_file_ids(database, record);
        records.push_back(std::move(record));
    }
    return records;
}

std::int64_t folder_store_count_in_subtree(
    storage::SqliteDatabase& database, std::int64_t folder_id) {
    if (!folder_store_tables_exist(database)) return 0;
    auto row = database.prepare(
        "SELECT COUNT(*) FROM folder_stores WHERE folder_id IN ("
        "WITH RECURSIVE tree(id) AS ("
        "SELECT id FROM note_items WHERE id = ?1 AND kind = 'folder' "
        "UNION ALL SELECT n.id FROM note_items n JOIN tree t ON n.parent_id = t.id "
        "WHERE n.kind = 'folder') SELECT id FROM tree)",
        folder_id);
    (void)row.step();
    return row.integer(0);
}

bool folder_store_owns_subtree(
    storage::SqliteDatabase& database, std::int64_t folder_id) {
    return folder_store_count_in_subtree(database, folder_id) > 0;
}

std::vector<FolderStoreKeyUsage> folder_store_usage(storage::SqliteDatabase& database) {
    std::vector<FolderStoreKeyUsage> usage;
    if (!folder_store_tables_exist(database)) return usage;
    auto row = database.prepare(
        "SELECT fs.api_key_id, fs.folder_id, IFNULL(n.name, ''), fs.service "
        "FROM folder_stores fs LEFT JOIN note_items n ON n.id = fs.folder_id "
        "ORDER BY fs.id");
    while (row.step()) {
        usage.push_back({
            .api_key_id = row.text(0),
            .folder_id = row.integer(1),
            .folder_name = row.text(2),
            .service = parse_folder_store_service(row.text(3)),
        });
    }
    return usage;
}

std::vector<std::int64_t> session_folder_ids(
    storage::SqliteDatabase& database, SessionKey session_key) {
    std::vector<std::int64_t> ids;
    if (!table_exists(database, "session_folders")) return ids;
    auto row = database.prepare(
        "SELECT folder_id FROM session_folders WHERE session_key = ?1 ORDER BY folder_id",
        session_key);
    while (row.step()) ids.push_back(row.integer(0));
    return ids;
}

std::int64_t folder_store_register(
    storage::SqliteDatabase& database,
    std::int64_t folder_id,
    FolderStoreService service,
    std::string_view api_key_id,
    std::int64_t note_count) {
    const auto folder = folder_store_require_folder(database, folder_id);
    if (!folder) fail_store(not_folder);
    if (api_key_id.empty()) fail_store("A saved API key is required.");
    if (note_count <= 0) fail_store("A store needs at least one note.");
    auto insert = database.prepare(
        "INSERT INTO folder_stores (folder_id, service, api_key_id, state, note_count) "
        "VALUES (?1, ?2, ?3, 'incomplete', ?4)",
        folder_id,
        folder_store_service_name(service),
        api_key_id,
        note_count);
    insert.run();
    return database.last_insert_rowid();
}

void folder_store_set_uncertainty(
    storage::SqliteDatabase& database, std::int64_t store_id, bool unknown) {
    require_current_store(database, store_id);
    database.prepare(
        "UPDATE folder_stores SET remote_outcome_unknown = ?1, state = 'incomplete' "
        "WHERE id = ?2",
        unknown ? std::int64_t{1} : std::int64_t{0},
        store_id)
        .run();
    if (database.changes() != 1) fail_store(store_missing);
}

void folder_store_save_remote_id(
    storage::SqliteDatabase& database,
    std::int64_t store_id,
    std::string_view remote_store_id) {
    require_current_store(database, store_id);
    if (remote_store_id.empty()) fail_store("The remote store ID is empty.");
    database.prepare(
        "UPDATE folder_stores SET remote_store_id = ?1, remote_outcome_unknown = 0, "
        "state = 'incomplete' WHERE id = ?2",
        remote_store_id,
        store_id)
        .run();
    if (database.changes() != 1) fail_store(store_missing);
}

void folder_store_add_file_id(
    storage::SqliteDatabase& database,
    std::int64_t store_id,
    std::string_view file_id) {
    require_current_store(database, store_id);
    if (file_id.empty()) fail_store("The file ID is empty.");
    database.prepare(
        "INSERT OR IGNORE INTO folder_store_files (store_id, file_id) VALUES (?1, ?2)",
        store_id,
        file_id)
        .run();
    database.prepare(
        "UPDATE folder_stores SET remote_outcome_unknown = 0 WHERE id = ?1",
        store_id)
        .run();
}

void folder_store_remove_file_id(
    storage::SqliteDatabase& database,
    std::int64_t store_id,
    std::string_view file_id) {
    require_current_store(database, store_id);
    database.prepare(
        "DELETE FROM folder_store_files WHERE store_id = ?1 AND file_id = ?2",
        store_id,
        file_id)
        .run();
}

void folder_store_clear_remote_store_id(
    storage::SqliteDatabase& database, std::int64_t store_id) {
    require_current_store(database, store_id);
    database.prepare(
        "UPDATE folder_stores SET remote_store_id = NULL WHERE id = ?1",
        store_id)
        .run();
}

void folder_store_mark_ready(
    storage::SqliteDatabase& database, std::int64_t store_id) {
    const auto record = folder_store_get(database, store_id);
    if (!record || !record->remote_store_id || record->remote_outcome_unknown) fail_store("The folder store is not ready.");
    database.prepare(
        "UPDATE folder_stores SET state = 'ready', uploaded_at = ?1, "
        "remote_outcome_unknown = 0 WHERE id = ?2 AND remote_store_id IS NOT NULL",
        session_timestamp(),
        store_id)
        .run();
    if (database.changes() != 1) fail_store(store_missing);
}

void folder_store_mark_incomplete(
    storage::SqliteDatabase& database, std::int64_t store_id) {
    require_current_store(database, store_id);
    database.prepare(
        "UPDATE folder_stores SET state = 'incomplete' WHERE id = ?1",
        store_id)
        .run();
}

void folder_store_mark_subtree_incomplete(
    storage::SqliteDatabase& database, std::int64_t folder_id) {
    database.prepare(
        "UPDATE folder_stores SET state = 'incomplete' WHERE folder_id IN ("
        "WITH RECURSIVE tree(id) AS ("
        "SELECT id FROM note_items WHERE id = ?1 AND kind = 'folder' "
        "UNION ALL SELECT n.id FROM note_items n JOIN tree t ON n.parent_id = t.id "
        "WHERE n.kind = 'folder') SELECT id FROM tree)",
        folder_id)
        .run();
}

void folder_store_delete_record(
    storage::SqliteDatabase& database, std::int64_t store_id) {
    database.prepare("DELETE FROM folder_stores WHERE id = ?1", store_id).run();
    if (database.changes() != 1) fail_store(store_missing);
}

void folder_store_delete_session_folders_in_subtree(
    storage::SqliteDatabase& database, std::int64_t folder_id) {
    if (!table_exists(database, "session_folders")) return;
    database.prepare(
        "DELETE FROM session_folders WHERE folder_id IN ("
        "WITH RECURSIVE tree(id) AS ("
        "SELECT id FROM note_items WHERE id = ?1 AND kind = 'folder' "
        "UNION ALL SELECT n.id FROM note_items n JOIN tree t ON n.parent_id = t.id "
        "WHERE n.kind = 'folder') SELECT id FROM tree)",
        folder_id)
        .run();
}

void folder_store_clear_ownership(storage::SqliteDatabase& database) {
    if (table_exists(database, "folder_store_files")) database.execute("DELETE FROM folder_store_files");
    if (table_exists(database, "folder_stores")) database.execute("DELETE FROM folder_stores");
}

namespace {
FolderStoreCapture capture_impl(
    storage::SqliteDatabase& database,
    std::int64_t folder_id,
    FolderStoreService service) {
    const auto folder = folder_store_require_folder(database, folder_id);
    if (!folder) fail_store(not_folder);

    std::map<std::int64_t, std::optional<std::int64_t>> parents;
    std::map<std::int64_t, std::string> names;
    struct Row {
        std::int64_t id{};
        std::optional<std::int64_t> parent_id;
        std::string kind;
        std::string name;
        std::optional<std::string> content;
    };
    std::vector<Row> rows;
    auto tree = database.prepare(
        "WITH RECURSIVE tree(id, parent_id, kind, name) AS ("
        "SELECT id, parent_id, kind, name FROM note_items WHERE id = ?1 "
        "UNION ALL SELECT n.id, n.parent_id, n.kind, n.name "
        "FROM note_items n JOIN tree t ON n.parent_id = t.id) "
        "SELECT t.id, t.parent_id, t.kind, t.name, "
        "CASE WHEN t.kind = 'note' THEN n.content_json END "
        "FROM tree t JOIN note_items n ON n.id = t.id",
        folder_id);
    while (tree.step()) {
        Row row;
        row.id = tree.integer(0);
        if (!tree.is_null(1)) row.parent_id = tree.integer(1);
        row.kind = tree.text(2);
        row.name = tree.text(3);
        if (!tree.is_null(4)) row.content = tree.blob(4);
        parents[row.id] = row.parent_id;
        names[row.id] = row.name;
        rows.push_back(std::move(row));
    }

    auto ancestors = database.prepare(
        "WITH RECURSIVE up(id, parent_id, name) AS ("
        "SELECT id, parent_id, name FROM note_items WHERE id = ?1 "
        "UNION ALL SELECT n.id, n.parent_id, n.name FROM note_items n "
        "JOIN up u ON u.parent_id = n.id) "
        "SELECT id, parent_id, name FROM up",
        folder_id);
    while (ancestors.step()) {
        const auto id = ancestors.integer(0);
        names[id] = ancestors.text(2);
        if (ancestors.is_null(1)) parents[id] = std::nullopt;
        else parents[id] = ancestors.integer(1);
    }

    FolderStoreCapture capture;
    capture.folder = *folder;
    capture.folder_path = ancestor_path(parents, names, folder_id);
    for (const auto& row : rows) {
        if (row.kind != "note") continue;
        if (!row.content) {
            throw FolderStoreError(
                "Could not export note “" + row.name + "” ("
                + std::to_string(row.id) + ").");
        }
        nlohmann::json document;
        try {
            document = note_parse_saved_document(*row.content, row.id);
        } catch (const NoteError& error) {
            throw FolderStoreError(
                "Could not export note “" + row.name + "” ("
                + std::to_string(row.id) + "): " + error.what());
        }
        std::string body;
        try {
            if (!folder_store_document_nonempty(document)) continue;
            body = note_document_markdown(document);
        } catch (const std::exception&) {
            throw FolderStoreError(
                "Could not export note “" + row.name + "” ("
                + std::to_string(row.id) + ").");
        }
        if (body.find_first_not_of(" \t\r\n") == std::string::npos) continue;
        const auto parent = row.parent_id.value_or(folder_id);
        const auto path = ancestor_path(parents, names, parent);
        FolderStoreDocument document_out;
        document_out.note_id = row.id;
        document_out.note_name = row.name;
        document_out.folder_path = path;
        document_out.filename = folder_store_filename(row.name, row.id);
        document_out.markdown =
            "# Note: " + escape_metadata(row.name) + "\n\nFolder: "
            + escape_metadata(path) + "\nCHA Ref ID: " + note_item_ref_id(row.id)
            + "\n\n" + body;
        folder_store_validate_document_size(service, document_out.markdown.size(), row.name, row.id);
        capture.documents.push_back(std::move(document_out));
    }
    if (capture.documents.empty()) fail_store(empty_upload);
    return capture;
}

} // namespace

FolderStoreCapture folder_store_capture(storage::SqliteDatabase& database,
    std::int64_t folder_id, FolderStoreService service) {
    try {
        return capture_impl(database, folder_id, service);
    } catch (const std::bad_alloc&) {
        throw FolderStoreError("Could not allocate memory for the folder snapshot.");
    }
}

void session_folder_insert(
    storage::SqliteDatabase& database, SessionKey session_key, std::int64_t folder_id) {
    require_positive_id(folder_id, "Folder ID");
    database.prepare(
        "INSERT OR IGNORE INTO session_folders (session_key, folder_id) VALUES (?1, ?2)",
        session_key,
        folder_id)
        .run();
}

void session_folder_delete(
    storage::SqliteDatabase& database, SessionKey session_key, std::int64_t folder_id) {
    database.prepare(
        "DELETE FROM session_folders WHERE session_key = ?1 AND folder_id = ?2",
        session_key,
        folder_id)
        .run();
}

void session_folder_clear(
    storage::SqliteDatabase& database, SessionKey session_key) {
    if (!table_exists(database, "session_folders")) return;
    database.prepare(
        "DELETE FROM session_folders WHERE session_key = ?1", session_key)
        .run();
}

void session_folder_prune(
    storage::SqliteDatabase& database,
    SessionKey session_key,
    std::span<const std::int64_t> folder_ids) {
    if (!table_exists(database, "session_folders")) return;
    for (const auto folder_id : folder_ids) {
        session_folder_delete(database, session_key, folder_id);
    }
}

} // namespace cha
