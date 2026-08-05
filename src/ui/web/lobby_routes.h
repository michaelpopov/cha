#pragma once

#include "ui/web/web_settings.h"

#include <memory>

namespace httplib {
class Server;
}

namespace cha {
class Workspace;
class WebDiscovery;
class WelcomeStorage;
}

namespace cha::web {

class SessionRegistry;

class LobbyRoutes {
public:
    LobbyRoutes(
        std::shared_ptr<const Workspace> workspace,
        const WebDiscovery& discovery,
        const WelcomeStorage& welcome_storage,
        SessionRegistry& registry,
        WebSettings settings);

    void install(httplib::Server& server) const;

private:
    std::shared_ptr<const Workspace> workspace_;
    const WebDiscovery& discovery_;
    const WelcomeStorage& welcome_storage_;
    SessionRegistry& registry_;
    WebSettings settings_;
};

} // namespace cha::web
