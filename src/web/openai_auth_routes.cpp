#include "web/openai_auth_routes.h"

#include "app/settings_operations.h"
#include "providers/openai_oauth.h"
#include "web/http_response.h"
#include "web/json.h"
#include "web/route_support.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace cha::web {
namespace {

bool accept_empty_json_post(
    const httplib::Request& request,
    httplib::Response& response,
    const WebSettings& settings) {
    if (!validate_json_mutation(request, response)) return false;
    return parse_route_json_body(
        request,
        response,
        settings.request_body_limit,
        [](const nlohmann::json& json) { parse_empty_object(json); });
}

void set_snapshot(
    httplib::Response& response,
    const OpenAiAuth& snapshot) {
    set_json_response(response, 200, snapshot);
}

} // namespace

OpenAiAuthRoutes::OpenAiAuthRoutes(OpenAiOAuth& owner, WebSettings settings)
    : owner_(&owner), settings_(std::move(settings)) {}

void OpenAiAuthRoutes::install(httplib::Server& server) const {
    OpenAiOAuth* const owner = owner_;
    const WebSettings settings = settings_;

    server.Get(
        "/api/v1/openai/auth",
        [owner](const httplib::Request&, httplib::Response& response) {
            set_snapshot(response, cha::app::settings::openai_auth_status(*owner));
        });

    server.Post(
        "/api/v1/openai/auth/login",
        [owner, settings](
            const httplib::Request& request, httplib::Response& response) {
            if (!accept_empty_json_post(request, response, settings)) return;
            set_snapshot(response, cha::app::settings::start_openai_auth(*owner));
        });

    server.Post(
        "/api/v1/openai/auth/poll",
        [owner, settings](
            const httplib::Request& request, httplib::Response& response) {
            if (!accept_empty_json_post(request, response, settings)) return;
            set_snapshot(response, cha::app::settings::poll_openai_auth(*owner));
        });

    server.Post(
        "/api/v1/openai/auth/disconnect",
        [owner, settings](
            const httplib::Request& request, httplib::Response& response) {
            if (!accept_empty_json_post(request, response, settings)) return;
            set_snapshot(
                response, cha::app::settings::disconnect_openai_auth(*owner));
        });
}

} // namespace cha::web
