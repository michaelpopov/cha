#include "storage/note_storage.h"

#include "storage/session_label.h"
#include "util/logging.h"

#include <algorithm>
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
    item.has_compressed = statement.integer(4) != 0;
    item.read_only = statement.integer(5) != 0;
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
        "SELECT id, parent_id, kind, name, compressed_text IS NOT NULL, read_only "
        "FROM note_items WHERE id = ?1");
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

// Highlighting can split a text node. Merge matching neighbours after removing
// highlights so the comparison still protects text and all other formatting.
nlohmann::json without_highlights(nlohmann::json node) {
    if (!node.is_object()) return node;
    if (auto marks = node.find("marks"); marks != node.end() && marks->is_array()) {
        std::erase_if(marks->get_ref<nlohmann::json::array_t&>(), [](const auto& mark) {
            return mark.is_object() && mark.contains("type") && mark["type"] == "highlight";
        });
        if (marks->empty()) node.erase("marks");
    }
    if (auto content = node.find("content"); content != node.end() && content->is_array()) {
        auto merged = nlohmann::json::array();
        for (const auto& child : *content) {
            auto next = without_highlights(child);
            if (!merged.empty() && next.is_object() && next.contains("type")
                && next["type"] == "text" && next.contains("text") && next["text"].is_string()) {
                auto previous = merged.back();
                auto shape = next;
                shape.erase("text");
                if (previous.is_object() && previous.contains("text") && previous["text"].is_string()) {
                    const auto text = previous["text"].get<std::string>();
                    previous.erase("text");
                    if (previous == shape) {
                        merged.back()["text"] = text + next["text"].get<std::string>();
                        continue;
                    }
                }
            }
            merged.push_back(std::move(next));
        }
        *content = std::move(merged);
    }
    return node;
}

std::string backtick_fence(std::string_view text, std::size_t minimum = 3) {
    std::size_t length = minimum;
    std::size_t run = 0;
    for (const char c : text) {
        run = c == '`' ? run + 1 : 0;
        length = std::max(length, run + 1);
    }
    return std::string(length, '`');
}

std::string wrap_mark(std::string text, std::string_view marker) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return text;
    text.insert(text.find_last_not_of(" \t\r\n") + 1, marker);
    text.insert(first, marker);
    return text;
}

std::string markdown_text(const nlohmann::json& node) {
    std::string text = node.value("text", std::string{});
    const auto marks = node.find("marks");
    if (marks == node.end()) return text;
    for (const auto& mark : *marks) {
        const auto type = mark.value("type", std::string{});
        if (type == "bold") text = wrap_mark(std::move(text), "**");
        else if (type == "italic") text = wrap_mark(std::move(text), "*");
        else if (type == "code") {
            const auto fence = backtick_fence(text, 1);
            const bool pad = text.starts_with('`') || text.ends_with('`')
                || (text.starts_with(' ') && text.ends_with(' ') && text.find_first_not_of(' ') != std::string::npos);
            text = fence + (pad ? " " : "") + text + (pad ? " " : "") + fence;
        }
        else if (type == "link" && mark.contains("attrs")) {
            const auto href = mark["attrs"].value("href", std::string{});
            if (!href.empty()) {
                std::string destination;
                for (const char c : href) {
                    if (c == '<') destination += "%3C";
                    else if (c == '>') destination += "%3E";
                    else if (c == '\\') destination += "%5C";
                    else if (c == '\n') destination += "%0A";
                    else if (c == '\r') destination += "%0D";
                    else if (c == '\t') destination += "%09";
                    else if (c == ' ') destination += "%20";
                    else destination += c;
                }
                text = "[" + text + "](<" + destination + ">)";
            }
        }
    }
    return text;
}

std::string markdown_list_item(std::string_view text, std::string_view marker) {
    std::string result(marker);
    const std::string indent(marker.size(), ' ');
    for (std::size_t i = 0; i < text.size(); ++i) {
        result += text[i];
        if (text[i] == '\n' && i + 1 < text.size() && text[i + 1] != '\n') result += indent;
    }
    return result;
}

std::string document_markdown(const nlohmann::json& node) {
    if (!node.is_object()) return {};
    const auto type = node.value("type", std::string{});
    if (type == "text") return markdown_text(node);
    if (type == "hardBreak") return "\\\n";
    const auto attrs = node.find("attrs");
    if (type == "image") {
        return attrs != node.end() && attrs->contains("alt") && (*attrs)["alt"].is_string()
            ? (*attrs)["alt"].get<std::string>() : "";
    }
    const auto content = node.find("content");
    if (type == "codeBlock") {
        std::string text;
        if (content != node.end()) {
            for (const auto& child : *content) text += child.value("text", std::string{});
        }
        const auto fence = backtick_fence(text);
        const auto language = attrs != node.end() && attrs->contains("language") && (*attrs)["language"].is_string()
            ? (*attrs)["language"].get<std::string>() : "";
        return fence + (language.find_first_of("`\r\n") == std::string::npos ? language : "")
            + "\n" + text + (text.ends_with('\n') ? "" : "\n") + fence;
    }
    if (content == node.end()) return {};
    std::string text;
    std::int64_t number = attrs != node.end() ? attrs->value("start", 1) : 1;
    for (const auto& child : *content) {
        const auto part = document_markdown(child);
        if (type == "bulletList" || type == "orderedList") {
            const auto marker = type == "bulletList" ? "- " : std::to_string(number++) + ". ";
            if (!text.empty()) text += '\n';
            text += markdown_list_item(part, marker);
        } else {
            if (!text.empty() && !part.empty() && type != "paragraph" && type != "heading") text += "\n\n";
            text += part;
        }
    }
    if (type == "heading") {
        // Note headings sit one level below the "# Note:" line of the chat copy.
        const int level = attrs != node.end() ? attrs->value("level", 1) : 1;
        return std::string(std::clamp(level, 1, 5) + 1, '#') + " " + text;
    }
    return text;
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
        {"has_compressed", item.has_compressed},
        {"read_only", item.read_only},
    };
}

void to_json(nlohmann::json& json, const NoteContent& content) {
    json = nlohmann::json{
        {"id", content.id},
        {"document", content.document},
        {"scroll_top", content.scroll_top},
    };
    if (content.compressed_text) json["compressed_text"] = *content.compressed_text;
}

std::string note_document_markdown(const nlohmann::json& document) {
    return document_markdown(document);
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
    compressed_text TEXT,
    read_only INTEGER NOT NULL DEFAULT 0 CHECK (read_only IN (0, 1)),
    scroll_top INTEGER NOT NULL DEFAULT 0 CHECK (scroll_top >= 0 AND scroll_top <= 9007199254740991),
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

CREATE TABLE IF NOT EXISTS session_files (
    session_key INTEGER NOT NULL REFERENCES sessions(session_key) ON DELETE CASCADE,
    name TEXT NOT NULL,
    text TEXT NOT NULL,
    PRIMARY KEY (session_key, name)
) STRICT;
)sql");
    bool has_compressed_column = false;
    bool has_read_only_column = false;
    bool has_scroll_top_column = false;
    {
        auto columns = database.prepare("PRAGMA table_info(note_items)");
        while (columns.step()) {
            if (columns.text(1) == "compressed_text") has_compressed_column = true;
            if (columns.text(1) == "read_only") has_read_only_column = true;
            if (columns.text(1) == "scroll_top") has_scroll_top_column = true;
        }
    }
    if (!has_compressed_column) {
        database.execute("ALTER TABLE note_items ADD COLUMN compressed_text TEXT");
    }
    if (!has_read_only_column) {
        database.execute("ALTER TABLE note_items ADD COLUMN read_only INTEGER NOT NULL DEFAULT 0 CHECK (read_only IN (0, 1))");
    }
    if (!has_scroll_top_column) {
        database.execute("ALTER TABLE note_items ADD COLUMN scroll_top INTEGER NOT NULL DEFAULT 0 CHECK (scroll_top >= 0 AND scroll_top <= 9007199254740991)");
    }
}

std::vector<NoteItem> note_list(storage::SqliteDatabase& database) {
    try {
        storage::SqliteTransaction transaction(database);
        storage::SqliteStatement statement = database.prepare(
            "SELECT id, parent_id, kind, name, compressed_text IS NOT NULL, read_only "
            "FROM note_items ORDER BY id");
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

std::int64_t note_import_book(
    storage::SqliteDatabase& database,
    std::optional<std::int64_t> parent_id,
    std::string_view name,
    const std::vector<NoteImportEntry>& notes) {
    require_name(name);
    if (notes.empty()) fail(NoteFailure::invalid, "The book has no reading-order files.");
    try {
        storage::SqliteTransaction transaction(database);
        if (parent_id) require_folder(database, *parent_id);
        if (name_taken(database, parent_id, name, 0)) {
            fail(NoteFailure::invalid, duplicate_name_message);
        }
        auto folder = database.prepare(
            "INSERT INTO note_items (parent_id, kind, name) VALUES (?1, 'folder', ?2)");
        if (parent_id) folder.bind(1, *parent_id);
        else folder.bind_null(1);
        folder.bind(2, name);
        folder.run();
        const auto id = database.last_insert_rowid();
        for (const auto& note : notes) {
            require_name(note.name);
            const auto compact = compact_document(note.document, 0, true);
            auto insert = database.prepare(
                "INSERT INTO note_items (parent_id, kind, name, content_json, read_only) "
                "VALUES (?1, 'note', ?2, ?3, 1)");
            insert.bind(1, id);
            insert.bind(2, note.name);
            insert.bind(3, compact);
            insert.run();
        }
        transaction.commit();
        return id;
    } catch (const std::runtime_error& error) {
        throw sql_failure(error, NoteFailure::saving, change_message);
    }
}

NoteContent note_get(storage::SqliteDatabase& database, std::int64_t id) {
    try {
        storage::SqliteTransaction transaction(database);
        storage::SqliteStatement statement = database.prepare(
            "SELECT kind, content_json, compressed_text, scroll_top FROM note_items WHERE id = ?1");
        statement.bind(1, id);
        if (!statement.step()) fail(NoteFailure::not_found, missing_note_message);
        if (statement.text(0) != "note") {
            fail(NoteFailure::invalid, not_note_message);
        }
        if (statement.is_null(1)) fail(NoteFailure::opening, opening_message);
        nlohmann::json document = parse_document(statement.text(1), id);
        NoteContent content{id, std::move(document), std::nullopt};
        if (!statement.is_null(2)) content.compressed_text = statement.text(2);
        content.scroll_top = statement.integer(3);
        transaction.commit();
        return content;
    } catch (const std::runtime_error& error) {
        throw sql_failure(error, NoteFailure::opening, opening_message);
    }
}

NoteText note_get_text(
    storage::SqliteDatabase& database, std::int64_t id, bool prefer_compressed) {
    try {
        auto statement = database.prepare(
            "SELECT name, content_json, compressed_text FROM note_items "
            "WHERE id = ?1 AND kind = 'note'", id);
        if (!statement.step()) fail(NoteFailure::not_found, missing_note_message);
        return {id, statement.text(0),
            prefer_compressed && !statement.is_null(2)
                ? statement.text(2)
                : note_document_markdown(parse_document(statement.text(1), id))};
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
        const auto item = find_item(database, id);
        if (!item) fail(NoteFailure::not_found, missing_note_message);
        if (item->kind != NoteItemKind::note) fail(NoteFailure::invalid, not_note_message);
        if (item->read_only) {
            auto stored = database.prepare("SELECT content_json FROM note_items WHERE id = ?1", id);
            if (!stored.step()) fail(NoteFailure::not_found, missing_note_message);
            if (without_highlights(parse_document(stored.text(0), id)) != without_highlights(document)) {
                fail(NoteFailure::invalid, "This note is read-only. Make it editable first.");
            }
        }
        storage::SqliteStatement update = database.prepare(
            "UPDATE note_items SET content_json = ?1 "
            "WHERE id = ?2 AND kind = 'note'");
        update.bind(1, compact);
        update.bind(2, id);
        update.run();
        transaction.commit();
    } catch (const std::runtime_error& error) {
        throw sql_failure(error, NoteFailure::saving, saving_message);
    }
}

void note_save_scroll_position(
    storage::SqliteDatabase& database, std::int64_t id, std::int64_t scroll_top) {
    if (scroll_top < 0 || scroll_top > note_id_limit) {
        fail(NoteFailure::invalid, "The scroll position is not valid.");
    }
    try {
        storage::SqliteTransaction transaction(database);
        const auto item = find_item(database, id);
        if (!item) fail(NoteFailure::not_found, missing_note_message);
        if (item->kind != NoteItemKind::note) fail(NoteFailure::invalid, not_note_message);
        auto update = database.prepare(
            "UPDATE note_items SET scroll_top = ?1 WHERE id = ?2", scroll_top, id);
        update.run();
        transaction.commit();
    } catch (const std::runtime_error& error) {
        throw sql_failure(error, NoteFailure::saving, change_message);
    }
}

NoteItem note_set_read_only(
    storage::SqliteDatabase& database, std::int64_t id, bool read_only) {
    try {
        storage::SqliteTransaction transaction(database);
        auto item = find_item(database, id);
        if (!item) fail(NoteFailure::not_found, missing_note_message);
        if (item->kind != NoteItemKind::note) fail(NoteFailure::invalid, not_note_message);
        auto update = database.prepare("UPDATE note_items SET read_only = ?1 WHERE id = ?2");
        update.bind(1, static_cast<std::int64_t>(read_only));
        update.bind(2, id);
        update.run();
        item->read_only = read_only;
        transaction.commit();
        return *item;
    } catch (const std::runtime_error& error) {
        throw sql_failure(error, NoteFailure::saving, change_message);
    }
}

void note_save_compressed(
    storage::SqliteDatabase& database, std::int64_t id, std::string_view text) {
    if (text.find_first_not_of(" \t\r\n") == std::string_view::npos) {
        fail(NoteFailure::invalid, "The provider returned no compressed text.");
    }
    try {
        storage::SqliteTransaction transaction(database);
        auto update = database.prepare(
            "UPDATE note_items SET compressed_text = ?1 WHERE id = ?2 AND kind = 'note'");
        update.bind(1, text);
        update.bind(2, id);
        update.run();
        if (database.changes() != 1) fail(NoteFailure::not_found, missing_note_message);
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
