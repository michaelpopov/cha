#pragma once

#include "ui/web/web_settings.h"

namespace httplib {
class Server;
}

namespace cha::web {

class SessionRegistry;

// Path-scoped routes are deliberately separate from the lobby's stored
// session API. Every request resolves one running handle and retains it until
// its owner-queue operation has completed. Block 10 owns registration of the
// reserved /api/v1/events suffix; until then it deliberately falls through to
// the server's ordinary not-found response.
class SessionRoutes {
public:
    SessionRoutes(SessionRegistry& registry, WebSettings settings);

    void install(httplib::Server& server) const;

private:
    SessionRegistry& registry_;
    WebSettings settings_;
};

} // namespace cha::web
