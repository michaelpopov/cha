#pragma once

#include "web/web_settings.h"

namespace httplib {
class Server;
}

namespace cha {
class ApiKeyStore;
class OpenAiOAuth;
class WorkspaceConfigStore;
}

namespace cha::web {

class LiveSessionManager;

class SettingsRoutes {
public:
    SettingsRoutes(
        LiveSessionManager& live_sessions,
        WebSettings settings,
        WorkspaceConfigStore& config,
        ApiKeyStore& api_keys,
        OpenAiOAuth& openai_auth);

    void install(httplib::Server& server) const;

private:
    LiveSessionManager* live_sessions_;
    WebSettings settings_;
    WorkspaceConfigStore* config_;
    ApiKeyStore* api_keys_;
    OpenAiOAuth* openai_auth_;
};

} // namespace cha::web
