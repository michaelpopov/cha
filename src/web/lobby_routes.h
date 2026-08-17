#pragma once

#include "session/session_identity.h"
#include "web/web_settings.h"

#include <memory>
#include <string>

namespace httplib {
class Server;
}

namespace cha {
class WorkspaceRuntime;
}

namespace cha::web {

class LiveSessionManager;

// What the browser starts on. These are startup facts rather than model state,
// so the application decides them and the route layer only reports them.
struct InitialSelection {
    SessionIdentity session;
};

class LobbyRoutes {
public:
    LobbyRoutes(
        std::shared_ptr<WorkspaceRuntime> workspace,
        InitialSelection initial,
        LiveSessionManager& live_sessions,
        WebSettings settings);

    void install(httplib::Server& server) const;

private:
    std::shared_ptr<WorkspaceRuntime> workspace_;
    InitialSelection initial_;
    LiveSessionManager& live_sessions_;
    WebSettings settings_;
};

} // namespace cha::web
