#pragma once

#include "ui/web/web_settings.h"

namespace httplib {
class Server;
}

namespace cha::web {

// Installs process-wide HTTP limits and fallback error handling. Route
// components deliberately do not modify these server-global hooks.
void configure_http_server(httplib::Server& server, WebSettings settings);

} // namespace cha::web
