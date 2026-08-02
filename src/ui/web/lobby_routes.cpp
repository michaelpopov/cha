#include "ui/web/lobby_routes.h"

#include "session/workspace.h"
#include "ui/web/http_response.h"
#include "ui/web/json.h"
#include "ui/web/protocol.h"
#include "ui/web/route_support.h"
#include "ui/web/session_registry.h"
#include "util/logging.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace cha::web {
namespace {

bool is_running(const RegistrySnapshot& snapshot, const SessionKey& key) {
    return std::find(snapshot.running_sessions.begin(), snapshot.running_sessions.end(), key)
        != snapshot.running_sessions.end();
}

int status_for(const ErrorCode code) {
    switch (code) {
    case ErrorCode::not_found: return 404;
    case ErrorCode::session_busy:
    case ErrorCode::session_stopping: return 409;
    case ErrorCode::session_limit_reached:
    case ErrorCode::session_open_timeout:
    case ErrorCode::server_stopping: return 503;
    default: return 500;
    }
}

void set_open_result(
    httplib::Response& response,
    const RegistryOpenResult& result) {
    if (const auto* success = std::get_if<OpenSessionSuccess>(&result)) {
        set_json_response(response, 200, nlohmann::json(*success));
        return;
    }
    const Error& error = std::get<Error>(result);
    set_error_response(response, status_for(error.code), error);
}

} // namespace

LobbyRoutes::LobbyRoutes(
    std::shared_ptr<const Workspace> workspace,
    SessionRegistry& registry,
    WebSettings settings)
    : workspace_(std::move(workspace)), registry_(registry), settings_(std::move(settings)) {
    if (!workspace_) throw std::invalid_argument("Lobby routes need a workspace");
}

void LobbyRoutes::install(httplib::Server& server) const {
    const auto workspace = workspace_;
    SessionRegistry* const registry = &registry_;
    const WebSettings settings = settings_;
    server.Get("/health", [registry](const httplib::Request&, httplib::Response& response) {
        const RegistrySnapshot snapshot = registry->snapshot();
        set_json_response(response, 200, nlohmann::json{
            {"ready", true}, {"live_session_count", snapshot.live_entry_count}});
    });

    server.Get("/api/v1/forums", [workspace](const httplib::Request&, httplib::Response& response) {
        std::vector<ForumSummary> forums;
        for (const std::string& name : workspace->forums()) {
            try {
                const Forum forum = workspace->load_forum(name);
                forums.push_back({forum.name, forum.display_name});
            } catch (const std::bad_alloc&) {
                throw;
            } catch (const std::exception& error) {
                log_warn(
                    "web server event=forum_omitted forum_id=" + name
                    + " reason=" + error.what());
            }
        }
        set_json_response(response, 200, nlohmann::json(forums));
    });

    server.Get("/api/v1/users", [workspace](const httplib::Request&, httplib::Response& response) {
        std::vector<UserSummary> users;
        for (const User& user : workspace->load_users()) {
            users.push_back({user.id, user.display_name});
        }
        set_json_response(response, 200, nlohmann::json(users));
    });

    server.Get(R"(/api/v1/forums/([^/]+)/sessions)", [workspace, registry](const httplib::Request& request, httplib::Response& response) {
        const std::string forum = request.matches[1];
        if (!is_valid_route_component(forum)) return set_route_not_found(response);
        try {
            const RegistrySnapshot snapshot = registry->snapshot();
            std::vector<SessionListing> sessions;
            for (const SessionSummary& stored : workspace->sessions(forum)) {
                sessions.push_back({stored.id, stored.label, is_running(snapshot, {forum, stored.id})});
            }
            set_json_response(response, 200, nlohmann::json(sessions));
        } catch (const ForumNotFoundError&) {
            set_route_not_found(response);
        }
    });

    server.Post(R"(/api/v1/forums/([^/]+)/sessions)", [workspace, settings](const httplib::Request& request, httplib::Response& response) {
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
        try {
            const SessionSummary created = workspace->create_stored_session(forum, std::move(label));
            set_json_response(response, 201, nlohmann::json(CreateSessionSuccess{created.id, created.label}));
        } catch (const ForumNotFoundError&) {
            set_route_not_found(response);
        }
    });

    server.Post(R"(/api/v1/forums/([^/]+)/sessions/([^/]+)/open)", [workspace, registry, settings](const httplib::Request& request, httplib::Response& response) {
        const SessionKey key{request.matches[1], request.matches[2]};
        if (!is_valid_route_component(key.forum)
            || !is_valid_route_component(key.session_id)) {
            return set_route_not_found(response);
        }
        if (!validate_json_mutation(request, response)) return;
        if (!parse_route_json_body(
                request,
                response,
                settings.request_body_limit,
                [](const nlohmann::json& json) { parse_empty_object(json); })) return;
        if (const auto reattached = registry->try_reattach(key)) {
            return set_open_result(response, *reattached);
        }
        try {
            workspace->check_session(key.forum, key.session_id);
        } catch (const ForumNotFoundError&) {
            return set_route_not_found(response);
        } catch (const SessionNotFoundError&) {
            return set_route_not_found(response);
        }
        set_open_result(
            response,
            registry->open(key, settings.open_deadline));
    });
}

} // namespace cha::web
