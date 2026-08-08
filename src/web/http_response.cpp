#include "web/http_response.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace cha::web {

void set_json_response(
    httplib::Response& response,
    int status,
    const nlohmann::json& body) {
    response.status = status;
    response.set_header("Cache-Control", "no-store");
    response.set_content(body.dump(), json_content_type);
}

void set_error_response(
    httplib::Response& response,
    int status,
    const Error& error) {
    set_json_response(response, status, nlohmann::json(error));
}

} // namespace cha::web
