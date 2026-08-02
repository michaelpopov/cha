#pragma once

namespace httplib {
class Server;
struct Response;
}

namespace cha::web {

// Keeps browser pages and future bundled assets separate from the JSON lobby
// API. The lobby owns browser-side persona, forum, and session selection.
class AssetHandler {
public:
    void install(httplib::Server& server) const;
    static void set_chat_page(httplib::Response& response);
    static void set_session_not_open_page(httplib::Response& response);
};

} // namespace cha::web
