#pragma once

#include "session/session_identity.h"
#include "web/web_settings.h"

#include <memory>
#include <filesystem>
#include <string>

namespace httplib {
class Server;
}

namespace cha {
class SessionRepository;
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
        std::shared_ptr<const SessionRepository> sessions,
        InitialSelection initial,
        LiveSessionManager& live_sessions,
        std::filesystem::path backup_dir,
        WebSettings settings);

    void install(httplib::Server& server) const;

private:
    std::shared_ptr<const SessionRepository> sessions_;
    InitialSelection initial_;
    LiveSessionManager& live_sessions_;
    std::filesystem::path backup_dir_;
    WebSettings settings_;
};

} // namespace cha::web
