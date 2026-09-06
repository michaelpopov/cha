#pragma once

#include "chat/session_identity.h"
#include "web/web_settings.h"

#include <memory>
#include <string>
#include <vector>

namespace httplib {
class Server;
}

namespace cha {
class SessionRepository;
class WorkspaceConfigStore;
}

namespace cha::web {

class CurrentVault;
class LiveSessionManager;
class SessionMirror;

// What the browser starts on. These are startup facts rather than model state,
// so the application decides them and the route layer only reports them.
struct InitialSelection {
    FullSessionId session;
};

class LobbyRoutes {
public:
    LobbyRoutes(
        std::shared_ptr<const SessionRepository> sessions,
        InitialSelection initial,
        LiveSessionManager& live_sessions,
        WebSettings settings,
        WorkspaceConfigStore& config,
        CurrentVault& current_vault,
        std::vector<std::string> vault_names,
        std::shared_ptr<SessionMirror> mirror = {});

    void install(httplib::Server& server) const;

private:
    std::shared_ptr<const SessionRepository> sessions_;
    InitialSelection initial_;
    LiveSessionManager& live_sessions_;
    WebSettings settings_;
    WorkspaceConfigStore* config_;
    CurrentVault* current_vault_;
    std::vector<std::string> vault_names_;
    std::shared_ptr<SessionMirror> mirror_;
};

} // namespace cha::web
