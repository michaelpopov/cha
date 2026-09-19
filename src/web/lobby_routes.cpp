#include "web/lobby_routes.h"
#include "web/audio_download.h"

#include "app/application.h"
#include "app/workspace_operations.h"
#include "workspace/workspace.h"
#include "workspace/workspace_config_store.h"
#include "session/not_found_error.h"
#include "session/session_label.h"
#include "session/session_repository.h"
#include "web/current_vault.h"
#include "web/http_response.h"
#include "web/json.h"
#include "web/protocol.h"
#include "web/route_support.h"
#include "web/session_markdown.h"
#include "web/session_mirror.h"
#include "web/session_projection.h"
#include "web/live_session_manager.h"
#include "util/text.h"
#include "util/logging.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cha::web {
namespace {

std::string session_event(
    const FullSessionId& identity,
    std::string_view event) {
    return "web session forum_id=" + identity.forum_id
        + " session_id=" + identity.session_id
        + " event=" + std::string(event);
}

bool validate_route_session_label(
    httplib::Response& response,
    std::string_view label,
    bool allow_empty) {
    if (allow_empty && label.empty()) return true;
    try {
        validate_session_label(label);
        return true;
    } catch (const std::invalid_argument&) {
        set_error_response(response, 400,
            {ErrorCode::bad_request, "Invalid session label."});
        return false;
    }
}

void set_open_result(
    httplib::Response& response,
    const FullSessionId& identity,
    const LiveSessionOpenResult& result) {
    if (std::holds_alternative<LiveSessionReady>(result)) {
        set_json_response(response, 200, nlohmann::json(OpenSessionSuccess{
            identity.forum_id, identity.session_id}));
        return;
    }
    switch (std::get<LiveSessionOpenFailure>(result)) {
    case LiveSessionOpenFailure::not_found:
        set_route_not_found(response);
        return;
    case LiveSessionOpenFailure::stopping:
        set_error_response(response, 409, {ErrorCode::session_stopping, "Session is stopping."});
        return;
    case LiveSessionOpenFailure::limit_reached:
        set_error_response(response, 503, {ErrorCode::session_limit_reached, "Session limit reached."});
        return;
    case LiveSessionOpenFailure::open_timeout:
        set_error_response(response, 503, {ErrorCode::session_open_timeout, "Session is still opening."});
        return;
    case LiveSessionOpenFailure::manager_stopping:
        set_error_response(response, 503, {ErrorCode::server_stopping, "Server is stopping."});
        return;
    case LiveSessionOpenFailure::internal_error:
        set_error_response(response, 500, {ErrorCode::internal_error, "Session could not be opened."});
        return;
    }
}

void set_command_error(httplib::Response& response, ErrorCode code) {
    switch (code) {
    case ErrorCode::command_queue_full:
        return set_error_response(response, 503, {code, "Session command queue is full."});
    case ErrorCode::command_timeout:
        return set_error_response(response, 503, {code, "Session command timed out."});
    case ErrorCode::server_stopping:
        return set_error_response(response, 503, {code, "Server is stopping."});
    case ErrorCode::session_not_live:
        return set_error_response(response, 409, {code, "That session is not open."});
    default:
        return set_error_response(response, 500,
            {ErrorCode::internal_error, "The request could not be completed."});
    }
}

void set_application_error(
    httplib::Response& response,
    const cha::app::ApplicationError& error) {
    switch (error.code) {
    case ErrorCode::not_found:
        return set_route_not_found(response, error.what());
    case ErrorCode::invalid_argument: {
        const std::string_view message = error.what();
        const int status =
            message.find("still used") != std::string_view::npos ? 409 : 400;
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

std::shared_ptr<const Workspace> loaded_workspace() {
    std::shared_ptr<const Workspace> workspace = getws();
    if (!workspace) throw std::runtime_error("Workspace is not loaded");
    return workspace;
}

using cha::app::workspace::sessions_for;

Bootstrap bootstrap_for(
    const Workspace& workspace,
    const std::vector<StoredSession>& recent,
    const InitialSelection& initial,
    std::string vault_name,
    std::vector<std::string> vaults) {
    Bootstrap bootstrap{.initial_forum_id = initial.session.forum_id,
                        .initial_session_id = initial.session.session_id};
    for (const WorkspacePersona& persona : workspace.personas()) {
        bootstrap.personas.push_back(
            cha::app::workspace::persona_summary(workspace, persona));
    }
    for (const WorkspaceCharacter& character : workspace.characters()) {
        bootstrap.characters.push_back(
            cha::app::workspace::character_summary(workspace, character));
    }
    for (const WorkspaceForum& forum : workspace.forums()) {
        bootstrap.forums.push_back(
            cha::app::workspace::forum_summary(forum, workspace));
    }
    bootstrap.recent_sessions.reserve(recent.size());
    for (const StoredSession& stored : recent) {
        bootstrap.recent_sessions.push_back({
            stored.identity.forum_id,
            stored.identity.session_id,
            stored.label,
            stored.updated_at});
    }
    bootstrap.vault_name = std::move(vault_name);
    bootstrap.vaults = std::move(vaults);
    return bootstrap;
}

} // namespace

LobbyRoutes::LobbyRoutes(
    std::shared_ptr<const SessionRepository> sessions,
    InitialSelection initial,
    LiveSessionManager& live_sessions,
    WebSettings settings,
    WorkspaceConfigStore& config,
    CurrentVault& current_vault,
    std::vector<std::string> vault_names,
    std::shared_ptr<SessionMirror> mirror,
    std::function<void(const FullSessionId&)> clear_audio)
    : sessions_(std::move(sessions)),
      initial_(std::move(initial)), live_sessions_(live_sessions),
      settings_(std::move(settings)),
      config_(&config),
      current_vault_(&current_vault),
      vault_names_(std::move(vault_names)),
      mirror_(std::move(mirror)), clear_audio_(std::move(clear_audio)) {
    if (!sessions_) throw std::invalid_argument("Lobby routes need a session repository");
}

void LobbyRoutes::install(httplib::Server& server) const {
    const auto sessions = sessions_;
    const auto clear_audio = clear_audio_;
    const InitialSelection initial = initial_;
    LiveSessionManager* const live_sessions = &live_sessions_;
    const WebSettings settings = settings_;
    WorkspaceConfigStore* const config = config_;
    CurrentVault* const current_vault = current_vault_;
    const std::vector<std::string> vault_names = vault_names_;
    const std::shared_ptr<SessionMirror> mirror = mirror_;
    server.Get("/health", [live_sessions](const httplib::Request&, httplib::Response& response) {
        const LiveSessionManagerSnapshot snapshot = live_sessions->snapshot();
        set_json_response(response, 200, nlohmann::json{
            {"ready", true}, {"live_session_count", snapshot.live_session_count}});
    });

    server.Get("/api/v1/bootstrap", [sessions, initial, current_vault, vault_names](
                                        const httplib::Request&, httplib::Response& response) {
        const std::shared_ptr<const Workspace> current = loaded_workspace();
        const std::vector<StoredSession> recent = sessions->recent();
        auto [vault, current_names] = current_vault->snapshot();
        if (current_names.empty()) current_names = vault_names;
        set_json_response(response, 200, nlohmann::json(
            bootstrap_for(
                *current, recent, initial,
                std::move(vault.name), std::move(current_names))));
    });

    server.Post("/api/v1/personas",
        [settings, config](const httplib::Request& request,
                           httplib::Response& response) {
        if (!validate_json_mutation(request, response)) return;
        std::string display_name;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&display_name](const nlohmann::json& json) {
                    display_name = parse_create_persona_name(json);
                })) return;
        try {
            set_json_response(
                response, 201,
                nlohmann::json(cha::app::workspace::create_persona(
                    *config, display_name)));
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    server.Post("/api/v1/characters",
        [settings, config](const httplib::Request& request,
                           httplib::Response& response) {
        if (!validate_json_mutation(request, response)) return;
        CreateCharacterRequest create;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&create](const nlohmann::json& json) {
                    create = parse_create_character_request(json);
                })) return;
        try {
            set_json_response(
                response, 201,
                nlohmann::json(cha::app::workspace::create_character(
                    *config, create.display_name, create.description)));
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    server.Post("/api/v1/forums",
        [settings, config](const httplib::Request& request,
                           httplib::Response& response) {
        if (!validate_json_mutation(request, response)) return;
        CreateForumRequest create;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&create](const nlohmann::json& json) {
                    create = parse_create_forum_request(json);
                })) return;
        try {
            set_json_response(
                response, 201,
                nlohmann::json(cha::app::workspace::create_forum(
                    *config, create.display_name, create.persona_id)));
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    server.Get(R"(/api/v1/characters/([^/]+))", [](const httplib::Request& request, httplib::Response& response) {
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) {
            return set_route_not_found(response, "That character was not found.");
        }
        try {
            set_json_response(
                response, 200,
                nlohmann::json(cha::app::workspace::get_character(id)));
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    server.Delete(R"(/api/v1/characters/([^/]+))",
        [settings, config](const httplib::Request& request,
                           httplib::Response& response) {
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) {
            return set_route_not_found(response, "That character was not found.");
        }
        if (!validate_json_mutation(request, response)) return;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [](const nlohmann::json& json) { parse_empty_object(json); })) return;
        try {
            cha::app::workspace::delete_character(*config, id);
            response.status = 204;
            response.set_header("Cache-Control", "no-store");
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    server.Patch(R"(/api/v1/characters/([^/]+))",
        [live_sessions, settings, config](const httplib::Request& request,
                                  httplib::Response& response) {
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) {
            return set_route_not_found(response, "That character was not found.");
        }
        if (!validate_json_mutation(request, response)) return;
        CharacterSettingsUpdate update;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&update](const nlohmann::json& json) {
                    update = parse_character_settings_update(json);
                })) return;
        try {
            set_json_response(
                response, 200,
                nlohmann::json(cha::app::workspace::update_character_settings(
                    *config, *live_sessions, id, update)));
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    server.Patch(R"(/api/v1/characters/([^/]+)/definition)",
        [live_sessions, settings, config](const httplib::Request& request,
                                          httplib::Response& response) {
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) {
            return set_route_not_found(response, "That character was not found.");
        }
        if (!validate_json_mutation(request, response)) return;
        CharacterDefinitionUpdate update;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&update](const nlohmann::json& json) {
                    update = parse_character_definition_update(json);
                })) return;
        try {
            set_json_response(
                response, 200,
                nlohmann::json(cha::app::workspace::update_character_definition(
                    *config, *live_sessions, id, update)));
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    server.Get(R"(/api/v1/characters/([^/]+)/files/([^/]+))",
        [](const httplib::Request& request, httplib::Response& response) {
        const std::string id = request.matches[1];
        const std::string filename = request.matches[2];
        if (!is_valid_route_component(id)) {
            return set_route_not_found(response, "That character file was not found.");
        }
        try {
            set_json_response(
                response, 200,
                nlohmann::json(cha::app::workspace::get_character_file(id, filename)));
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    const auto edit_character_file = [live_sessions, settings, config](
        const httplib::Request& request, httplib::Response& response,
        bool create, bool remove) {
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) {
            return set_route_not_found(response, "That character file was not found.");
        }
        if (!validate_json_mutation(request, response)) return;
        std::string filename = create ? std::string() : request.matches[2].str();
        std::optional<std::string> content;
        if (!parse_route_json_body(request, response, settings.request_body_limit,
            [&](const nlohmann::json& json) {
                if (remove) return parse_empty_object(json);
                if (!json.is_object() || json.size() != (create ? 2 : 1)) {
                    throw std::invalid_argument("Invalid character file");
                }
                if (create) filename = required_string(json, "filename");
                content = required_string(json, "content");
            })) return;
        try {
            if (remove) {
                cha::app::workspace::delete_character_file(
                    *config, *live_sessions, id, filename);
                response.status = 204;
                response.set_header("Cache-Control", "no-store");
                return;
            }
            const auto file = create
                ? cha::app::workspace::create_character_file(
                    *config, *live_sessions, id, filename, *content)
                : cha::app::workspace::update_character_file(
                    *config, *live_sessions, id, filename, *content);
            set_json_response(response, create ? 201 : 200, nlohmann::json(file));
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    };
    server.Post(R"(/api/v1/characters/([^/]+)/files)",
        [edit_character_file](const auto& request, auto& response) {
            edit_character_file(request, response, true, false);
        });
    server.Put(R"(/api/v1/characters/([^/]+)/files/([^/]+))",
        [edit_character_file](const auto& request, auto& response) {
            edit_character_file(request, response, false, false);
        });
    server.Delete(R"(/api/v1/characters/([^/]+)/files/([^/]+))",
        [edit_character_file](const auto& request, auto& response) {
            edit_character_file(request, response, false, true);
        });

    server.Get(R"(/api/v1/personas/([^/]+))", [](const httplib::Request& request, httplib::Response& response) {
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) {
            return set_route_not_found(response, "That persona was not found.");
        }
        try {
            set_json_response(
                response, 200,
                nlohmann::json(cha::app::workspace::get_persona(id)));
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    server.Delete(R"(/api/v1/personas/([^/]+))",
        [settings, config](const httplib::Request& request,
                           httplib::Response& response) {
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) {
            return set_route_not_found(response, "That persona was not found.");
        }
        if (!validate_json_mutation(request, response)) return;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [](const nlohmann::json& json) { parse_empty_object(json); })) return;
        try {
            cha::app::workspace::delete_persona(*config, id);
            response.status = 204;
            response.set_header("Cache-Control", "no-store");
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    server.Patch(R"(/api/v1/personas/([^/]+))",
        [live_sessions, settings, config](const httplib::Request& request,
                                          httplib::Response& response) {
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) {
            return set_route_not_found(response, "That persona was not found.");
        }
        if (!validate_json_mutation(request, response)) return;
        PersonaUpdate update;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&update](const nlohmann::json& json) {
                    update = parse_persona_update(json);
                })) return;
        try {
            set_json_response(
                response, 200,
                nlohmann::json(cha::app::workspace::update_persona(
                    *config, *live_sessions, id, update)));
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    server.Get(R"(/api/v1/forums/([^/]+)/files/([^/]+))",
        [](const httplib::Request& request, httplib::Response& response) {
        const std::string id = request.matches[1];
        const std::string filename = request.matches[2];
        if (!is_valid_route_component(id)) {
            return set_route_not_found(response, "That forum file was not found.");
        }
        try {
            set_json_response(
                response, 200,
                nlohmann::json(cha::app::workspace::get_forum_file(id, filename)));
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    const auto edit_forum_file = [live_sessions, settings, config](
        const httplib::Request& request, httplib::Response& response,
        bool create, bool remove) {
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) {
            return set_route_not_found(response, "That forum file was not found.");
        }
        if (!validate_json_mutation(request, response)) return;
        std::string filename = create ? std::string() : request.matches[2].str();
        std::optional<std::string> content;
        if (!parse_route_json_body(request, response, settings.request_body_limit,
            [&](const nlohmann::json& json) {
                if (remove) return parse_empty_object(json);
                if (!json.is_object() || json.size() != (create ? 2 : 1)) {
                    throw std::invalid_argument("Invalid forum file");
                }
                if (create) filename = required_string(json, "filename");
                content = required_string(json, "content");
            })) return;
        try {
            if (remove) {
                cha::app::workspace::delete_forum_file(
                    *config, *live_sessions, id, filename);
                response.status = 204;
                response.set_header("Cache-Control", "no-store");
                return;
            }
            const auto file = create
                ? cha::app::workspace::create_forum_file(
                    *config, *live_sessions, id, filename, *content)
                : cha::app::workspace::update_forum_file(
                    *config, *live_sessions, id, filename, *content);
            set_json_response(response, create ? 201 : 200, nlohmann::json(file));
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    };
    server.Post(R"(/api/v1/forums/([^/]+)/files)",
        [edit_forum_file](const auto& request, auto& response) {
            edit_forum_file(request, response, true, false);
        });
    server.Put(R"(/api/v1/forums/([^/]+)/files/([^/]+))",
        [edit_forum_file](const auto& request, auto& response) {
            edit_forum_file(request, response, false, false);
        });
    server.Delete(R"(/api/v1/forums/([^/]+)/files/([^/]+))",
        [edit_forum_file](const auto& request, auto& response) {
            edit_forum_file(request, response, false, true);
        });

    // `[^/]+` cannot span the separator, so this never shadows the session
    // routes registered below it.
    server.Get(R"(/api/v1/forums/([^/]+))", [](const httplib::Request& request, httplib::Response& response) {
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) return set_route_not_found(response);
        try {
            set_json_response(
                response, 200,
                nlohmann::json(cha::app::workspace::get_forum(id)));
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    server.Delete(R"(/api/v1/forums/([^/]+))",
        [live_sessions, settings, config](const httplib::Request& request,
                                          httplib::Response& response) {
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) {
            return set_route_not_found(response, "That forum was not found.");
        }
        if (!validate_json_mutation(request, response)) return;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [](const nlohmann::json& json) { parse_empty_object(json); })) return;
        try {
            cha::app::workspace::delete_forum(*config, *live_sessions, id);
            response.status = 204;
            response.set_header("Cache-Control", "no-store");
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    server.Patch(R"(/api/v1/forums/([^/]+))",
        [live_sessions, settings, config](const httplib::Request& request,
                                          httplib::Response& response) {
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) {
            return set_route_not_found(response, "That forum was not found.");
        }
        if (!validate_json_mutation(request, response)) return;
        ForumUpdate update;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&update](const nlohmann::json& json) {
                    update = parse_forum_update(json);
                })) return;
        try {
            set_json_response(
                response, 200,
                nlohmann::json(cha::app::workspace::update_forum(
                    *config, *live_sessions, id, update)));
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    server.Put(R"(/api/v1/forums/([^/]+)/members)",
        [live_sessions, settings, config](const httplib::Request& request,
                                          httplib::Response& response) {
        const std::string id = request.matches[1];
        if (!is_valid_route_component(id)) {
            return set_route_not_found(response, "That forum was not found.");
        }
        if (!validate_json_mutation(request, response)) return;
        ForumMembersUpdate update;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&update](const nlohmann::json& json) {
                    update = parse_forum_members_update(json);
                })) return;
        try {
            set_json_response(
                response, 200,
                nlohmann::json(cha::app::workspace::update_forum_members(
                    *config, *live_sessions, id, update)));
        } catch (const cha::app::ApplicationError& error) {
            set_application_error(response, error);
        }
    });

    server.Get(R"(/api/v1/forums/([^/]+)/sessions)", [sessions, live_sessions](const httplib::Request& request, httplib::Response& response) {
        const std::string forum = request.matches[1];
        if (!is_valid_route_component(forum)) return set_route_not_found(response);
        try {
            const LiveSessionManagerSnapshot snapshot = live_sessions->snapshot();
            set_json_response(response, 200, nlohmann::json(sessions_for(*sessions, snapshot, forum)));
        } catch (const ForumNotFoundError&) {
            set_route_not_found(response);
        }
    });

    server.Post(R"(/api/v1/forums/([^/]+)/sessions)", [sessions, mirror, settings](const httplib::Request& request, httplib::Response& response) {
        const std::string forum = request.matches[1];
        if (!is_valid_route_component(forum)) return set_route_not_found(response);
        if (!validate_json_mutation(request, response)) return;
        std::string label;
        if (!parse_route_json_body(
                request,
                response,
                settings.request_body_limit,
                [&label](const nlohmann::json& json) {
                    label = parse_create_session_label(json);
                })) return;
        if (!validate_route_session_label(response, label, true)) return;
        try {
            const StoredSession created = sessions->create(forum, std::move(label));
            if (mirror) mirror->add(created);
            set_json_response(response, 201, nlohmann::json(CreateSessionSuccess{
                created.identity.session_id, created.label}));
        } catch (const ForumNotFoundError&) {
            set_route_not_found(response);
        } catch (const std::invalid_argument&) {
            set_error_response(response, 400,
                {ErrorCode::bad_request, "Invalid session label."});
        }
    });

    server.Get(R"(/api/v1/forums/([^/]+)/sessions/([^/]+)/download)",
        [sessions, live_sessions, settings](const httplib::Request& request,
                                             httplib::Response& response) {
        const FullSessionId key{request.matches[1], request.matches[2]};
        if (!is_valid_route_component(key.forum_id)
            || !is_valid_route_component(key.session_id)) {
            return set_route_not_found(response);
        }
        try {
            std::string markdown;
            if (const LiveSessionHandle live = live_sessions->lookup(key)) {
                CommandSubmitResult result = live->snapshot(settings.command_deadline);
                if (const auto* snapshot = std::get_if<SessionSnapshot>(&result)) {
                    markdown = session_markdown(
                        snapshot->session_label, snapshot->transcript);
                } else if (const auto* error = std::get_if<ErrorCode>(&result)) {
                    return set_command_error(response, *error);
                } else {
                    return set_error_response(response, 500,
                        {ErrorCode::internal_error,
                         "The session could not be downloaded."});
                }
            } else {
                PreparedSession prepared = sessions->prepare(key);
                markdown = session_markdown(
                    prepared.label, prepared.restore.entries);
            }
            response.status = 200;
            response.set_header("Cache-Control", "no-store");
            response.set_content(markdown, "text/markdown; charset=utf-8");
        } catch (const ForumNotFoundError&) {
            set_route_not_found(response);
        } catch (const SessionNotFoundError&) {
            set_route_not_found(response);
        }
    });

    server.Delete(R"(/api/v1/forums/([^/]+)/sessions/([^/]+)/audio-cache)",
        [sessions, settings, clear_audio](const httplib::Request& request, httplib::Response& response) {
        const FullSessionId key{request.matches[1], request.matches[2]};
        if (!is_valid_route_component(key.forum_id)
            || !is_valid_route_component(key.session_id)) {
            return set_route_not_found(response);
        }
        if (!validate_json_mutation(request, response)) return;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [](const nlohmann::json& json) { parse_empty_object(json); })) return;
        try {
            if (clear_audio) clear_audio(key);
            else sessions->clear_session_audio(key);
            response.status = 204;
            response.set_header("Cache-Control", "no-store");
        } catch (const AudioDownloadError& error) {
            set_error_response(response, error.status, {ErrorCode::speech_busy, error.what()});
        } catch (const ForumNotFoundError&) {
            set_route_not_found(response);
        } catch (const SessionNotFoundError&) {
            set_route_not_found(response);
        }
    });

    server.Patch(R"(/api/v1/forums/([^/]+)/sessions/([^/]+))",
        [sessions, initial, live_sessions, mirror, settings](const httplib::Request& request,
                                                     httplib::Response& response) {
        const FullSessionId key{request.matches[1], request.matches[2]};
        if (!is_valid_route_component(key.forum_id)
            || !is_valid_route_component(key.session_id)) {
            return set_route_not_found(response);
        }
        // Welcome is process-local and immutable. Reject it before consulting
        // the live registry, which deliberately bypasses repository reads for
        // an already-open session.
        if (key == initial.session) return set_route_not_found(response);
        if (!validate_json_mutation(request, response)) return;
        std::string label;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [&label](const nlohmann::json& json) {
                    label = parse_rename_session_label(json);
                })) return;
        if (!validate_route_session_label(response, label, false)) return;
        log_info(session_event(key, "rename_requested"));
        try {
            if (const LiveSessionHandle live = live_sessions->lookup(key)) {
                CommandSubmitResult result = live->submit(
                    RenameSessionCommand{std::move(label)},
                    settings.command_deadline);
                if (const auto* renamed = std::get_if<SessionLabelResult>(&result)) {
                    log_info(session_event(key, "rename_committed"));
                    return set_json_response(response, 200, nlohmann::json(*renamed));
                }
                if (const auto* error = std::get_if<ErrorCode>(&result)) {
                    log_warn(session_event(key, "rename_failed"));
                    return set_command_error(response, *error);
                }
                return set_error_response(response, 500,
                    {ErrorCode::internal_error, "The request could not be completed."});
            }
            const StoredSession renamed = sessions->rename(key, std::move(label));
            if (mirror) {
                mirror->update(
                    renamed.identity,
                    renamed.label,
                    sessions->history(renamed.identity));
            }
            log_info(session_event(key, "rename_committed"));
            set_json_response(response, 200, nlohmann::json(SessionLabelResult{
                renamed.identity.session_id, renamed.label}));
        } catch (const ForumNotFoundError&) {
            log_warn(session_event(key, "rename_failed"));
            set_route_not_found(response);
        } catch (const SessionNotFoundError&) {
            log_warn(session_event(key, "rename_failed"));
            set_route_not_found(response);
        } catch (const std::invalid_argument&) {
            log_warn(session_event(key, "rename_failed"));
            set_error_response(response, 400,
                {ErrorCode::bad_request, "Invalid session label."});
        } catch (...) {
            log_warn(session_event(key, "rename_failed"));
            throw;
        }
    });

    server.Delete(R"(/api/v1/forums/([^/]+)/sessions/([^/]+))",
        [sessions, initial, live_sessions, settings](const httplib::Request& request,
                                                     httplib::Response& response) {
        const FullSessionId key{request.matches[1], request.matches[2]};
        if (!is_valid_route_component(key.forum_id)
            || !is_valid_route_component(key.session_id)) {
            return set_route_not_found(response);
        }
        // The guard must precede the maintenance reservation: acquiring one
        // may stop a live actor, which is an impermissible side effect for an
        // immutable-session request that will ultimately be rejected.
        if (key == initial.session) return set_route_not_found(response);
        if (!validate_json_mutation(request, response)) return;
        if (!parse_route_json_body(
                request, response, settings.request_body_limit,
                [](const nlohmann::json& json) { parse_empty_object(json); })) return;

        log_info(session_event(key, "delete_requested"));
        MaintenanceReservationResult reserved =
            live_sessions->reserve_for_deletion(key, settings.delete_deadline);
        if (const auto* failure = std::get_if<MaintenanceFailure>(&reserved)) {
            log_warn(session_event(key, "delete_failed"));
            if (*failure == MaintenanceFailure::manager_stopping) {
                return set_error_response(response, 503,
                    {ErrorCode::server_stopping, "Server is stopping."});
            }
            return set_error_response(response, 409,
                {ErrorCode::session_stopping, "Session is stopping."});
        }
        try {
            sessions->delete_session(key);
            log_info(session_event(key, "delete_committed"));
            response.status = 204;
            response.set_header("Cache-Control", "no-store");
        } catch (const ForumNotFoundError&) {
            log_warn(session_event(key, "delete_failed"));
            set_route_not_found(response);
        } catch (const SessionNotFoundError&) {
            log_warn(session_event(key, "delete_failed"));
            set_route_not_found(response);
        } catch (...) {
            log_warn(session_event(key, "delete_failed"));
            throw;
        }
    });

    server.Post(R"(/api/v1/forums/([^/]+)/sessions/([^/]+)/open)", [sessions, live_sessions, settings](const httplib::Request& request, httplib::Response& response) {
        const FullSessionId key{request.matches[1], request.matches[2]};
        if (!is_valid_route_component(key.forum_id)
            || !is_valid_route_component(key.session_id)) {
            return set_route_not_found(response);
        }
        if (!validate_json_mutation(request, response)) return;
        if (!parse_route_json_body(
                request,
                response,
                settings.request_body_limit,
                [](const nlohmann::json& json) { parse_empty_object(json); })) return;
        if (const auto reattached = live_sessions->try_reattach(key)) {
            return set_open_result(response, key, *reattached);
        }
        try {
            sessions->validate(key);
        } catch (const ForumNotFoundError&) {
            return set_route_not_found(response);
        } catch (const SessionNotFoundError&) {
            return set_route_not_found(response);
        }
        set_open_result(
            response,
            key, live_sessions->open(key, settings.open_deadline));
    });
}

} // namespace cha::web
