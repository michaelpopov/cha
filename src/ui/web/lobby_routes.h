#pragma once

#include "ui/web/web_settings.h"

#include <memory>

namespace httplib {
class Server;
}

namespace cha {
class Workspace;
}

namespace cha::web {

class SessionRegistry;

class LobbyRoutes {
public:
    LobbyRoutes(
        std::shared_ptr<const Workspace> workspace,
        SessionRegistry& registry,
        WebSettings settings);

    void install(httplib::Server& server) const;

private:
    std::shared_ptr<const Workspace> workspace_;
    SessionRegistry& registry_;
    WebSettings settings_;
};

} // namespace cha::web
