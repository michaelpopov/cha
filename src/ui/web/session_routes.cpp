#include "ui/web/session_routes.h"

#include "ui/web/asset_handler.h"
#include "ui/web/http_response.h"
#include "ui/web/json.h"
#include "ui/web/route_support.h"
#include "ui/web/session_registry.h"
#include "ui/web/sse_stream.h"

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
    case ErrorCode::browser_stream_in_use:
        set_error_response(response, 409, {code, "This session is already open in another browser page."});
        return;
    default:
        set_error_response(response, 500, {ErrorCode::internal_error, "The request could not be completed."});
    }
}

std::optional<SessionKey> validate_key(
    const httplib::Request& request,
    httplib::Response& response) {
    SessionKey key{request.matches[1], request.matches[2]};
    if (!is_valid_route_component(key.forum)
        || !is_valid_route_component(key.session_id)) {
        set_route_not_found(response);
        return std::nullopt;
    }
    return key;
}

SessionHandle resolve(
    const httplib::Request& request,
    httplib::Response& response,
    SessionRegistry& registry,
    bool page) {
    const std::optional<SessionKey> key = validate_key(request, response);
    if (!key) return {};
    SessionHandle handle = registry.lookup(*key);
    if (!handle) {
        if (page) AssetHandler::set_session_not_open_page(response);
        else set_not_live(response);
    }
    return handle;
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

SessionRoutes::SessionRoutes(SessionRegistry& registry, WebSettings settings)
    : registry_(registry), settings_(std::move(settings)) {}

void SessionRoutes::install(httplib::Server& server) const {
    SessionRegistry* const registry = &registry_;
    const WebSettings settings = settings_;
    constexpr auto base = R"(/s/([^/]+)/([^/]+))";

    server.Get(std::string(base) + R"(/)", [registry](const httplib::Request& request, httplib::Response& response) {
        if (resolve(request, response, *registry, true)) AssetHandler::set_chat_page(response);
    });
    server.Get(std::string(base) + R"(/api/v1/session)", [registry, settings](const httplib::Request& request, httplib::Response& response) {
        SessionHandle handle = resolve(request, response, *registry, false);
        if (!handle) return;
        CommandSubmitResult result = handle.runtime().snapshot(settings.command_deadline);
        if (const auto* snapshot = std::get_if<SessionSnapshot>(&result)) {
            set_json_response(response, 200, nlohmann::json(*snapshot));
        } else if (const auto* error = std::get_if<ErrorCode>(&result)) {
            set_submission_error(response, *error);
        } else {
            set_error_response(response, 500, {ErrorCode::internal_error, "The request could not be completed."});
        }
    });
    server.Get(std::string(base) + R"(/api/v1/events)", [registry, settings](const httplib::Request& request, httplib::Response& response) {
        SessionHandle handle = resolve(request, response, *registry, false);
        if (!handle) return;
        CommandSubmitResult result = handle.runtime().connect_sse(settings.command_deadline);
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
                httplib::DataSink& sink) mutable {
                return SseStreamWriter(mailbox, stream, interval).write(sink);
            },
            [handle, mailbox = connection->mailbox, stream = connection->stream,
             connection_id = connection->connection_id](bool) {
                handle.runtime().disconnect_sse(
                    connection_id, mailbox->end_stream(stream));
            });
    });
    server.Post(std::string(base) + R"(/api/v1/input)", [registry, settings](const httplib::Request& request, httplib::Response& response) {
        const std::optional<SessionKey> key = validate_key(request, response);
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
        SessionHandle handle = registry->lookup(*key);
        if (!handle) return set_not_live(response);
        set_command_result(response, handle.runtime().submit(std::move(command), settings.command_deadline));
    });
    server.Post(std::string(base) + R"(/api/v1/actions/stop)", [registry, settings](const httplib::Request& request, httplib::Response& response) {
        const std::optional<SessionKey> key = validate_key(request, response);
        if (!key) return;
        if (!validate_json_mutation(request, response)) return;
        if (!parse_route_json_body(request, response, settings.request_body_limit, [](const nlohmann::json& json) { parse_empty_object(json); })) return;
        SessionHandle handle = registry->lookup(*key);
        if (!handle) return set_not_live(response);
        set_command_result(response, handle.runtime().submit(StopCommand{}, settings.command_deadline));
    });
    server.Post(std::string(base) + R"(/api/v1/actions/default-agent)", [registry, settings](const httplib::Request& request, httplib::Response& response) {
        const std::optional<SessionKey> key = validate_key(request, response);
        if (!key) return;
        if (!validate_json_mutation(request, response)) return;
        SetDefaultAgentCommand command;
        if (!parse_route_json_body(request, response, settings.request_body_limit, [&command](const nlohmann::json& json) {
                command = parse_default_agent_command(json);
                if (command.persona_id.empty()) throw std::invalid_argument("Persona ID is empty");
            })) return;
        SessionHandle handle = registry->lookup(*key);
        if (!handle) return set_not_live(response);
        set_command_result(response, handle.runtime().submit(std::move(command), settings.command_deadline));
    });
}

} // namespace cha::web
