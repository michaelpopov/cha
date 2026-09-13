#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace httplib {
class Server;
struct Response;
}

namespace cha::web {

// Keeps browser pages and future bundled assets separate from the JSON lobby
// API. The lobby owns browser-side persona, forum, and session selection.
class AssetHandler {
public:
    using ConnectUrlProvider =
        std::function<std::optional<std::string>()>;

    explicit AssetHandler(
        std::filesystem::path web_root,
        std::optional<std::string> connect_url = std::nullopt,
        std::vector<std::string> additional_connect_urls = {});
    AssetHandler(
        std::filesystem::path web_root,
        ConnectUrlProvider connect_url,
        std::vector<std::string> additional_connect_urls = {});

    void install(httplib::Server& server) const;
    void set_shell(httplib::Response& response) const;

private:
    [[nodiscard]] std::string content_security_policy() const;

    std::filesystem::path web_root_;
    std::string shell_;
    ConnectUrlProvider connect_url_;
    std::vector<std::string> additional_connect_urls_;
};

} // namespace cha::web
