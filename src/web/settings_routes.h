#pragma once

#include "web/web_settings.h"

#include <string>

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
class FishAudioProxy;

class SettingsRoutes {
public:
    SettingsRoutes(
        LiveSessionManager& live_sessions,
        WebSettings settings,
        WorkspaceConfigStore& config,
        ApiKeyStore& api_keys,
        OpenAiOAuth& openai_auth,
        bool voice_enabled,
        FishAudioProxy& fish_audio);

    void install(httplib::Server& server) const;

private:
    LiveSessionManager* live_sessions_;
    WebSettings settings_;
    WorkspaceConfigStore* config_;
    ApiKeyStore* api_keys_;
    OpenAiOAuth* openai_auth_;
    bool voice_enabled_{};
    FishAudioProxy* fish_audio_;
};

} // namespace cha::web
