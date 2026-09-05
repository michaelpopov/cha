#pragma once

#include "web/web_settings.h"

namespace httplib {
class Server;
}

namespace cha {
class OpenAiOAuth;
}

namespace cha::web {

// Root-scoped ChatGPT connection routes. Each handler calls one owner
// operation and returns that snapshot; the browser polls.
class OpenAiAuthRoutes {
public:
    OpenAiAuthRoutes(OpenAiOAuth& owner, WebSettings settings);

    void install(httplib::Server& server) const;

private:
    OpenAiOAuth* owner_;
    WebSettings settings_;
};

} // namespace cha::web
