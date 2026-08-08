#pragma once

#include <filesystem>
#include <string>

namespace httplib {
class Server;
struct Response;
}

namespace cha::web {

// Keeps browser pages and future bundled assets separate from the JSON lobby
// API. The lobby owns browser-side persona, forum, and session selection.
class AssetHandler {
public:
    explicit AssetHandler(std::filesystem::path web_root);

    void install(httplib::Server& server) const;
    void set_shell(httplib::Response& response) const;

private:
    std::filesystem::path web_root_;
    std::string shell_;
};

} // namespace cha::web
