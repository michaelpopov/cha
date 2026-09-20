#include "bridge/operation_dispatch.h"

#include "app/application.h"
#include "bridge/request_params.h"
#include "web/json.h"

namespace cha::bridge {
namespace {

template<typename T>
nlohmann::json encode_optional(const std::optional<T>& value) {
    if (!value) return nlohmann::json(nullptr);
    return nlohmann::json(*value);
}

} // namespace

std::optional<nlohmann::json> dispatch_settings_operation(
    cha::app::Application& application,
    Method method,
    const nlohmann::json& params,
    std::uint64_t epoch) {
    nlohmann::json result = nlohmann::json::object();
    switch (method) {
    case Method::provider_list:
        require_only_keys(params, {});
        result = application.list_providers(epoch);
        break;
    case Method::provider_get:
        require_only_keys(params, {"provider_id"});
        result = application.get_provider(
            require_identifier(params, "provider_id"), epoch);
        break;
    case Method::provider_create:
        result = application.create_provider(
            cha::web::parse_create_provider_request(params), epoch);
        break;
    case Method::provider_update: {
        const std::string id = require_identifier(params, "provider_id");
        result = application.update_provider(
            id, without_key(params, "provider_id"), epoch);
        break;
    }
    case Method::provider_delete:
        require_only_keys(params, {"provider_id"});
        application.delete_provider(
            require_identifier(params, "provider_id"), epoch);
        result = nlohmann::json::object();
        break;
    case Method::style_list:
        require_only_keys(params, {});
        result = application.list_styles(epoch);
        break;
    case Method::style_create:
        result = application.create_style(
            cha::web::parse_create_display_name(params), epoch);
        break;
    case Method::style_update: {
        const std::string id = require_identifier(params, "style_id");
        result = application.update_style(
            id,
            cha::web::parse_style_update(without_key(params, "style_id")),
            epoch);
        break;
    }
    case Method::style_delete:
        require_only_keys(params, {"style_id"});
        application.delete_style(
            require_identifier(params, "style_id"), epoch);
        result = nlohmann::json::object();
        break;
    case Method::voice_list:
        require_only_keys(params, {});
        result = application.list_voices(epoch);
        break;
    case Method::voice_create:
        result = application.create_voice(
            cha::web::parse_create_voice_request(params), epoch);
        break;
    case Method::voice_update: {
        const std::string id = require_identifier(params, "voice_id");
        result = application.update_voice(
            id,
            cha::web::parse_voice_update(without_key(params, "voice_id")),
            epoch);
        break;
    }
    case Method::voice_delete:
        require_only_keys(params, {"voice_id"});
        application.delete_voice(
            require_identifier(params, "voice_id"), epoch);
        result = nlohmann::json::object();
        break;
    case Method::voice_input_get:
        require_only_keys(params, {});
        result = encode_optional(application.get_voice_input_settings(epoch));
        break;
    case Method::voice_input_save:
        result = application.save_voice_input_settings(
            cha::web::parse_voice_input_settings(params), epoch);
        break;
    case Method::voice_input_runtime:
        require_only_keys(params, {});
        result = encode_optional(application.get_voice_input_runtime(epoch));
        break;
    case Method::voice_output_get:
        require_only_keys(params, {});
        result = encode_optional(
            application.get_voice_output_settings(epoch));
        break;
    case Method::voice_output_save:
        result = application.save_voice_output_settings(
            cha::web::parse_voice_output_settings(params), epoch);
        break;
    case Method::voice_output_runtime:
        require_only_keys(params, {});
        result = encode_optional(
            application.get_voice_output_runtime(epoch));
        break;
    case Method::api_key_list:
        require_only_keys(params, {});
        result = application.list_api_keys(epoch);
        break;
    case Method::api_key_create:
        result = application.create_api_key(
            cha::web::parse_create_api_key_request(params), epoch);
        break;
    case Method::api_key_rename: {
        const std::string id = require_identifier(params, "api_key_id");
        result = application.rename_api_key(
            id,
            cha::web::parse_rename_display_name(
                without_key(params, "api_key_id")),
            epoch);
        break;
    }
    case Method::api_key_replace_value: {
        const std::string id = require_identifier(params, "api_key_id");
        result = application.replace_api_key_value(
            id,
            cha::web::parse_replace_secret_value(
                without_key(params, "api_key_id")),
            epoch);
        break;
    }
    case Method::api_key_delete:
        require_only_keys(params, {"api_key_id"});
        application.delete_api_key(
            require_identifier(params, "api_key_id"), epoch);
        result = nlohmann::json::object();
        break;
    case Method::r2_storage_get:
        require_only_keys(params, {});
        result = encode_optional(application.get_r2_storage(epoch));
        break;
    case Method::r2_storage_save:
        result = application.save_r2_storage(
            cha::web::parse_save_r2_storage_request(params), epoch);
        break;
    case Method::r2_storage_delete:
        require_only_keys(params, {});
        application.delete_r2_storage(epoch);
        result = nlohmann::json::object();
        break;
    case Method::openai_auth_get:
        require_only_keys(params, {});
        result = application.openai_auth_status(epoch);
        break;
    case Method::openai_auth_disconnect:
        require_only_keys(params, {});
        result = application.disconnect_openai_auth(epoch);
        break;
    default:
        return std::nullopt;
    }
    return result;
}

} // namespace cha::bridge
