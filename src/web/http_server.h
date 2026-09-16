#pragma once

#include "web/web_settings.h"

#include <string>

namespace httplib {
class Server;
}

namespace cha::web {

// Installs process-wide HTTP limits and fallback error handling. Route
// components deliberately do not modify these server-global hooks.
// Returns the effective limits, including reserved native speech workers.
WebSettings configure_http_server(
    httplib::Server& server,
    WebSettings settings,
    bool native_voice_enabled = false);

} // namespace cha::web
