#include "bridge/operation_dispatch.h"

#include "app/application.h"
#include "bridge/request_params.h"
#include "storage/note_storage.h"
#include "util/path_name.h"
#include "util/text_file.h"

#include <stdexcept>

namespace cha::bridge {
namespace {

std::int64_t require_note_id(const nlohmann::json& params) {
    if (!params.contains("id")) {
        throw std::invalid_argument("The request was not valid.");
    }
    const auto id = as_safe_uint(params.at("id"));
    if (!id || *id < 1) {
        throw std::invalid_argument("The item id is not valid.");
    }
    return static_cast<std::int64_t>(*id);
}

std::optional<std::int64_t> require_parent_id(const nlohmann::json& params) {
    if (!params.contains("parent_id")) {
        throw std::invalid_argument("The request was not valid.");
    }
    const nlohmann::json& parent = params.at("parent_id");
    if (parent.is_null()) return std::nullopt;
    const auto id = as_safe_uint(parent);
    if (!id || *id < 1) {
        throw std::invalid_argument("The parent is not valid.");
    }
    return static_cast<std::int64_t>(*id);
}

NoteItemKind require_kind(const nlohmann::json& params) {
    if (!params.contains("kind") || !params.at("kind").is_string()) {
        throw std::invalid_argument("The request was not valid.");
    }
    const std::string& kind = params.at("kind").get_ref<const std::string&>();
    if (kind == "folder") return NoteItemKind::folder;
    if (kind == "note") return NoteItemKind::note;
    throw std::invalid_argument("Kind must be folder or note.");
}

const nlohmann::json& require_document(const nlohmann::json& params) {
    if (!params.contains("document")) {
        throw std::invalid_argument("The request was not valid.");
    }
    const nlohmann::json& document = params.at("document");
    if (!document.is_object()) {
        throw std::invalid_argument("Note content must be a JSON object.");
    }
    return document;
}

} // namespace

std::optional<nlohmann::json> dispatch_notes_operation(
    app::Application& application,
    Method method,
    const nlohmann::json& params,
    std::uint64_t epoch) {
    switch (method) {
    case Method::notes_list:
        require_only_keys(params, {});
        return nlohmann::json{{"items", application.list_note_items(epoch)}};
    case Method::notes_create:
        require_only_keys(params, {"parent_id", "kind", "name"});
        return nlohmann::json(application.create_note_item(
            require_parent_id(params),
            require_kind(params),
            require_string(params, "name"),
            epoch));
    case Method::notes_import_book: {
        require_only_keys(params, {"parent_id", "name", "notes"});
        if (!params.contains("notes") || !params.at("notes").is_array()) {
            throw std::invalid_argument("The request was not valid.");
        }
        std::vector<NoteImportEntry> notes;
        for (const auto& note : params.at("notes")) {
            require_only_keys(note, {"name", "document"});
            notes.push_back({require_string(note, "name"), require_document(note)});
        }
        return nlohmann::json{{"id", application.import_note_book(
            require_parent_id(params), require_string(params, "name"), notes, epoch)}};
    }
    case Method::notes_get:
        require_only_keys(params, {"id"});
        return nlohmann::json(application.get_note(require_note_id(params), epoch));
    case Method::notes_read_text_file: {
        require_only_keys(params, {"path"});
        const auto name = require_string(params, "path");
        const auto path = path_from_utf8(name);
        if (name.find_first_of("\r\n") != std::string::npos || name.find('\0') != std::string::npos
            || !path.is_absolute()) {
            throw std::invalid_argument("Use a full file path.");
        }
        const auto text = read_text_file(path);
        if (!text) return nlohmann::json(nullptr);
        validate_text_file(*text, "This file is too large to load into a note. The limit is 256 KiB of text.");
        return nlohmann::json{{"name", name}, {"text", *text}};
    }
    case Method::notes_save:
        require_only_keys(params, {"id", "document"});
        application.save_note(
            require_note_id(params), require_document(params), epoch);
        return nlohmann::json::object();
    case Method::notes_save_scroll_position: {
        require_only_keys(params, {"id", "scroll_top"});
        const auto top = params.contains("scroll_top") ? as_safe_uint(params.at("scroll_top")) : std::nullopt;
        if (!top) throw std::invalid_argument("The scroll position is not valid.");
        application.save_note_scroll_position(
            require_note_id(params), static_cast<std::int64_t>(*top), epoch);
        return nlohmann::json::object();
    }
    case Method::notes_set_read_only:
        require_only_keys(params, {"id", "read_only"});
        if (!params.contains("read_only") || !params.at("read_only").is_boolean()) {
            throw std::invalid_argument("The read-only state must be a boolean.");
        }
        return nlohmann::json(application.set_note_read_only(
            require_note_id(params), params.at("read_only").get<bool>(), epoch));
    case Method::notes_rename:
        require_only_keys(params, {"id", "name"});
        return nlohmann::json(application.rename_note_item(
            require_note_id(params), require_string(params, "name"), epoch));
    case Method::notes_delete:
        require_only_keys(params, {"id"});
        application.delete_note_item(require_note_id(params), epoch);
        return nlohmann::json::object();
    default:
        return std::nullopt;
    }
}

} // namespace cha::bridge
