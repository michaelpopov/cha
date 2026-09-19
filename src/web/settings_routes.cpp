#include "web/settings_routes.h"
#include "web/fish_audio.h"

#include "app/application.h"
#include "app/settings_operations.h"
#include "providers/api_key_store.h"
#include "providers/openai_oauth.h"
#include "web/http_response.h"
#include "web/json.h"
#include "web/protocol.h"
#include "web/route_support.h"
#include "workspace/workspace_config_store.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <optional>
#include <string>
#include <utility>

namespace cha::web {
namespace {

using Json = nlohmann::json;

void set_application_error(
    httplib::Response& response,
    const cha::app::ApplicationError& error) {
    switch (error.code) {
    case ErrorCode::not_found:
        return set_route_not_found(response, error.what());
    case ErrorCode::invalid_argument: {
        const std::string_view message = error.what();
        const int status =
            message.find("still used") != std::string_view::npos
                || message.find("still in use") != std::string_view::npos
                ? 409 : 400;
        return set_error_response(
            response, status, {ErrorCode::bad_request, std::string(message)});
    }
    case ErrorCode::application_unavailable:
        return set_error_response(
            response, 500, {ErrorCode::internal_error, error.what()});
    default:
        return set_error_response(
            response, 500,
            {ErrorCode::internal_error, "The request could not be completed."});
    }
}

void set_empty_success(httplib::Response& response) {
    response.status = 204;
    response.set_header("Cache-Control", "no-store");
}

void install_provider_routes(
    httplib::Server& server,
    LiveSessionManager* live_sessions,
    WorkspaceConfigStore* config,
    ApiKeyStore* api_keys,
    OpenAiOAuth* openai_auth,
    WebSettings settings) {
    server.Get("/api/v1/providers", [](const httplib::Request&, httplib::Response& response) {
        set_json_response(response, 200, cha::app::settings::list_providers());
    });

    server.Post("/api/v1/providers", [api_keys, config, settings](const httplib::Request& request, httplib::Response& response) {
        if (!validate_json_mutation(request, response)) return;
        CreateProviderRequest create;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&](const Json& json) {
                    create = parse_create_provider_request(json);
                })) {
            return;
        }
        try {
            set_json_response(
                response, 201,
                cha::app::settings::create_provider(*config, *api_keys, create));
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    server.Get(R"(/api/v1/providers/([^/]+))", [api_keys](const httplib::Request& request, httplib::Response& response) {
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) {
            return set_route_not_found(response, "That provider was not found.");
        }
        try {
            set_json_response(
                response, 200, cha::app::settings::get_provider(id, *api_keys));
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    server.Post(R"(/api/v1/providers/([^/]+)/test)", [api_keys, openai_auth, settings](const httplib::Request& request, httplib::Response& response) {
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) {
            return set_route_not_found(response, "That provider was not found.");
        }
        if (!validate_json_mutation(request, response)) return;
        Json body;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&](const Json& json) { body = json; })) {
            return;
        }
        try {
            const std::atomic_bool cancellation{false};
            cha::app::settings::test_provider(
                id, body, *openai_auth, *api_keys, cancellation);
            set_empty_success(response);
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    server.Patch(R"(/api/v1/providers/([^/]+))", [api_keys, live_sessions, config, settings](const httplib::Request& request, httplib::Response& response) {
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) {
            return set_route_not_found(response, "That provider was not found.");
        }
        if (!validate_json_mutation(request, response)) return;
        Json body;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&](const Json& json) { body = json; })) {
            return;
        }
        try {
            set_json_response(
                response, 200,
                cha::app::settings::update_provider(
                    *config, *live_sessions, *api_keys, id, body));
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    server.Delete(R"(/api/v1/providers/([^/]+))", [config](const httplib::Request& request, httplib::Response& response) {
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) {
            return set_route_not_found(response, "That provider was not found.");
        }
        if (!validate_json_mutation(request, response)) return;
        try {
            cha::app::settings::delete_provider(*config, id);
            set_empty_success(response);
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });
}

void install_appearance_voice_routes(
    httplib::Server& server,
    LiveSessionManager* live_sessions,
    WorkspaceConfigStore* config,
    ApiKeyStore* api_keys,
    WebSettings settings,
    bool voice_enabled) {
    server.Get("/api/v1/styles", [](const httplib::Request&, httplib::Response& response) {
        set_json_response(response, 200, cha::app::settings::list_styles());
    });

    server.Post("/api/v1/styles", [config, settings](const httplib::Request& request, httplib::Response& response) {
        if (!validate_json_mutation(request, response)) return;
        std::string display_name;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&](const Json& json) {
                    display_name = parse_create_display_name(json);
                })) {
            return;
        }
        try {
            set_json_response(
                response, 201,
                cha::app::settings::create_style(*config, display_name));
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    server.Patch(R"(/api/v1/styles/([^/]+))", [live_sessions, config, settings](const httplib::Request& request, httplib::Response& response) {
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) {
            return set_route_not_found(response, "That style was not found.");
        }
        if (!validate_json_mutation(request, response)) return;
        StyleUpdate update;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&](const Json& json) { update = parse_style_update(json); })) {
            return;
        }
        try {
            set_json_response(
                response, 200,
                cha::app::settings::update_style(
                    *config, *live_sessions, id, update));
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    server.Delete(R"(/api/v1/styles/([^/]+))", [config](const httplib::Request& request, httplib::Response& response) {
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) {
            return set_route_not_found(response, "That style was not found.");
        }
        if (!validate_json_mutation(request, response)) return;
        try {
            cha::app::settings::delete_style(*config, id);
            set_empty_success(response);
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    server.Get("/api/v1/voices", [](const httplib::Request&, httplib::Response& response) {
        set_json_response(response, 200, cha::app::settings::list_voices());
    });

    server.Post("/api/v1/voices", [config, settings](const httplib::Request& request, httplib::Response& response) {
        if (!validate_json_mutation(request, response)) return;
        CreateVoiceRequest create;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&](const Json& json) {
                    create = parse_create_voice_request(json);
                })) {
            return;
        }
        try {
            set_json_response(
                response, 201,
                cha::app::settings::create_voice(*config, create));
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    server.Patch(R"(/api/v1/voices/([^/]+))", [live_sessions, config, settings](const httplib::Request& request, httplib::Response& response) {
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) {
            return set_route_not_found(response, "That voice was not found.");
        }
        if (!validate_json_mutation(request, response)) return;
        VoiceUpdate update;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&](const Json& json) { update = parse_voice_update(json); })) {
            return;
        }
        try {
            set_json_response(
                response, 200,
                cha::app::settings::update_voice(
                    *config, *live_sessions, id, update));
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    server.Delete(R"(/api/v1/voices/([^/]+))", [config](const httplib::Request& request, httplib::Response& response) {
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) {
            return set_route_not_found(response, "That voice was not found.");
        }
        if (!validate_json_mutation(request, response)) return;
        try {
            cha::app::settings::delete_voice(*config, id);
            set_empty_success(response);
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    server.Get(
        "/api/v1/voice-input",
        [](const httplib::Request&, httplib::Response& response) {
            const auto settings = cha::app::settings::get_voice_input_settings();
            set_json_response(
                response, 200, settings ? Json(*settings) : Json(nullptr));
            response.set_header("Cache-Control", "no-store");
        });

    server.Put(
        "/api/v1/voice-input",
        [api_keys, config, settings](
            const httplib::Request& request,
            httplib::Response& response) {
            if (!validate_json_mutation(request, response)) return;
            VoiceInputSettings update;
            if (!parse_route_json_body(
                    request, response, settings.request_body_limit,
                    [&](const Json& json) {
                        update = parse_voice_input_settings(json);
                    })) return;
            try {
                set_json_response(
                    response, 200,
                    cha::app::settings::save_voice_input_settings(
                        *config, *api_keys, update));
                response.set_header("Cache-Control", "no-store");
            } catch (const cha::app::ApplicationError& error) {
                set_application_error(response, error);
            }
        });

    server.Get(
        "/api/v1/voice-input/runtime",
        [api_keys, voice_enabled](
            const httplib::Request&, httplib::Response& response) {
            const auto runtime = cha::app::settings::get_voice_input_runtime(
                *api_keys, voice_enabled);
            if (!runtime) {
                set_json_response(response, 200, Json(nullptr));
            } else {
                Json result = *runtime;
                result["api_key"] = *cha::app::settings::voice_input_secret(
                    *api_keys, voice_enabled);
                set_json_response(response, 200, result);
            }
            response.set_header("Cache-Control", "no-store");
        });

    server.Get(
        "/api/v1/voice-output",
        [](const httplib::Request&, httplib::Response& response) {
            const auto settings = cha::app::settings::get_voice_output_settings();
            set_json_response(
                response, 200, settings ? Json(*settings) : Json(nullptr));
            response.set_header("Cache-Control", "no-store");
        });

    server.Put(
        "/api/v1/voice-output",
        [api_keys, config, settings](
            const httplib::Request& request,
            httplib::Response& response) {
            if (!validate_json_mutation(request, response)) return;
            VoiceOutputSettings update;
            if (!parse_route_json_body(
                    request, response, settings.request_body_limit,
                    [&](const Json& json) {
                        update = parse_voice_output_settings(json);
                    })) return;
            try {
                set_json_response(
                    response, 200,
                    cha::app::settings::save_voice_output_settings(
                        *config, *api_keys, update));
                response.set_header("Cache-Control", "no-store");
            } catch (const cha::app::ApplicationError& error) {
                set_application_error(response, error);
            }
        });

    server.Get(
        "/api/v1/voice-output/runtime",
        [api_keys, voice_enabled](
            const httplib::Request&, httplib::Response& response) {
            const auto runtime = cha::app::settings::get_voice_output_runtime(
                *api_keys, voice_enabled);
            set_json_response(
                response, 200, runtime ? Json(*runtime) : Json(nullptr));
            response.set_header("Cache-Control", "no-store");
        });
}

void install_credential_routes(
    httplib::Server& server,
    ApiKeyStore* api_keys,
    WebSettings settings) {
    server.Get(
        "/api/v1/api-keys",
        [api_keys](
            const httplib::Request&, httplib::Response& response) {
            set_json_response(
                response, 200, cha::app::settings::list_api_keys(*api_keys));
        });

    server.Get(
        "/api/v1/r2-storage",
        [api_keys](const httplib::Request&, httplib::Response& response) {
            const auto key = cha::app::settings::get_r2_storage(*api_keys);
            set_json_response(
                response, 200, key ? Json(*key) : Json(nullptr));
            response.set_header("Cache-Control", "no-store");
        });

    server.Put(
        "/api/v1/r2-storage",
        [api_keys, settings](
            const httplib::Request& request,
            httplib::Response& response) {
            if (!validate_json_mutation(request, response)) return;
            SaveR2StorageRequest save;
            if (!parse_route_json_body(
                    request, response, settings.request_body_limit,
                    [&](const Json& json) {
                        save = parse_save_r2_storage_request(json);
                    })) return;
            try {
                set_json_response(
                    response, 200,
                    cha::app::settings::save_r2_storage(*api_keys, save));
                response.set_header("Cache-Control", "no-store");
            } catch (const cha::app::ApplicationError& error) {
                set_application_error(response, error);
            }
        });

    server.Delete(
        "/api/v1/r2-storage",
        [api_keys](const httplib::Request& request, httplib::Response& response) {
            if (!validate_json_mutation(request, response)) return;
            try {
                cha::app::settings::delete_r2_storage(*api_keys);
                set_empty_success(response);
            } catch (const cha::app::ApplicationError& error) {
                set_application_error(response, error);
            }
        });

    server.Post("/api/v1/api-keys", [api_keys, settings](const httplib::Request& request, httplib::Response& response) {
        if (!validate_json_mutation(request, response)) return;
        CreateApiKeyRequest create;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&](const Json& json) {
                    create = parse_create_api_key_request(json);
                })) return;
        try {
            set_json_response(
                response, 201,
                cha::app::settings::create_api_key(*api_keys, create));
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    server.Patch(R"(/api/v1/api-keys/([^/]+))", [api_keys, settings](const httplib::Request& request, httplib::Response& response) {
        if (!validate_json_mutation(request, response)) return;
        const std::string id = request.matches[1];
        std::string display_name;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&](const Json& json) {
                    display_name = parse_rename_display_name(json);
                })) return;
        try {
            set_json_response(
                response, 200,
                cha::app::settings::rename_api_key(*api_keys, id, display_name));
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    server.Put(R"(/api/v1/api-keys/([^/]+)/value)", [api_keys, settings](const httplib::Request& request, httplib::Response& response) {
        if (!validate_json_mutation(request, response)) return;
        const std::string id = request.matches[1];
        std::string value;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&](const Json& json) {
                    value = parse_replace_secret_value(json);
                })) return;
        try {
            set_json_response(
                response, 200,
                cha::app::settings::replace_api_key_value(*api_keys, id, value));
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    server.Delete(R"(/api/v1/api-keys/([^/]+))", [api_keys](const httplib::Request& request, httplib::Response& response) {
        if (!validate_json_mutation(request, response)) return;
        const std::string id = request.matches[1];
        try {
            cha::app::settings::delete_api_key(*api_keys, id);
            set_empty_success(response);
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });
}

} // namespace

SettingsRoutes::SettingsRoutes(
    LiveSessionManager& live_sessions,
    WebSettings settings,
    WorkspaceConfigStore& config,
    ApiKeyStore& api_keys,
    OpenAiOAuth& openai_auth,
    bool voice_enabled,
    FishAudioProxy& fish_audio)
    : live_sessions_(&live_sessions),
      settings_(std::move(settings)),
      config_(&config),
      api_keys_(&api_keys),
      openai_auth_(&openai_auth),
      voice_enabled_(voice_enabled),
      fish_audio_(&fish_audio) {}

void SettingsRoutes::install(httplib::Server& server) const {
    install_fish_audio_route(server, *api_keys_, settings_, voice_enabled_, *fish_audio_);
    install_provider_routes(
        server, live_sessions_, config_, api_keys_, openai_auth_, settings_);
    install_appearance_voice_routes(
        server, live_sessions_, config_, api_keys_, settings_, voice_enabled_);
    install_credential_routes(server, api_keys_, settings_);
}

} // namespace cha::web
