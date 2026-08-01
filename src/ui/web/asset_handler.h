#pragma once

namespace httplib {
class Server;
struct Response;
}

namespace cha::web {

// Keeps page and future bundled-asset serving separate from the JSON lobby
// API. This block intentionally supplies only a framework-neutral placeholder.
class AssetHandler {
public:
    void install(httplib::Server& server) const;
    static void set_chat_page(httplib::Response& response);
    static void set_session_not_open_page(httplib::Response& response);
};

} // namespace cha::web
