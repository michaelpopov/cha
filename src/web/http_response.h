#pragma once

#include "web/protocol.h"

#include <exception>
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace httplib {
struct Response;
}

namespace cha::web {

inline constexpr const char json_content_type[] = "application/json";
void set_json_response(
    httplib::Response& response,
    int status,
    const nlohmann::json& body);
void set_error_response(
    httplib::Response& response,
    int status,
    const Error& error);
void internal_error(httplib::Response& response, const std::exception& error);

} // namespace cha::web
