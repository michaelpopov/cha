#include "storage/note_storage.h"

#include "storage/session_label.h"
#include "util/logging.h"

#include <stdexcept>
#include <utility>

namespace cha {
namespace {

constexpr std::string_view duplicate_name_message =
    "An item with this name already exists here.";
constexpr std::string_view missing_parent_message =
    "The parent folder was not found.";
constexpr std::string_view parent_kind_message =
    "The parent must be a folder.";
constexpr std::string_view kind_message =
    "Kind must be folder or note.";
constexpr std::string_view object_message =
    "Note content must be a JSON object.";
constexpr std::string_view missing_note_message =
    "That note was not found.";
constexpr std::string_view missing_item_message =
    "That note or folder was not found.";
constexpr std::string_view not_note_message =
    "That item is not a note.";
constexpr std::string_view too_large_message =
    "Note exceeds 12 MiB. Remove content or use a smaller picture.";
constexpr std::string_view opening_message =
    "The note could not be opened.";
constexpr std::string_view saving_message =
    "The note could not be saved.";
constexpr std::string_view loading_message =
    "The notes could not be loaded.";
constexpr std::string_view change_message =
    "The change could not be saved.";

[[noreturn]] void fail(NoteFailure failure, std::string_view message) {
    throw NoteError(failure, std::string(message));
}

std::string_view kind_name(NoteItemKind kind) {
    switch (kind) {
    case NoteItemKind::folder: return "folder";
    case NoteItemKind::note: return "note";
    }
    fail(NoteFailure::invalid, kind_message);
}

void require_name(std::string_view name) {
    try {
        validate_display_label(name, "Name");
    } catch (const std::invalid_argument& error) {
        fail(NoteFailure::invalid, error.what());
    }
}

std::int64_t parent_key(const std::optional<std::int64_t>& parent_id) {
    return parent_id.value_or(0);
}

NoteError sql_failure(
    const std::runtime_error& error,
    NoteFailure failure,
    std::string_view public_message) {
    const std::string message = error.what();
    if (message.find("UNIQUE constraint failed") != std::string::npos) {
        return NoteError(NoteFailure::invalid, std::string(duplicate_name_message));
    }
    if (message.find("FOREIGN KEY constraint failed") != std::string::npos) {
        return NoteError(NoteFailure::invalid, std::string(missing_parent_message));
    }
    log_error(std::string("Note storage failed: ") + message);
    return NoteError(failure, std::string(public_message));
}

// A damaged row fails as a storage error, so the caller logs it and uses
// the public message of its operation.
NoteItem read_item(storage::SqliteStatement& statement) {
    NoteItem item;
    item.id = statement.integer(0);
    if (!statement.is_null(1)) item.parent_id = statement.integer(1);
    const std::string kind = statement.text(2);
    if (kind == "folder") item.kind = NoteItemKind::folder;
    else if (kind == "note") item.kind = NoteItemKind::note;
    else throw std::runtime_error(
        "Note item " + std::to_string(item.id) + " has an invalid kind");
    item.name = statement.text(3);
    if (item.id <= 0 || item.id > note_id_limit) {
        throw std::runtime_error(
            "Note item " + std::to_string(item.id) + " has an invalid id");
    }
    return item;
}

bool name_taken(
    storage::SqliteDatabase& database,
    std::optional<std::int64_t> parent_id,
    std::string_view name,
    std::int64_t except_id) {
    storage::SqliteStatement statement = database.prepare(
        "SELECT 1 FROM note_items "
        "WHERE IFNULL(parent_id, 0) = ?1 AND name = ?2 AND id <> ?3 LIMIT 1");
    statement.bind(1, parent_key(parent_id));
    statement.bind(2, name);
    statement.bind(3, except_id);
    return statement.step();
}

void require_folder(
    storage::SqliteDatabase& database,
    std::int64_t parent_id) {
    storage::SqliteStatement statement = database.prepare(
        "SELECT kind FROM note_items WHERE id = ?1");
    statement.bind(1, parent_id);
    if (!statement.step()) fail(NoteFailure::invalid, missing_parent_message);
    if (statement.text(0) != "folder") fail(NoteFailure::invalid, parent_kind_message);
}

std::optional<NoteItem> find_item(
    storage::SqliteDatabase& database,
    std::int64_t id) {
    storage::SqliteStatement statement = database.prepare(
        "SELECT id, parent_id, kind, name FROM note_items WHERE id = ?1");
    statement.bind(1, id);
    if (!statement.step()) return std::nullopt;
    return read_item(statement);
}

std::string compact_document(
    const nlohmann::json& document,
    std::int64_t id,
    bool saving) {
    if (!document.is_object()) fail(NoteFailure::invalid, object_message);
    try {
        std::string compact = document.dump();
        if (compact.size() > note_document_byte_limit) {
            fail(NoteFailure::too_large, too_large_message);
        }
        return compact;
    } catch (const NoteError&) {
        throw;
    } catch (const std::exception&) {
        log_error(
            "Note content could not be serialized for id "
            + std::to_string(id));
        fail(saving ? NoteFailure::saving : NoteFailure::opening,
            saving ? saving_message : opening_message);
    }
}

nlohmann::json parse_document(std::string_view text, std::int64_t id) {
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(std::string(text));
    } catch (const std::exception&) {
        log_error(
            "Note content is not valid JSON for id " + std::to_string(id));
        fail(NoteFailure::opening, opening_message);
    }
    if (!parsed.is_object()) {
        log_error(
            "Note content is not a JSON object for id " + std::to_string(id));
        fail(NoteFailure::opening, opening_message);
    }
    (void)compact_document(parsed, id, false);
    return parsed;
}

void append_document_text(const nlohmann::json& node, std::string& text) {
    if (!node.is_object()) return;
    const auto type = node.value("type", std::string{});
    if (type == "text") {
        text += node.value("text", std::string{});
        if (const auto marks = node.find("marks"); marks != node.end() && marks->is_array()) {
            for (const auto& mark : *marks) {
                if (mark.value("type", "") == "link" && mark.contains("attrs")) {
                    const auto href = mark["attrs"].value("href", std::string{});
                    if (!href.empty() && href != node.value("text", std::string{}))
                        text += " (" + href + ")";
                }
            }
        }
        return;
    }
    if (type == "hardBreak" || type == "horizontalRule") text += '\n';
    if (type == "image" && node.contains("attrs")) {
        const auto& attrs = node["attrs"];
        if (attrs.contains("alt") && attrs["alt"].is_string()) text += attrs["alt"].get<std::string>();
        text += '\n';
    }
    if (const auto content = node.find("content"); content != node.end() && content->is_array()) {
        for (const auto& child : *content) append_document_text(child, text);
    }
    if (type == "paragraph" || type == "heading" || type == "codeBlock"
        || type == "listItem" || type == "blockquote") {
        text += '\n';
    }
}

} // namespace

NoteError::NoteError(NoteFailure failure, std::string message)
    : failure(failure), message_(std::move(message)) {}

const char* NoteError::what() const noexcept {
    return message_.c_str();
}

void to_json(nlohmann::json& json, const NoteItem& item) {
    json = nlohmann::json{
        {"id", item.id},
        {"parent_id", item.parent_id
            ? nlohmann::json(*item.parent_id)
            : nlohmann::json(nullptr)},
        {"kind", kind_name(item.kind)},
        {"name", item.name},
    };
}

void to_json(nlohmann::json& json, const NoteContent& content) {
    json = nlohmann::json{
        {"id", content.id},
        {"document", content.document},
    };
}

std::string note_document_text(const nlohmann::json& document) {
    std::string text;
    append_document_text(document, text);
    while (!text.empty() && text.back() == '\n') text.pop_back();
    return text;
}

void create_note_items_schema(storage::SqliteDatabase& database) {
    database.execute(R"sql(
CREATE TABLE IF NOT EXISTS note_items (
    id INTEGER PRIMARY KEY AUTOINCREMENT
        CHECK (id > 0 AND id <= 9007199254740991),
    parent_id INTEGER REFERENCES note_items(id) ON DELETE CASCADE,
    kind TEXT NOT NULL CHECK (kind IN ('folder', 'note')),
    name TEXT NOT NULL COLLATE BINARY,
    content_json TEXT,
    CHECK (
        (kind = 'folder' AND content_json IS NULL)
        OR (kind = 'note' AND content_json IS NOT NULL)
    )
) STRICT;
CREATE UNIQUE INDEX IF NOT EXISTS note_items_sibling_name
    ON note_items(IFNULL(parent_id, 0), name);
CREATE INDEX IF NOT EXISTS note_items_parent
    ON note_items(parent_id);

-- Copies belong to the chat, so deleting the source note keeps them intact.
CREATE TABLE IF NOT EXISTS session_notes (
    session_key INTEGER NOT NULL REFERENCES sessions(session_key) ON DELETE CASCADE,
    note_id INTEGER NOT NULL CHECK (note_id > 0),
    name TEXT NOT NULL,
    text TEXT NOT NULL,
    PRIMARY KEY (session_key, note_id)
) STRICT;
)sql");
}

std::vector<NoteItem> note_list(storage::SqliteDatabase& database) {
    try {
        storage::SqliteTransaction transaction(database);
        storage::SqliteStatement statement = database.prepare(
            "SELECT id, parent_id, kind, name FROM note_items ORDER BY id");
        std::vector<NoteItem> items;
        while (statement.step()) items.push_back(read_item(statement));
        transaction.commit();
        return items;
    } catch (const std::runtime_error& error) {
        throw sql_failure(error, NoteFailure::opening, loading_message);
    }
}

NoteItem note_create(
    storage::SqliteDatabase& database,
    std::optional<std::int64_t> parent_id,
    NoteItemKind kind,
    std::string_view name) {
    require_name(name);
    const std::string kind_text(kind_name(kind));
    try {
        storage::SqliteTransaction transaction(database);
        if (parent_id) require_folder(database, *parent_id);
        if (name_taken(database, parent_id, name, 0)) {
            fail(NoteFailure::invalid, duplicate_name_message);
        }
        storage::SqliteStatement statement = database.prepare(
            "INSERT INTO note_items (parent_id, kind, name, content_json) "
            "VALUES (?1, ?2, ?3, ?4)");
        if (parent_id) statement.bind(1, *parent_id);
        else statement.bind_null(1);
        statement.bind(2, kind_text);
        statement.bind(3, name);
        if (kind == NoteItemKind::folder) statement.bind_null(4);
        else statement.bind(4, empty_note_document);
        statement.run();
        const std::int64_t id = database.last_insert_rowid();
        transaction.commit();
        return NoteItem{id, parent_id, kind, std::string(name)};
    } catch (const std::runtime_error& error) {
        throw sql_failure(error, NoteFailure::saving, change_message);
    }
}

NoteContent note_get(storage::SqliteDatabase& database, std::int64_t id) {
    try {
        storage::SqliteTransaction transaction(database);
        storage::SqliteStatement statement = database.prepare(
            "SELECT kind, content_json FROM note_items WHERE id = ?1");
        statement.bind(1, id);
        if (!statement.step()) fail(NoteFailure::not_found, missing_note_message);
        if (statement.text(0) != "note") {
            fail(NoteFailure::invalid, not_note_message);
        }
        if (statement.is_null(1)) fail(NoteFailure::opening, opening_message);
        nlohmann::json document = parse_document(statement.text(1), id);
        NoteContent content{id, std::move(document)};
        transaction.commit();
        return content;
    } catch (const std::runtime_error& error) {
        throw sql_failure(error, NoteFailure::opening, opening_message);
    }
}

NoteText note_get_text(storage::SqliteDatabase& database, std::int64_t id) {
    try {
        auto statement = database.prepare(
            "SELECT name, content_json FROM note_items WHERE id = ?1 AND kind = 'note'", id);
        if (!statement.step()) fail(NoteFailure::not_found, missing_note_message);
        return {id, statement.text(0),
            note_document_text(parse_document(statement.text(1), id))};
    } catch (const nlohmann::json::exception&) {
        fail(NoteFailure::opening, opening_message);
    } catch (const std::runtime_error& error) {
        throw sql_failure(error, NoteFailure::opening, opening_message);
    }
}

void note_save(
    storage::SqliteDatabase& database,
    std::int64_t id,
    const nlohmann::json& document) {
    const std::string compact = compact_document(document, id, true);
    try {
        storage::SqliteTransaction transaction(database);
        storage::SqliteStatement update = database.prepare(
            "UPDATE note_items SET content_json = ?1 "
            "WHERE id = ?2 AND kind = 'note'");
        update.bind(1, compact);
        update.bind(2, id);
        update.run();
        if (database.changes() != 1) {
            storage::SqliteStatement existing = database.prepare(
                "SELECT 1 FROM note_items WHERE id = ?1");
            existing.bind(1, id);
            if (!existing.step()) fail(NoteFailure::not_found, missing_note_message);
            fail(NoteFailure::invalid, not_note_message);
        }
        transaction.commit();
    } catch (const std::runtime_error& error) {
        throw sql_failure(error, NoteFailure::saving, saving_message);
    }
}

NoteItem note_rename(
    storage::SqliteDatabase& database,
    std::int64_t id,
    std::string_view name) {
    require_name(name);
    try {
        storage::SqliteTransaction transaction(database);
        const std::optional<NoteItem> current = find_item(database, id);
        if (!current) fail(NoteFailure::not_found, missing_item_message);
        NoteItem item = *current;
        if (item.name != name) {
            if (name_taken(database, item.parent_id, name, id)) {
                fail(NoteFailure::invalid, duplicate_name_message);
            }
            storage::SqliteStatement update = database.prepare(
                "UPDATE note_items SET name = ?1 WHERE id = ?2");
            update.bind(1, name);
            update.bind(2, id);
            update.run();
            item.name = std::string(name);
        }
        transaction.commit();
        return item;
    } catch (const std::runtime_error& error) {
        throw sql_failure(error, NoteFailure::saving, change_message);
    }
}

void note_delete(storage::SqliteDatabase& database, std::int64_t id) {
    try {
        storage::SqliteTransaction transaction(database);
        storage::SqliteStatement statement = database.prepare(
            "DELETE FROM note_items WHERE id = ?1");
        statement.bind(1, id);
        statement.run();
        if (database.changes() != 1) {
            fail(NoteFailure::not_found, missing_item_message);
        }
        transaction.commit();
    } catch (const std::runtime_error& error) {
        throw sql_failure(error, NoteFailure::saving, change_message);
    }
}

} // namespace cha
