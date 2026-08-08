#pragma once

#include "web/asset_handler.h"
#include "web/web_settings.h"

namespace httplib {
class Server;
}

namespace cha::web {

class LiveSessionManager;

// Path-scoped routes are deliberately separate from the lobby's stored
// session API. Every request resolves one running actor and retains it until
// its owner-queue operation has completed. Block 10 owns registration of the
// reserved /api/v1/events suffix; until then it deliberately falls through to
// the server's ordinary not-found response.
class SessionRoutes {
public:
    SessionRoutes(
        LiveSessionManager& live_sessions,
        WebSettings settings,
        AssetHandler assets);

    void install(httplib::Server& server) const;

private:
    LiveSessionManager& live_sessions_;
    WebSettings settings_;
    AssetHandler assets_;
};

} // namespace cha::web
