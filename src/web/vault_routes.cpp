#include "web/vault_routes.h"

#include "util/path_name.h"
#include "web/application_config.h"
#include "web/application_runtime.h"
#include "web/http_response.h"
#include "web/json.h"
#include "web/protocol.h"
#include "web/route_support.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace cha::web {
namespace {

std::optional<std::string> nullable_json_string(
    const nlohmann::json& json,
    std::string_view key) {
    const std::string name(key);
    if (!json.is_object() || !json.contains(name)) {
        throw std::invalid_argument("Invalid vault settings");
    }
    if (json.at(name).is_null()) return std::nullopt;
    if (!json.at(name).is_string()) {
        throw std::invalid_argument("Invalid vault settings");
    }
    const std::string value = json.at(name).get<std::string>();
    if (value.empty()) throw std::invalid_argument("Invalid vault settings");
    return value;
}

nlohmann::json vault_json(
    const VaultDefinition& vault,
    std::string_view active_name,
    std::size_t vault_count) {
    return {
        {"display_name", vault.name},
        {"protected", vault.password_protected},
        {"data_path", utf8_path(vault.data)},
        {"mirror_path", vault.mirror
            ? nlohmann::json(utf8_path(*vault.mirror)) : nlohmann::json(nullptr)},
        {"modify_path", vault.modify
            ? nlohmann::json(utf8_path(*vault.modify)) : nlohmann::json(nullptr)},
        {"active", same_vault_name(vault.name, active_name)},
        {"can_delete", vault_count > 1
            && !same_vault_name(vault.name, active_name)},
    };
}

} // namespace

void install_vault_routes(
    httplib::Server& server,
    ApplicationRuntime* runtime,
    WebSettings settings) {
    server.Get(
        "/api/v1/vaults",
        [runtime](const httplib::Request&, httplib::Response& response) {
            const VaultRegistrySnapshot snapshot = runtime->vault_snapshot();
            nlohmann::json result = nlohmann::json::array();
            for (const VaultDefinition& vault : snapshot.vaults) {
                result.push_back(vault_json(
                    vault, snapshot.active.name, snapshot.vaults.size()));
            }
            set_json_response(response, 200, result);
        });
    server.Get(
        "/api/v1/r2-vaults",
        [runtime](const httplib::Request&, httplib::Response& response) {
            try {
                set_json_response(response, 200, runtime->list_r2_vaults());
            } catch (const std::invalid_argument& error) {
                set_error_response(
                    response, 400, {ErrorCode::bad_request, error.what()});
            } catch (const std::exception& error) {
                set_error_response(
                    response, 500, {ErrorCode::internal_error, error.what()});
            }
        });
    server.Post(
        "/api/v1/r2-vaults",
        [runtime, settings](
            const httplib::Request& request, httplib::Response& response) {
            if (!validate_json_mutation(request, response)) return;
            std::string name;
            if (!parse_route_json_body(
                    request, response, settings.request_body_limit,
                    [&name](const nlohmann::json& json) {
                        if (!json.is_object() || json.size() != 1) {
                            throw std::invalid_argument("Invalid R2 vault");
                        }
                        name = required_string(json, "name");
                    })) return;
            try {
                const VaultDefinition created =
                    runtime->download_r2_vault(name);
                const VaultRegistrySnapshot snapshot =
                    runtime->vault_snapshot();
                set_json_response(
                    response, 201,
                    vault_json(
                        created, snapshot.active.name, snapshot.vaults.size()));
            } catch (const std::invalid_argument& error) {
                set_error_response(
                    response, 400, {ErrorCode::bad_request, error.what()});
            } catch (const std::exception& error) {
                set_error_response(
                    response, 500, {ErrorCode::internal_error, error.what()});
            }
        });
    server.Post(
        "/api/v1/vaults",
        [runtime, settings](
            const httplib::Request& request, httplib::Response& response) {
            if (!validate_json_mutation(request, response)) return;
            VaultCreate create;
            if (!parse_route_json_body(
                    request, response, settings.request_body_limit,
                    [&create](const nlohmann::json& json) {
                        if (!json.is_object() || json.size() != 3) {
                            throw std::invalid_argument("Invalid vault settings");
                        }
                        create.display_name =
                            required_string(json, "display_name");
                        create.copy_from =
                            nullable_json_string(json, "copy_from");
                        create.password =
                            nullable_json_string(json, "password").value_or("");
                    })) return;
            try {
                const VaultDefinition created =
                    runtime->create_vault(std::move(create));
                const VaultRegistrySnapshot snapshot =
                    runtime->vault_snapshot();
                set_json_response(
                    response, 201,
                    vault_json(
                        created, snapshot.active.name, snapshot.vaults.size()));
            } catch (const std::invalid_argument& error) {
                set_error_response(
                    response, 400, {ErrorCode::bad_request, error.what()});
            } catch (const std::exception& error) {
                set_error_response(
                    response, 500, {ErrorCode::internal_error, error.what()});
            }
        });
    server.Patch(
        "/api/v1/vaults",
        [runtime, settings](
            const httplib::Request& request, httplib::Response& response) {
            if (!validate_json_mutation(request, response)) return;
            std::string name;
            VaultUpdate update;
            if (!parse_route_json_body(
                    request, response, settings.request_body_limit,
                    [&name, &update](const nlohmann::json& json) {
                        if (!json.is_object() || json.size() != 3) {
                            throw std::invalid_argument("Invalid vault settings");
                        }
                        name = required_string(json, "vault_name");
                        update.display_name =
                            required_string(json, "display_name");
                        update.password =
                            nullable_json_string(json, "password").value_or("");
                    })) return;
            try {
                const VaultDefinition updated =
                    runtime->update_vault(name, std::move(update));
                const VaultRegistrySnapshot snapshot =
                    runtime->vault_snapshot();
                set_json_response(
                    response, 200,
                    vault_json(
                        updated, snapshot.active.name, snapshot.vaults.size()));
            } catch (const std::out_of_range&) {
                set_route_not_found(response, "That vault was not found.");
            } catch (const std::invalid_argument& error) {
                set_error_response(
                    response, 400, {ErrorCode::bad_request, error.what()});
            } catch (const std::exception& error) {
                set_error_response(
                    response, 500, {ErrorCode::internal_error, error.what()});
            }
        });
    server.Delete(
        "/api/v1/vaults",
        [runtime, settings](
            const httplib::Request& request, httplib::Response& response) {
            if (!validate_json_mutation(request, response)) return;
            std::string name;
            if (!parse_route_json_body(
                    request, response, settings.request_body_limit,
                    [&name](const nlohmann::json& json) {
                        if (!json.is_object() || json.size() != 1) {
                            throw std::invalid_argument("Invalid vault settings");
                        }
                        name = required_string(json, "vault_name");
                    })) return;
            try {
                runtime->delete_vault(name);
                response.status = 204;
                response.set_header("Cache-Control", "no-store");
            } catch (const std::out_of_range&) {
                set_route_not_found(response, "That vault was not found.");
            } catch (const std::invalid_argument& error) {
                set_error_response(
                    response, 409, {ErrorCode::bad_request, error.what()});
            } catch (const std::exception& error) {
                set_error_response(
                    response, 500, {ErrorCode::internal_error, error.what()});
            }
        });
    server.Post(
        "/api/v1/vault/switch",
        [runtime, settings](
            const httplib::Request& request, httplib::Response& response) {
            if (!validate_json_mutation(request, response)) return;
            std::string vault_name;
            std::string password;
            if (!parse_route_json_body(
                    request,
                    response,
                    settings.request_body_limit,
                    [&vault_name, &password](const nlohmann::json& json) {
                        if (!json.is_object() || json.size() != 2) {
                            throw std::invalid_argument("Invalid vault selection");
                        }
                        vault_name = required_string(json, "vault_name");
                        password = nullable_json_string(json, "password").value_or("");
                    })) {
                return;
            }
            try {
                runtime->switch_vault(vault_name, std::move(password));
            } catch (const UnknownVaultError& error) {
                return set_error_response(
                    response, 400, {ErrorCode::bad_request, error.what()});
            } catch (const VaultPasswordError& error) {
                return set_error_response(
                    response,
                    401,
                    {ErrorCode::vault_password_required, error.what()});
            } catch (const std::exception& error) {
                return set_error_response(
                    response, 500, {ErrorCode::internal_error, error.what()});
            }
            response.status = 204;
            response.set_header("Cache-Control", "no-store");
        });
    server.Post(
        "/api/v1/vault/merge",
        [runtime, settings](
            const httplib::Request& request, httplib::Response& response) {
            if (!validate_json_mutation(request, response)) return;
            std::string source_vault;
            std::string password;
            if (!parse_route_json_body(
                    request,
                    response,
                    settings.request_body_limit,
                    [&source_vault, &password](const nlohmann::json& json) {
                        if (!json.is_object() || json.size() != 2) {
                            throw std::invalid_argument("Invalid vault merge");
                        }
                        source_vault =
                            required_string(json, "source_vault");
                        if (source_vault.empty()) {
                            throw std::invalid_argument("Invalid vault merge");
                        }
                        password = nullable_json_string(json, "password")
                                       .value_or("");
                    })) {
                return;
            }
            try {
                runtime->merge_vault(source_vault, std::move(password));
            } catch (const UnknownVaultError& error) {
                return set_error_response(
                    response, 400, {ErrorCode::bad_request, error.what()});
            } catch (const std::invalid_argument& error) {
                return set_error_response(
                    response, 400, {ErrorCode::bad_request, error.what()});
            } catch (const VaultPasswordError& error) {
                return set_error_response(
                    response,
                    401,
                    {ErrorCode::source_vault_password_required, error.what()});
            } catch (const std::exception& error) {
                return set_error_response(
                    response, 500, {ErrorCode::internal_error, error.what()});
            }
            response.status = 204;
            response.set_header("Cache-Control", "no-store");
        });
}

} // namespace cha::web
