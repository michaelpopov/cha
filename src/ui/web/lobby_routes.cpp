#include "ui/web/lobby_routes.h"

#include "session/workspace.h"
#include "ui/web/http_response.h"
#include "ui/web/json.h"
#include "ui/web/protocol.h"
#include "ui/web/session_registry.h"
#include "util/logging.h"
#include "util/path_name.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace cha::web {
namespace {

constexpr std::string_view not_found_message = "That forum or session was not found.";

void set_not_found(httplib::Response& response) {
    set_error_response(response, 404, {ErrorCode::not_found, std::string(not_found_message)});
}

void set_internal_error(httplib::Response& response) {
    set_error_response(response, 500, {ErrorCode::internal_error, "The request could not be completed."});
}

bool has_matching_origin(const httplib::Request& request) {
    if (!request.has_header("Origin")) return true;
    const std::string origin = request.get_header_value("Origin");
    constexpr std::string_view prefix = "http://";
    constexpr std::string_view secure_prefix = "https://";
    std::string_view authority(origin);
    if (authority.starts_with(prefix)) authority.remove_prefix(prefix.size());
    else if (authority.starts_with(secure_prefix)) authority.remove_prefix(secure_prefix.size());
    else return false;
    return authority == request.get_header_value("Host");
}

bool validate_mutation(const httplib::Request& request, httplib::Response& response) {
    if (!is_json_content_type(request.get_header_value("Content-Type"))) {
        set_error_response(response, 400, {ErrorCode::bad_request, "Expected a JSON request body."});
        return false;
    }
    if (!has_matching_origin(request)) {
        set_error_response(response, 403, {ErrorCode::forbidden_origin, "Request origin is not allowed."});
        return false;
    }
    return true;
}

bool validate_component(std::string_view component) {
    try {
        require_path_component(component, "route identifier");
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool validate_open_body(
    const httplib::Request& request,
    httplib::Response& response,
    std::size_t maximum_bytes) {
    try {
        parse_empty_object(parse_json_body(request.body, maximum_bytes));
        return true;
    } catch (const std::length_error&) {
        set_error_response(
            response,
            413,
            {ErrorCode::body_too_large, "Request body is too large."});
        return false;
    } catch (const std::invalid_argument&) {
        set_error_response(
            response,
            400,
            {ErrorCode::bad_request, "Invalid JSON request body."});
        return false;
    }
}

bool is_running(const RegistrySnapshot& snapshot, const SessionKey& key) {
    return std::find(snapshot.running_sessions.begin(), snapshot.running_sessions.end(), key)
        != snapshot.running_sessions.end();
}

int status_for(const ErrorCode code) {
    switch (code) {
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
        try {
            std::vector<ForumSummary> forums;
            for (const std::string& name : workspace->forums()) {
                try {
                    const Forum forum = workspace->load_forum(name);
                    forums.push_back({forum.name, forum.display_name});
                } catch (const std::bad_alloc&) {
                    throw;
                } catch (const std::exception& error) {
                    log_warn(
                        "Forum omitted from lobby listing: forum_id=" + name
                        + " reason=" + error.what());
                }
            }
            set_json_response(response, 200, nlohmann::json(forums));
        } catch (const std::exception&) {
            set_internal_error(response);
        }
    });

    server.Get(R"(/api/v1/forums/([^/]+)/sessions)", [workspace, registry](const httplib::Request& request, httplib::Response& response) {
        const std::string forum = request.matches[1];
        if (!validate_component(forum)) return set_not_found(response);
        try {
            const RegistrySnapshot snapshot = registry->snapshot();
            std::vector<SessionListing> sessions;
            for (const SessionSummary& stored : workspace->sessions(forum)) {
                sessions.push_back({stored.id, stored.label, is_running(snapshot, {forum, stored.id})});
            }
            set_json_response(response, 200, nlohmann::json(sessions));
        } catch (const ForumNotFoundError&) {
            set_not_found(response);
        } catch (const std::exception&) {
            set_internal_error(response);
        }
    });

    server.Post(R"(/api/v1/forums/([^/]+)/sessions)", [workspace, settings](const httplib::Request& request, httplib::Response& response) {
        const std::string forum = request.matches[1];
        if (!validate_component(forum)) return set_not_found(response);
        if (!validate_mutation(request, response)) return;
        try {
            (void)workspace->load_forum(forum);
        } catch (const ForumNotFoundError&) {
            return set_not_found(response);
        } catch (const std::exception&) {
            return set_internal_error(response);
        }
        std::string label;
        try {
            label = parse_create_session_label(
                parse_json_body(request.body, settings.request_body_limit));
        } catch (const std::length_error&) {
            set_error_response(response, 413, {ErrorCode::body_too_large, "Request body is too large."});
            return;
        } catch (const std::invalid_argument&) {
            set_error_response(response, 400, {ErrorCode::bad_request, "Invalid JSON request body."});
            return;
        }
        try {
            const SessionSummary created = workspace->create_stored_session(forum, std::move(label));
            set_json_response(response, 201, nlohmann::json(CreateSessionSuccess{created.id, created.label}));
        } catch (const ForumNotFoundError&) {
            set_not_found(response);
        } catch (const std::exception&) {
            set_internal_error(response);
        }
    });

    server.Post(R"(/api/v1/forums/([^/]+)/sessions/([^/]+)/open)", [workspace, registry, settings](const httplib::Request& request, httplib::Response& response) {
        const SessionKey key{request.matches[1], request.matches[2]};
        if (!validate_component(key.forum) || !validate_component(key.session_id)) return set_not_found(response);
        if (!validate_mutation(request, response)) return;
        if (!validate_open_body(request, response, settings.request_body_limit)) return;
        if (const auto reattached = registry->try_reattach(key)) {
            return set_open_result(response, *reattached);
        }
        try {
            workspace->check_session(key.forum, key.session_id);
        } catch (const ForumNotFoundError&) {
            return set_not_found(response);
        } catch (const SessionNotFoundError&) {
            return set_not_found(response);
        } catch (const std::exception&) {
            return set_internal_error(response);
        }
        set_open_result(
            response,
            registry->open(key, settings.open_deadline));
    });
}

} // namespace cha::web
