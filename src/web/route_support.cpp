#include "web/route_support.h"

#include "web/http_response.h"
#include "web/json.h"
#include "web/live_session_manager.h"
#include "util/path_name.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cha::web {
namespace {

bool has_matching_origin(const httplib::Request& request) {
    if (!request.has_header("Origin")) return true;

    const std::string origin_value = request.get_header_value("Origin");
    std::string_view origin(origin_value);
    constexpr std::string_view http = "http://";
    constexpr std::string_view https = "https://";
    if (origin.starts_with(http)) origin.remove_prefix(http.size());
    else if (origin.starts_with(https)) origin.remove_prefix(https.size());
    else return false;

    const std::string host = request.get_header_value("Host");
    return origin == host;
}

} // namespace

bool is_valid_route_component(std::string_view component) {
    return is_url_safe_identifier(component);
}

bool validate_json_mutation(
    const httplib::Request& request,
    httplib::Response& response) {
    if (!is_json_content_type(request.get_header_value("Content-Type"))) {
        set_error_response(
            response,
            400,
            {ErrorCode::bad_request, "Expected a JSON request body."});
        return false;
    }
    if (!has_matching_origin(request)) {
        set_error_response(
            response,
            403,
            {ErrorCode::forbidden_origin, "Request origin is not allowed."});
        return false;
    }
    return true;
}

bool parse_route_json_body(
    const httplib::Request& request,
    httplib::Response& response,
    std::size_t maximum_bytes,
    const RouteBodyParser& parser) {
    try {
        parser(parse_json_body(request.body, maximum_bytes));
        return true;
    } catch (const std::length_error&) {
        set_error_response(
            response,
            413,
            {ErrorCode::body_too_large, "Request body is too large."});
    } catch (const std::invalid_argument&) {
        set_error_response(
            response,
            400,
            {ErrorCode::bad_request, "Invalid JSON request body."});
    }
    return false;
}

void set_route_not_found(httplib::Response& response, std::string_view message) {
    set_error_response(
        response,
        404,
        {ErrorCode::not_found, std::string(message)});
}

void request_reload(
    LiveSessionManager& live_sessions,
    const std::vector<std::string>& forum_ids) {
    for (const LiveSessionHandle& live : live_sessions.active_sessions()) {
        const SessionIdentity& key = live->identity();
        if (std::ranges::find(forum_ids, key.forum_id) == forum_ids.end()) continue;
        live->request_shutdown(ShutdownReason::reloading);
    }
}

void request_workspace_reload(LiveSessionManager& live_sessions) {
    for (const LiveSessionHandle& live : live_sessions.active_sessions()) {
        live->request_shutdown(ShutdownReason::workspace_reloading);
    }
}

} // namespace cha::web
