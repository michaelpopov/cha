#include "bridge/operation_dispatch.h"

#include "app/application.h"
#include "bridge/request_params.h"
#include "runtime/request_parser.h"

namespace cha::bridge {
namespace {

std::string require_filename(
    const nlohmann::json& params,
    std::string_view key) {
    const std::string value = require_string(params, key);
    if (value.empty()
        || value.find('/') != std::string::npos
        || value.find('\\') != std::string::npos
        || value.find('\0') != std::string::npos) {
        throw std::invalid_argument("The request was not valid.");
    }
    return value;
}

} // namespace

std::optional<nlohmann::json> dispatch_workspace_operation(
    app::Application& application,
    Method method,
    const nlohmann::json& params,
    std::uint64_t epoch) {
    nlohmann::json result = nlohmann::json::object();
    switch (method) {
    case Method::character_get: {
        require_only_keys(params, {"character_id"});
        result = application.get_character(
            require_identifier(params, "character_id"), epoch);
        break;
    }
    case Method::character_create: {
        result = application.create_character(
            parse_create_character_request(params), epoch);
        break;
    }
    case Method::character_update: {
        const std::string id = require_identifier(params, "character_id");
        result = application.update_character(
            id,
            parse_character_settings_update(
                without_key(params, "character_id")),
            epoch);
        break;
    }
    case Method::character_update_definition: {
        const std::string id = require_identifier(params, "character_id");
        result = application.update_character_definition(
            id,
            parse_character_definition_update(
                without_key(params, "character_id")),
            epoch);
        break;
    }
    case Method::character_delete: {
        require_only_keys(params, {"character_id"});
        application.delete_character(
            require_identifier(params, "character_id"), epoch);
        result = nlohmann::json::object();
        break;
    }
    case Method::character_file_get: {
        require_only_keys(params, {"character_id", "filename"});
        result = application.get_character_file(
            require_identifier(params, "character_id"),
            require_filename(params, "filename"),
            epoch);
        break;
    }
    case Method::character_file_create: {
        require_only_keys(params, {"character_id", "filename", "content"});
        result = application.create_character_file(
            require_identifier(params, "character_id"),
            require_filename(params, "filename"),
            require_string(params, "content"),
            epoch);
        break;
    }
    case Method::character_file_update: {
        require_only_keys(params, {"character_id", "filename", "content"});
        result = application.update_character_file(
            require_identifier(params, "character_id"),
            require_filename(params, "filename"),
            require_string(params, "content"),
            epoch);
        break;
    }
    case Method::character_file_delete: {
        require_only_keys(params, {"character_id", "filename"});
        application.delete_character_file(
            require_identifier(params, "character_id"),
            require_filename(params, "filename"),
            epoch);
        result = nlohmann::json::object();
        break;
    }
    case Method::persona_get: {
        require_only_keys(params, {"persona_id"});
        result = application.get_persona(
            require_identifier(params, "persona_id"), epoch);
        break;
    }
    case Method::persona_create: {
        result = application.create_persona(
            parse_create_persona_name(params), epoch);
        break;
    }
    case Method::persona_update: {
        const std::string id = require_identifier(params, "persona_id");
        result = application.update_persona(
            id,
            parse_persona_update(without_key(params, "persona_id")),
            epoch);
        break;
    }
    case Method::persona_delete: {
        require_only_keys(params, {"persona_id"});
        application.delete_persona(
            require_identifier(params, "persona_id"), epoch);
        result = nlohmann::json::object();
        break;
    }
    case Method::forum_get: {
        require_only_keys(params, {"forum_id"});
        result = application.get_forum(
            require_identifier(params, "forum_id"), epoch);
        break;
    }
    case Method::forum_create: {
        result = application.create_forum(
            parse_create_forum_request(params), epoch);
        break;
    }
    case Method::forum_update: {
        const std::string id = require_identifier(params, "forum_id");
        result = application.update_forum(
            id,
            parse_forum_update(without_key(params, "forum_id")),
            epoch);
        break;
    }
    case Method::forum_delete: {
        require_only_keys(params, {"forum_id"});
        application.delete_forum(
            require_identifier(params, "forum_id"), epoch);
        result = nlohmann::json::object();
        break;
    }
    case Method::forum_members_update: {
        const std::string id = require_identifier(params, "forum_id");
        result = application.update_forum_members(
            id,
            parse_forum_members_update(
                without_key(params, "forum_id")),
            epoch);
        break;
    }
    case Method::forum_file_get: {
        require_only_keys(params, {"forum_id", "filename"});
        result = application.get_forum_file(
            require_identifier(params, "forum_id"),
            require_filename(params, "filename"),
            epoch);
        break;
    }
    case Method::forum_file_create: {
        require_only_keys(params, {"forum_id", "filename", "content"});
        result = application.create_forum_file(
            require_identifier(params, "forum_id"),
            require_filename(params, "filename"),
            require_string(params, "content"),
            epoch);
        break;
    }
    case Method::forum_file_update: {
        require_only_keys(params, {"forum_id", "filename", "content"});
        result = application.update_forum_file(
            require_identifier(params, "forum_id"),
            require_filename(params, "filename"),
            require_string(params, "content"),
            epoch);
        break;
    }
    case Method::forum_file_delete: {
        require_only_keys(params, {"forum_id", "filename"});
        application.delete_forum_file(
            require_identifier(params, "forum_id"),
            require_filename(params, "filename"),
            epoch);
        result = nlohmann::json::object();
        break;
    }
    default:
        return std::nullopt;
    }
    return result;
}

} // namespace cha::bridge
