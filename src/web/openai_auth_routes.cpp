#include "web/openai_auth_routes.h"

#include "providers/openai_oauth.h"
#include "web/http_response.h"
#include "web/json.h"
#include "web/route_support.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <string_view>

namespace cha::web {
namespace {

std::string_view status_name(OpenAiOAuthState state) {
    switch (state) {
    case OpenAiOAuthState::waiting:
        return "waiting";
    case OpenAiOAuthState::connected:
        return "connected";
    case OpenAiOAuthState::signed_out:
        return "signed_out";
    }
    return "signed_out";
}

nlohmann::json snapshot_json(const OpenAiOAuthSnapshot& snapshot) {
    nlohmann::json json{{"status", status_name(snapshot.state)}};
    if (snapshot.state == OpenAiOAuthState::waiting) {
        if (snapshot.user_code) json["user_code"] = *snapshot.user_code;
        if (snapshot.verification_url) json["verification_url"] = *snapshot.verification_url;
        if (snapshot.attempt_expires_at) json["attempt_expires_at"] = *snapshot.attempt_expires_at;
        if (snapshot.next_poll_delay_ms) json["next_poll_delay_ms"] = *snapshot.next_poll_delay_ms;
    }
    if (snapshot.error) json["error"] = *snapshot.error;
    return json;
}

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
    const OpenAiOAuthSnapshot& snapshot) {
    set_json_response(response, 200, snapshot_json(snapshot));
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
            set_snapshot(response, owner->status());
        });

    server.Post(
        "/api/v1/openai/auth/login",
        [owner, settings](
            const httplib::Request& request, httplib::Response& response) {
            if (!accept_empty_json_post(request, response, settings)) return;
            set_snapshot(response, owner->start());
        });

    server.Post(
        "/api/v1/openai/auth/poll",
        [owner, settings](
            const httplib::Request& request, httplib::Response& response) {
            if (!accept_empty_json_post(request, response, settings)) return;
            set_snapshot(response, owner->poll());
        });

    server.Post(
        "/api/v1/openai/auth/disconnect",
        [owner, settings](
            const httplib::Request& request, httplib::Response& response) {
            if (!accept_empty_json_post(request, response, settings)) return;
            set_snapshot(response, owner->disconnect());
        });
}

} // namespace cha::web
