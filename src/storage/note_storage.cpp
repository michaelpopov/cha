#include "storage/note_storage.h"

#include "storage/folder_store_storage.h"
#include "storage/session_label.h"
#include "util/logging.h"

#include <zlib.h>

#include <algorithm>
#include <fstream>
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
    case NoteItemKind::annotation: return "annotation";
    }
    fail(NoteFailure::invalid, kind_message);
}

void require_name(std::string_view name) {
    if (name == "ANNOTATIONS") fail(NoteFailure::invalid, "The name ANNOTATIONS is reserved.");
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
    else if (kind == "annotation") item.kind = NoteItemKind::annotation;
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

std::string compress_document(std::string_view text) {
    uLongf size = compressBound(static_cast<uLong>(text.size()));
    std::string bytes(size, '\0');
    if (compress(reinterpret_cast<Bytef*>(bytes.data()), &size,
            reinterpret_cast<const Bytef*>(text.data()), static_cast<uLong>(text.size())) != Z_OK) {
        throw std::runtime_error("Note compression failed");
    }
    bytes.resize(size);
    return bytes;
}

nlohmann::json parse_document(std::string_view bytes, std::int64_t id) {
    uLongf size = note_document_byte_limit;
    std::string text(size, '\0');
    const int result = uncompress(reinterpret_cast<Bytef*>(text.data()), &size,
        reinterpret_cast<const Bytef*>(bytes.data()), static_cast<uLong>(bytes.size()));
    if (result == Z_BUF_ERROR) fail(NoteFailure::too_large, too_large_message);
    if (result != Z_OK) {
        log_error("Note decompression failed for id " + std::to_string(id));
        fail(NoteFailure::opening, opening_message);
    }
    text.resize(size);
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(text);
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

bool is_note_comment(const nlohmann::json& node) {
    if (!node.is_object() || !node.contains("type") || node["type"] != "paragraph"
        || !node.contains("content") || !node["content"].is_array() || node["content"].empty()) return false;
    bool has_text = false;
    for (const auto& child : node["content"]) {
        if (!child.is_object() || !child.contains("type")) return false;
        if (child["type"] == "hardBreak") continue;
        if (child["type"] != "text" || !child.contains("text") || !child["text"].is_string()
            || !child.contains("marks") || !child["marks"].is_array()) return false;
        has_text = has_text || !child["text"].get_ref<const std::string&>().empty();
        const bool blue = std::any_of(child["marks"].begin(), child["marks"].end(), [](const auto& mark) {
            return mark.is_object() && mark.contains("type") && mark["type"] == "highlight"
                && mark.contains("attrs") && mark["attrs"].is_object()
                && mark["attrs"].contains("color") && mark["attrs"]["color"] == "#b3d9ff";
        });
        if (!blue) return false;
    }
    return has_text;
}


using Json = nlohmann::json;
bool annotation_space(char32_t c) {
    return (c >= 9 && c <= 13) || c == 32 || c == 0xa0 || c == 0x1680
        || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029
        || c == 0x202f || c == 0x205f || c == 0x3000 || c == 0xfeff;
}
// Stored JSON has already passed UTF-8 serialization validation.
std::string_view annotation_character(std::string_view text, std::size_t& offset) {
    const auto c = static_cast<unsigned char>(text[offset]);
    const std::size_t size = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
    const auto result = text.substr(offset, size);
    offset += size;
    return result;
}
bool annotation_space(std::string_view text) {
    char32_t c = static_cast<unsigned char>(text[0]);
    if (text.size() > 1) {
        c &= text.size() == 2 ? 0x1f : text.size() == 3 ? 0x0f : 0x07;
        for (std::size_t i = 1; i < text.size(); ++i) c = (c << 6) | (static_cast<unsigned char>(text[i]) & 0x3f);
    }
    return annotation_space(c);
}
bool annotation_blank(std::string_view text) {
    for (std::size_t offset = 0; offset < text.size();)
        if (!annotation_space(annotation_character(text, offset))) return false;
    return true;
}
std::string annotation_type(const Json& node) {
    const auto type = node.find("type");
    return type != node.end() && type->is_string() ? type->get<std::string>() : "";
}
std::string annotation_text(const Json& node) {
    if (!node.is_object()) return {};
    if (annotation_type(node) == "hardBreak") return "\n";
    if (annotation_type(node) == "text") {
        const auto text = node.find("text");
        return text != node.end() && text->is_string() ? text->get<std::string>() : "";
    }
    std::string text;
    const auto content = node.find("content");
    if (content != node.end() && content->is_array()) for (const auto& child : *content) text += annotation_text(child);
    return text;
}
// Like noteHeadings: each whitespace run becomes one space, then trim.
std::string annotation_label_text(std::string_view text) {
    std::string result;
    bool space = false;
    for (std::size_t offset = 0; offset < text.size();) {
        const auto c = annotation_character(text, offset);
        if (annotation_space(c)) { space = !result.empty(); continue; }
        if (space) result += ' ';
        result += c;
        space = false;
    }
    return result;
}
std::string annotation_heading(const Json& node) {
    if (!node.is_object() || annotation_type(node) != "heading") return {};
    return annotation_label_text(annotation_text(node));
}
struct AnnotationEntry {
    std::string type, text;
    int ordinal{}, chapter_id{};
};
struct ExtractedAnnotations {
    std::vector<AnnotationEntry> entries;
    // Index zero means that no chapter precedes the entry.
    std::vector<std::string> chapters{ "" };
};
ExtractedAnnotations extract_annotations(const Json& doc) {
    ExtractedAnnotations result;
    int comments = 0, highlights = 0, chapter_id = 0;
    const auto emit = [&](std::string type, std::string text, int label_id) {
        if (annotation_blank(text)) return;
        const int ordinal = type == "comment" ? ++comments : ++highlights;
        result.entries.push_back({std::move(type), std::move(text), ordinal, label_id});
    };
    const auto walk = [&](auto&& self, const Json& node) -> void {
        if (!node.is_object()) return;
        const auto heading = annotation_heading(node);
        if (!heading.empty()) {
            chapter_id = static_cast<int>(result.chapters.size());
            result.chapters.push_back(heading);
        }
        const auto children = node.find("content");
        if (children == node.end() || !children->is_array()) return;
        const auto type = annotation_type(node);
        // Comments never get here: the container loop below emits them.
        if (type == "paragraph" || type == "heading" || type == "codeBlock") {
            std::string passage;
            for (const auto& child : *children) {
                bool marked = false;
                if (child.is_object()) {
                    const auto marks = child.find("marks");
                    if (marks != child.end() && marks->is_array()) {
                        for (const auto& mark : *marks)
                            if (mark.is_object() && annotation_type(mark) == "highlight") marked = true;
                    }
                }
                const auto child_type = child.is_object() ? annotation_type(child) : "";
                if (marked && (child_type == "text" || child_type == "hardBreak")) passage += annotation_text(child);
                else { emit("highlight", std::move(passage), chapter_id); passage.clear(); }
            }
            emit("highlight", std::move(passage), chapter_id);
            return;
        }
        for (std::size_t i = 0; i < children->size(); ++i) {
            const auto& child = (*children)[i];
            if (!is_note_comment(child)) { self(self, child); continue; }
            std::size_t next = i + 1;
            while (next < children->size() && is_note_comment((*children)[next])) ++next;
            const bool before_heading = next < children->size() && !annotation_heading((*children)[next]).empty();
            const auto label_id = before_heading ? static_cast<int>(result.chapters.size()) : chapter_id;
            // Resolve a whole run once. The following heading gets this index when visited.
            for (; i < next; ++i) emit("comment", annotation_text((*children)[i]), label_id);
            --i;
        }
    };
    walk(walk, doc);
    return result;
}
Json annotation_label(std::string_view text, int level) {
    return {{"type", "heading"}, {"attrs", {{"level", level}}}, {"content", Json::array({{{"type", "text"}, {"text", text}}})}};
}
// A save may create the row only from its own document; imports may use any imported note.
bool rebuild_folder_annotations(storage::SqliteDatabase& database, std::optional<std::int64_t> parent_id,
    bool allow_creation, const Json* creation_document = nullptr) {
    auto existing = database.prepare("SELECT id FROM note_items WHERE IFNULL(parent_id, 0) = ?1 AND kind = 'annotation'", parent_key(parent_id));
    const auto id = existing.step() ? existing.integer(0) : 0;
    if (!id) {
        if (!allow_creation) return false;
        if (creation_document && extract_annotations(*creation_document).entries.empty()) return false;
    }
    Json doc = {{"type", "doc"}, {"content", Json::array()}};
    std::size_t bytes = doc.dump().size();
    bool eligible = false, limited = false;
    auto sources = database.prepare("SELECT id, name, content_json FROM note_items WHERE IFNULL(parent_id, 0) = ?1 AND kind = 'note' ORDER BY id", parent_key(parent_id));
    while (!limited && sources.step()) {
        const auto source_id = sources.integer(0);
        const auto name = sources.text(1);
        const auto entries = extract_annotations(parse_document(sources.blob(2), source_id));
        bool first = true;
        int previous_chapter = 0;
        for (const auto& entry : entries.entries) {
            const auto& chapter = entries.chapters[entry.chapter_id];
            eligible = true;
            Json candidate = Json::array();
            if (first) candidate.push_back(annotation_label(name, 1));
            // A chapter label that repeats the note name or the entry text adds nothing.
            if (entry.chapter_id != previous_chapter && !chapter.empty() && chapter != name
                && annotation_label_text(entry.text) != chapter) candidate.push_back(annotation_label(chapter, 2));
            Json content = Json::array({{{"type", "annotationSource"}, {"attrs", {{"sourceId", source_id}, {"entryType", entry.type}, {"ordinal", entry.ordinal}}}}});
            std::string segment;
            const auto flush = [&] { if (!segment.empty()) { content.push_back({{"type", "text"}, {"text", segment}}); segment.clear(); } };
            for (std::size_t i = 0, offset = 0; i < 4000 && offset < entry.text.size(); ++i) {
                const auto c = annotation_character(entry.text, offset);
                if (c == "\n") { flush(); content.push_back({{"type", "hardBreak"}}); }
                else segment += c;
            }
            flush();
            candidate.push_back({{"type", "paragraph"}, {"content", std::move(content)}});
            std::size_t added = 0;
            for (const auto& node : candidate) added += node.dump().size() + 1;
            if (doc["content"].empty()) --added;
            if (bytes + added > 10000000) { limited = true; break; }
            bytes += added;
            for (auto& node : candidate) doc["content"].push_back(std::move(node));
            first = false;
            previous_chapter = entry.chapter_id;
        }
    }
    if (!id && !eligible) return false;
    if (limited) log_warn("ANNOTATIONS reached the 10 MB limit; remaining entries were omitted.");
    if (doc["content"].empty()) {
        if (eligible) doc = Json::parse(empty_note_document);
        else doc["content"].push_back({{"type", "paragraph"}, {"content", Json::array({{{"type", "text"}, {"text", "No comments or highlights."}}})}});
    }
    const auto compressed = compress_document(doc.dump());
    if (id) {
        auto update = database.prepare("UPDATE note_items SET content_json = ?1 WHERE id = ?2");
        update.bind_blob(1, compressed);
        update.bind(2, id);
        update.run();
    } else {
        auto insert = database.prepare("INSERT INTO note_items(parent_id, kind, name, content_json, read_only) VALUES (?1, 'annotation', 'ANNOTATIONS', ?2, 1)");
        insert.bind(1, parent_id);
        insert.bind_blob(2, compressed);
        insert.run();
    }
    return !id;
}

// Non-comment blocks must remain in order. Allow comment additions/deletions
// and highlight changes, including inside list items.
bool read_only_note_change(const nlohmann::json& before, const nlohmann::json& after) {
    if (without_highlights(before) == without_highlights(after)) return true;
    // ProseMirror keeps an empty paragraph when a required paragraph is deleted.
    if (is_note_comment(before) && after == nlohmann::json{{"type", "paragraph"}}) return true;
    if (!before.is_object() || !after.is_object()
        || !before.contains("content") || !before["content"].is_array()
        || !after.contains("content") || !after["content"].is_array()) return false;
    auto before_shape = before;
    auto after_shape = after;
    before_shape.erase("content");
    after_shape.erase("content");
    if (before_shape != after_shape) return false;
    const auto& original = before["content"];
    // A comment may repeat an existing paragraph's text. Keep both possible
    // matches until the next block distinguishes the insertion from the original.
    std::vector<bool> matched(original.size() + 1, false);
    matched[0] = true;
    for (std::size_t count = 1; count <= original.size(); ++count) {
        matched[count] = matched[count - 1] && is_note_comment(original[count - 1]);
    }
    for (const auto& child : after["content"]) {
        const bool comment = is_note_comment(child);
        for (std::size_t count = original.size(); count > 0; --count) {
            matched[count] = (comment && matched[count])
                || (matched[count - 1] && read_only_note_change(original[count - 1], child));
        }
        matched[0] = comment && matched[0];
        for (std::size_t count = 1; count <= original.size(); ++count) {
            if (matched[count - 1] && is_note_comment(original[count - 1])) matched[count] = true;
        }
    }
    return matched.back();
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

nlohmann::json note_parse_saved_document(std::string_view bytes, std::int64_t id) {
    return parse_document(bytes, id);
}

void create_note_items_schema(storage::SqliteDatabase& database) {
    bool legacy_notes = false;
    {
        auto columns = database.prepare("PRAGMA table_info(note_items)");
        while (columns.step()) {
            if (columns.text(1) == "content_json" && columns.text(2) == "TEXT") legacy_notes = true;
        }
    }
    // Existing uncompressed notes are deliberately discarded during this transition.
    if (legacy_notes) database.execute("DROP TABLE note_items");
    database.execute(R"sql(
CREATE TABLE IF NOT EXISTS note_items (
    id INTEGER PRIMARY KEY AUTOINCREMENT
        CHECK (id > 0 AND id <= 9007199254740991),
    parent_id INTEGER REFERENCES note_items(id) ON DELETE CASCADE,
    kind TEXT NOT NULL CHECK (kind IN ('folder', 'note', 'annotation')),
    name TEXT NOT NULL COLLATE BINARY,
    content_json BLOB,
    compressed_text TEXT,
    read_only INTEGER NOT NULL DEFAULT 0 CHECK (read_only IN (0, 1)),
    scroll_top INTEGER NOT NULL DEFAULT 0 CHECK (scroll_top >= 0 AND scroll_top <= 9007199254740991),
    CHECK (
        (kind = 'folder' AND content_json IS NULL)
        OR (kind IN ('note', 'annotation') AND content_json IS NOT NULL)
    ),
    CHECK ((kind = 'annotation' AND name = 'ANNOTATIONS' AND read_only = 1 AND compressed_text IS NULL)
        OR (kind IN ('folder', 'note') AND name <> 'ANNOTATIONS'))
) STRICT;
CREATE UNIQUE INDEX IF NOT EXISTS note_items_annotation_parent
    ON note_items(IFNULL(parent_id, 0)) WHERE kind = 'annotation';
CREATE UNIQUE INDEX IF NOT EXISTS note_items_sibling_name
    ON note_items(IFNULL(parent_id, 0), name);
CREATE INDEX IF NOT EXISTS note_items_parent
    ON note_items(parent_id);

CREATE TABLE IF NOT EXISTS note_audio_attempts (
    id INTEGER PRIMARY KEY AUTOINCREMENT CHECK (id > 0 AND id <= 9007199254740991),
    note_id INTEGER NOT NULL REFERENCES note_items(id) ON DELETE RESTRICT,
    note_name TEXT NOT NULL,
    clip_count INTEGER NOT NULL CHECK (clip_count BETWEEN 1 AND 1000),
    service_origin TEXT NOT NULL,
    api_key_id TEXT NOT NULL,
    batch_id TEXT
) STRICT;
CREATE UNIQUE INDEX IF NOT EXISTS note_audio_attempts_note ON note_audio_attempts(note_id);

CREATE TABLE IF NOT EXISTS note_audio (
    note_id INTEGER NOT NULL REFERENCES note_items(id) ON DELETE CASCADE,
    ordinal INTEGER NOT NULL CHECK (ordinal >= 0),
    mime_type TEXT NOT NULL,
    audio BLOB NOT NULL,
    PRIMARY KEY (note_id, ordinal)
) STRICT;

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
    if (kind == NoteItemKind::annotation) fail(NoteFailure::invalid, kind_message);
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
        else statement.bind_blob(4, compress_document(empty_note_document));
        statement.run();
        const std::int64_t id = database.last_insert_rowid();
        if (kind == NoteItemKind::note) rebuild_folder_annotations(database, parent_id, false);
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
            insert.bind_blob(3, compress_document(compact));
            insert.run();
        }
        rebuild_folder_annotations(database, id, true);
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
        if ((statement.text(0) != "note" && statement.text(0) != "annotation")) {
            fail(NoteFailure::invalid, not_note_message);
        }
        if (statement.is_null(1)) fail(NoteFailure::opening, opening_message);
        nlohmann::json document = parse_document(statement.blob(1), id);
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
            "WHERE id = ?1 AND kind IN ('note', 'annotation')", id);
        if (!statement.step()) fail(NoteFailure::not_found, missing_note_message);
        return {id, statement.text(0),
            prefer_compressed && !statement.is_null(2)
                ? statement.text(2)
                : note_document_markdown(parse_document(statement.blob(1), id))};
    } catch (const nlohmann::json::exception&) {
        fail(NoteFailure::opening, opening_message);
    } catch (const std::runtime_error& error) {
        throw sql_failure(error, NoteFailure::opening, opening_message);
    }
}

bool note_save(
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
            if (!read_only_note_change(parse_document(stored.blob(0), id), document)) {
                fail(NoteFailure::invalid, "This note is read-only. Make it editable first.");
            }
        }
        storage::SqliteStatement update = database.prepare(
            "UPDATE note_items SET content_json = ?1 "
            "WHERE id = ?2 AND kind = 'note'");
        update.bind_blob(1, compress_document(compact));
        update.bind(2, id);
        update.run();
        const bool created = rebuild_folder_annotations(database, item->parent_id, true, &document);
        transaction.commit();
        return created;
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
        if (item->kind == NoteItemKind::folder) fail(NoteFailure::invalid, not_note_message);
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
        if (!read_only) {
            const auto attempt = note_audio_attempt_for_note(database, id);
            if (attempt) {
                event_append_in_transaction(database, EventSeverity::warning,
                    "Audio generation abandoned for " + note_audio_target(*attempt) + ": note made editable.");
                database.prepare("DELETE FROM note_audio_attempts WHERE id=?", attempt->id).run();
            }
        }
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
        const auto item = find_item(database, id);
        if (item && item->kind == NoteItemKind::annotation) fail(NoteFailure::invalid, not_note_message);
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

bool note_commit_compressed(storage::SqliteDatabase& database, std::int64_t id,
    std::string_view source, std::string_view text, std::string_view success_message) {
    if (text.find_first_not_of(" \t\r\n") == std::string_view::npos)
        fail(NoteFailure::invalid, "The provider returned no compressed text.");
    try {
        storage::SqliteTransaction transaction(database);
        if (note_get_text(database, id).text != source) return false;
        database.prepare("UPDATE note_items SET compressed_text=? WHERE id=? AND kind='note'", text, id).run();
        if (database.changes() != 1) fail(NoteFailure::not_found, missing_note_message);
        event_append_in_transaction(database, EventSeverity::success, success_message);
        transaction.commit();
        return true;
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
        if (item.kind == NoteItemKind::annotation) fail(NoteFailure::invalid, not_note_message);
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
        if (item.kind == NoteItemKind::note) rebuild_folder_annotations(database, item.parent_id, false);
        transaction.commit();
        return item;
    } catch (const std::runtime_error& error) {
        throw sql_failure(error, NoteFailure::saving, change_message);
    }
}

void note_delete_in_transaction(storage::SqliteDatabase& database, std::int64_t id) {
    const auto item = find_item(database, id);
    if (item && item->kind == NoteItemKind::annotation) fail(NoteFailure::invalid, not_note_message);
    if (folder_store_owns_subtree(database, id)) {
        fail(NoteFailure::invalid,
            "This folder has a store. Remove the store before deleting it.");
    }
    auto descendants = database.prepare(
        "WITH RECURSIVE tree(id) AS (SELECT id FROM note_items WHERE id=? "
        "UNION ALL SELECT n.id FROM note_items n JOIN tree t ON n.parent_id=t.id) "
        "SELECT a.id FROM note_audio_attempts a JOIN tree t ON a.note_id=t.id ORDER BY a.id", id);
    std::vector<std::int64_t> attempt_ids;
    while (descendants.step()) attempt_ids.push_back(descendants.integer(0));
    for (const auto attempt_id : attempt_ids) {
        const auto attempt = note_audio_attempt_by_id(database, attempt_id);
        event_append_in_transaction(database, EventSeverity::warning,
            "Audio generation abandoned for " + note_audio_target(*attempt) + ": note deleted.");
        database.prepare("DELETE FROM note_audio_attempts WHERE id=?", attempt_id).run();
    }
    folder_store_delete_session_folders_in_subtree(database, id);
    storage::SqliteStatement statement = database.prepare(
        "DELETE FROM note_items WHERE id = ?1");
    statement.bind(1, id);
    statement.run();
    if (database.changes() != 1) {
        fail(NoteFailure::not_found, missing_item_message);
    }
    if (item && item->kind == NoteItemKind::note) rebuild_folder_annotations(database, item->parent_id, false);
}

void note_delete(storage::SqliteDatabase& database, std::int64_t id) {
    try {
        storage::SqliteTransaction transaction(database);
        note_delete_in_transaction(database, id);
        transaction.commit();
    } catch (const std::runtime_error& error) {
        throw sql_failure(error, NoteFailure::saving, change_message);
    }
}

nlohmann::json note_audio_info(storage::SqliteDatabase& database, std::int64_t id) {
    auto statement = database.prepare("SELECT COUNT(*) FROM note_audio WHERE note_id=?", id);
    (void)statement.step();
    return {{"count", statement.integer(0)}};
}

NoteAudioClip note_audio_clip(storage::SqliteDatabase& database, std::int64_t id, std::int64_t ordinal) {
    auto statement = database.prepare("SELECT mime_type, audio FROM note_audio WHERE note_id=? AND ordinal=?", id, ordinal);
    if (!statement.step()) throw NoteError(NoteFailure::not_found, "Note audio is not available.");
    return {statement.text(0), statement.blob(1)};
}

static void save_audio_in_transaction(storage::SqliteDatabase& database, std::int64_t id,
    const std::vector<std::filesystem::path>& clips) {
    auto note = database.prepare("SELECT 1 FROM note_items WHERE id=? AND kind='note' AND read_only=1", id);
    if (!note.step()) throw NoteError(NoteFailure::not_found, "That note was not found.");
    if (clips.empty()) throw NoteError(NoteFailure::invalid, "No audio clips to save.");
    database.prepare("DELETE FROM note_audio WHERE note_id=?", id).run();
    for (std::size_t i = 0; i < clips.size(); ++i) {
        const auto size = std::filesystem::file_size(clips[i]);
        if (size == 0 || size > 32 * 1024 * 1024) throw NoteError(NoteFailure::invalid, "Invalid compressed audio size.");
        std::string bytes(static_cast<std::size_t>(size), '\0');
        std::ifstream input(clips[i], std::ios::binary);
        if (!input.read(bytes.data(), static_cast<std::streamsize>(bytes.size())))
            throw std::filesystem::filesystem_error("Could not read compressed audio", clips[i], std::make_error_code(std::errc::io_error));
        auto insert = database.prepare("INSERT INTO note_audio (note_id, ordinal, mime_type, audio) VALUES (?,?,?,?)",
            id, static_cast<std::int64_t>(i), "audio/mp4");
        insert.bind_blob(4, bytes);
        insert.run();
    }
}


void note_save_audio(storage::SqliteDatabase& database, std::int64_t id,
    const std::vector<std::filesystem::path>& clips) {
    storage::SqliteTransaction transaction(database);
    save_audio_in_transaction(database, id, clips);
    transaction.commit();
}

std::string note_audio_target(const NoteAudioAttempt& attempt) {
    return "“" + attempt.note_name + "” (note " + std::to_string(attempt.note_id) + ")";
}

namespace {
NoteAudioAttempt decode_audio_attempt(storage::SqliteStatement& row) {
    NoteAudioAttempt result{row.integer(0), row.integer(1), row.integer(2), row.text(3), row.text(4), row.text(5), {}};
    if (!row.is_null(6)) result.batch_id = row.text(6);
    return result;
}
}

std::optional<NoteAudioAttempt> note_audio_attempt_by_id(storage::SqliteDatabase& database, std::int64_t id) {
    auto row = database.prepare("SELECT id, note_id, clip_count, note_name, service_origin, api_key_id, batch_id FROM note_audio_attempts WHERE id=?", id);
    if (!row.step()) return {};
    return decode_audio_attempt(row);
}
std::optional<NoteAudioAttempt> note_audio_attempt_for_note(storage::SqliteDatabase& database, std::int64_t id) {
    auto row = database.prepare("SELECT id, note_id, clip_count, note_name, service_origin, api_key_id, batch_id FROM note_audio_attempts WHERE note_id=?", id);
    if (!row.step()) return {};
    return decode_audio_attempt(row);
}
std::vector<NoteAudioAttempt> note_audio_attempts(storage::SqliteDatabase& database) {
    auto row = database.prepare("SELECT id, note_id, clip_count, note_name, service_origin, api_key_id, batch_id FROM note_audio_attempts ORDER BY id");
    std::vector<NoteAudioAttempt> result;
    while (row.step()) result.push_back(decode_audio_attempt(row));
    return result;
}
std::optional<NoteAudioAttempt> note_audio_accept(storage::SqliteDatabase& database, std::int64_t note_id,
    std::int64_t count, std::string_view origin, std::string_view key_id,
    const std::function<void(const NoteAudioAttempt&)>& launch, bool only_if_missing) {
    storage::SqliteTransaction transaction(database);
    const auto item = find_item(database, note_id);
    const auto pending = note_audio_attempt_for_note(database, note_id);
    if (only_if_missing && (!item || item->kind != NoteItemKind::note || !item->read_only
        || pending || note_audio_info(database, note_id).at("count").get<std::int64_t>() > 0)) return {};
    if (pending)
        throw NoteError(NoteFailure::invalid, "Audio generation is already running for " + note_audio_target(*pending)
            + ". Make that note editable to abandon the attempt.");
    if (!item || item->kind != NoteItemKind::note || !item->read_only)
        throw NoteError(NoteFailure::invalid, "Make the note read-only to generate audio.");
    database.prepare("INSERT INTO note_audio_attempts(note_id,note_name,clip_count,service_origin,api_key_id) "
        "VALUES (?,?,?,?,?)", note_id, item->name, count, origin, key_id).run();
    const auto attempt = *note_audio_attempt_by_id(database, database.last_insert_rowid());
    launch(attempt);
    transaction.commit();
    return attempt;
}

bool note_audio_register(storage::SqliteDatabase& database, std::int64_t attempt_id,
    std::string_view batch_id, std::string_view voice) {
    storage::SqliteTransaction transaction(database);
    const auto attempt = note_audio_attempt_by_id(database, attempt_id);
    if (!attempt) return false;
    if (attempt->batch_id) {
        if (*attempt->batch_id != batch_id) throw std::runtime_error("Audio batch ID changed.");
        return true;
    }
    database.prepare("UPDATE note_audio_attempts SET batch_id=? WHERE id=?", batch_id, attempt_id).run();
    event_append_in_transaction(database, EventSeverity::info, "Audio batch started for " + note_audio_target(*attempt)
        + ": " + std::to_string(attempt->clip_count) + " clips, voice " + std::string(voice) + ".");
    transaction.commit();
    return true;
}

bool note_audio_finish(storage::SqliteDatabase& database, std::int64_t attempt_id,
    const std::vector<std::filesystem::path>& clips) {
    storage::SqliteTransaction transaction(database);
    const auto attempt = note_audio_attempt_by_id(database, attempt_id);
    if (!attempt) return false;
    if (clips.size() != static_cast<std::size_t>(attempt->clip_count))
        throw NoteError(NoteFailure::invalid, "Incomplete audio batch.");
    save_audio_in_transaction(database, attempt->note_id, clips);
    event_append_in_transaction(database, EventSeverity::success, "Audio ready for " + note_audio_target(*attempt)
        + ": " + std::to_string(attempt->clip_count) + " clips saved.");
    database.prepare("DELETE FROM note_audio_attempts WHERE id=?", attempt_id).run();
    transaction.commit();
    return true;
}

bool note_audio_retire(storage::SqliteDatabase& database, std::int64_t attempt_id,
    EventSeverity severity, std::string_view message) {
    storage::SqliteTransaction transaction(database);
    const auto attempt = note_audio_attempt_by_id(database, attempt_id);
    if (!attempt) return false;
    event_append_in_transaction(database, severity, message);
    database.prepare("DELETE FROM note_audio_attempts WHERE id=?", attempt_id).run();
    transaction.commit();
    return true;
}

} // namespace cha
