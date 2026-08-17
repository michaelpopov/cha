#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace httplib {
struct Request;
struct Response;
}

namespace cha::web {

class LiveSessionManager;

using RouteBodyParser = std::function<void(const nlohmann::json&)>;

[[nodiscard]] bool is_valid_route_component(std::string_view component);
[[nodiscard]] bool validate_json_mutation(
    const httplib::Request& request,
    httplib::Response& response);
[[nodiscard]] bool parse_route_json_body(
    const httplib::Request& request,
    httplib::Response& response,
    std::size_t maximum_bytes,
    const RouteBodyParser& parser);
void set_route_not_found(
    httplib::Response& response,
    std::string_view message = "That forum or session was not found.");

// Called only after a settings write has committed. Sessions still starting
// are included because they may already have read their definitions; an actor
// that appears only afterwards will read the new settings when it opens.
void request_reload(
    LiveSessionManager& live_sessions,
    const std::vector<std::string>& forum_ids);
// Reloads every active session after a complete workspace generation swap.
void request_workspace_reload(LiveSessionManager& live_sessions);

} // namespace cha::web
