#include "web/session_routes.h"

#include "web/asset_handler.h"
#include "web/http_response.h"
#include "web/json.h"
#include "web/route_support.h"
#include "web/live_session_manager.h"
#include "web/sse_stream.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace cha::web {
namespace {

void set_not_live(httplib::Response& response) {
    set_error_response(response, 409, {ErrorCode::session_not_live, "That session is not open."});
}

void set_submission_error(httplib::Response& response, ErrorCode code) {
    switch (code) {
    case ErrorCode::command_queue_full:
        set_error_response(response, 503, {code, "The session command queue is full."});
        return;
    case ErrorCode::session_not_live:
        set_not_live(response);
        return;
    case ErrorCode::server_stopping:
        set_error_response(response, 503, {code, "Server is stopping."});
        return;
    case ErrorCode::command_timeout:
        set_error_response(response, 503, {code, "The command outcome is unknown."});
        return;
    default:
        set_error_response(response, 500, {ErrorCode::internal_error, "The request could not be completed."});
    }
}

std::optional<FullSessionId> validate_key(
    const httplib::Request& request,
    httplib::Response& response) {
    FullSessionId key{request.matches[1], request.matches[2]};
    if (!is_valid_route_component(key.forum_id)
        || !is_valid_route_component(key.session_id)) {
        set_route_not_found(response);
        return std::nullopt;
    }
    return key;
}

LiveSessionHandle resolve(
    const httplib::Request& request,
    httplib::Response& response,
    LiveSessionManager& live_sessions) {
    const std::optional<FullSessionId> key = validate_key(request, response);
    if (!key) return {};
    LiveSessionHandle session = live_sessions.lookup(*key);
    if (!session) set_not_live(response);
    return session;
}

void set_command_result(httplib::Response& response, CommandSubmitResult result) {
    if (const auto* command = std::get_if<CommandResult>(&result)) {
        set_json_response(response, 200, nlohmann::json(*command));
    } else if (const auto* error = std::get_if<ErrorCode>(&result)) {
        set_submission_error(response, *error);
    } else {
        set_error_response(response, 500, {ErrorCode::internal_error, "The request could not be completed."});
    }
}

} // namespace

SessionRoutes::SessionRoutes(
    LiveSessionManager& live_sessions,
    WebSettings settings,
    AssetHandler assets)
    : live_sessions_(live_sessions),
      settings_(std::move(settings)),
      assets_(std::move(assets)) {}

void SessionRoutes::install(httplib::Server& server) const {
    LiveSessionManager* const live_sessions = &live_sessions_;
    const WebSettings settings = settings_;
    const AssetHandler assets = assets_;
    constexpr auto base = R"(/s/([^/]+)/([^/]+))";

    server.Get(std::string(base) + R"(/)", [assets](const httplib::Request&, httplib::Response& response) {
        assets.set_shell(response);
    });
    server.Get(std::string(base) + R"(/api/v1/session)", [live_sessions, settings](const httplib::Request& request, httplib::Response& response) {
        LiveSessionHandle session = resolve(request, response, *live_sessions);
        if (!session) return;
        CommandSubmitResult result = session->snapshot(settings.command_deadline);
        if (const auto* snapshot = std::get_if<SessionSnapshot>(&result)) {
            set_json_response(response, 200, nlohmann::json(*snapshot));
        } else if (const auto* error = std::get_if<ErrorCode>(&result)) {
            set_submission_error(response, *error);
        } else {
            set_error_response(response, 500, {ErrorCode::internal_error, "The request could not be completed."});
        }
    });
    server.Get(std::string(base) + R"(/api/v1/events)", [live_sessions, settings](const httplib::Request& request, httplib::Response& response) {
        LiveSessionHandle session = resolve(request, response, *live_sessions);
        if (!session) return;
        CommandSubmitResult result = session->connect_sse(settings.command_deadline);
        const auto* connection = std::get_if<SseConnectResult>(&result);
        if (!connection) {
            if (const auto* error = std::get_if<ErrorCode>(&result)) {
                set_submission_error(response, *error);
            } else {
                set_error_response(response, 500, {ErrorCode::internal_error, "The event stream could not be opened."});
            }
            return;
        }
        response.set_header("Cache-Control", "no-cache");
        response.set_header("X-Accel-Buffering", "no");
        response.set_chunked_content_provider(
            "text/event-stream",
            [mailbox = connection->mailbox, stream = connection->stream,
             interval = settings.sse_heartbeat_interval](
                std::size_t,
                httplib::DataSink& sink) {
                return SseStreamWriter(mailbox, stream, interval).write(sink);
            },
            // Retaining the actor keeps this callback's target alive even
            // after the manager has removed and joined it; a call on a
            // finished actor is a safe no-op.
            [session, mailbox = connection->mailbox, stream = connection->stream,
             connection_id = connection->connection_id](bool) {
                session->disconnect_sse(
                    connection_id, mailbox->end_stream(stream));
            });
    });
    server.Post(std::string(base) + R"(/api/v1/input)", [live_sessions, settings](const httplib::Request& request, httplib::Response& response) {
        const std::optional<FullSessionId> key = validate_key(request, response);
        if (!key) return;
        if (!validate_json_mutation(request, response)) return;
        RawCommand command;
        if (!parse_route_json_body(request, response, settings.request_body_limit, [&command](const nlohmann::json& json) {
                command = parse_input_command(json);
            })) return;
        if (command.text.size() > settings.prompt_limit) {
            return set_error_response(
                response,
                413,
                {ErrorCode::prompt_too_large, "Prompt is too large."});
        }
        LiveSessionHandle session = live_sessions->lookup(*key);
        if (!session) return set_not_live(response);
        CommandSubmitResult result =
            session->submit(std::move(command), settings.command_deadline);
        if (const auto* completed = std::get_if<CommandResult>(&result);
            completed && completed->persist_default_persona_id) {
            request_reload(*live_sessions, {key->forum_id});
        }
        set_command_result(response, std::move(result));
    });
    server.Post(std::string(base) + R"(/api/v1/actions/stop)", [live_sessions, settings](const httplib::Request& request, httplib::Response& response) {
        const std::optional<FullSessionId> key = validate_key(request, response);
        if (!key) return;
        if (!validate_json_mutation(request, response)) return;
        if (!parse_route_json_body(request, response, settings.request_body_limit, [](const nlohmann::json& json) { parse_empty_object(json); })) return;
        LiveSessionHandle session = live_sessions->lookup(*key);
        if (!session) return set_not_live(response);
        set_command_result(response, session->submit(StopCommand{}, settings.command_deadline));
    });
    const auto set_default_character_handler =
        [live_sessions, settings](const httplib::Request& request, httplib::Response& response) {
        const std::optional<FullSessionId> key = validate_key(request, response);
        if (!key) return;
        if (!validate_json_mutation(request, response)) return;
        SetDefaultCharacterCommand command;
        if (!parse_route_json_body(request, response, settings.request_body_limit, [&command](const nlohmann::json& json) {
                command = parse_default_character_command(json);
                if (command.character_id.empty()) throw std::invalid_argument("Character ID is empty");
            })) return;
        LiveSessionHandle session = live_sessions->lookup(*key);
        if (!session) return set_not_live(response);
        set_command_result(response, session->submit(std::move(command), settings.command_deadline));
    };
    server.Post(
        std::string(base) + R"(/api/v1/actions/default-character)",
        set_default_character_handler);
    // Compatibility alias for clients built before the vocabulary migration.
    server.Post(
        std::string(base) + R"(/api/v1/actions/default-agent)",
        set_default_character_handler);
}

} // namespace cha::web
