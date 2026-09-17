#pragma once

#include "web/web_settings.h"

namespace httplib {
class Server;
}

namespace cha::web {

class ApplicationRuntime;

void install_vault_routes(
    httplib::Server& server,
    ApplicationRuntime* runtime,
    WebSettings settings);

} // namespace cha::web
