#include "bridge/operation_dispatch.h"

#include "app/application.h"
#include "bridge/request_params.h"
#include "storage/note_storage.h"

#include <stdexcept>

namespace cha::bridge {
namespace {

std::int64_t require_note_id(const nlohmann::json& params) {
    if (!params.contains("id")) {
        throw std::invalid_argument("The request was not valid.");
    }
    const auto id = as_safe_uint(params.at("id"));
    if (!id || *id < 1) {
        throw std::invalid_argument("The note id is not valid.");
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

nlohmann::json require_document(const nlohmann::json& params) {
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
    case Method::notes_get:
        require_only_keys(params, {"id"});
        return nlohmann::json(application.get_note(require_note_id(params), epoch));
    case Method::notes_save:
        require_only_keys(params, {"id", "document"});
        application.save_note(
            require_note_id(params), require_document(params), epoch);
        return nlohmann::json::object();
    case Method::notes_rename:
        require_only_keys(params, {"id", "name"});
        return nlohmann::json(application.rename_note_item(
            require_note_id(params), require_string(params, "name"), epoch));
    case Method::notes_move:
        require_only_keys(params, {"id", "parent_id"});
        return nlohmann::json(application.move_note_item(
            require_note_id(params), require_parent_id(params), epoch));
    case Method::notes_delete:
        require_only_keys(params, {"id"});
        application.delete_note_item(require_note_id(params), epoch);
        return nlohmann::json::object();
    default:
        return std::nullopt;
    }
}

} // namespace cha::bridge
