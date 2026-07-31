#include "ui/web/asset_handler.h"

#include <httplib.h>

namespace cha::web {

void AssetHandler::install(httplib::Server& server) const {
    server.Get("/", [](const httplib::Request&, httplib::Response& response) {
        response.set_content(
            "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
            "<title>cha</title></head><body><main><h1>cha</h1>"
            "<p>The lobby browser is not installed yet.</p></main></body></html>",
            "text/html; charset=utf-8");
    });
}

} // namespace cha::web
