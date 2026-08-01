#pragma once

#include <cstddef>
#include <functional>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace httplib {
struct Request;
struct Response;
}

namespace cha::web {

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
void set_route_not_found(httplib::Response& response);

} // namespace cha::web
