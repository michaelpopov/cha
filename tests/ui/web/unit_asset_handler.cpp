#include "ui/web/asset_handler.h"

#include "support/test_workspace.h"
#include "ui/web/http_server.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace cha::web {
namespace {

class AssetServer {
public:
    explicit AssetServer(const std::filesystem::path& web_root) {
        AssetHandler(web_root).install(server_);
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ <= 0) throw std::runtime_error("Could not bind asset test server");
        configure_http_server(server_, {}, "127.0.0.1", port_);
        listener_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }

    ~AssetServer() {
        server_.stop();
        listener_.join();
    }

    httplib::Client client() const {
        httplib::Client result("127.0.0.1", port_);
        result.set_keep_alive(false);
        return result;
    }

private:
    httplib::Server server_;
    int port_{};
    std::thread listener_;
};

TEST(AssetHandler, RequiresTheBrowserShellAtConstruction) {
    const std::filesystem::path missing =
        std::filesystem::temp_directory_path() / "cha_missing_web_root";
    std::error_code error;
    std::filesystem::remove_all(missing, error);
    try {
        (void)AssetHandler(missing);
        FAIL() << "expected a missing shell to fail";
    } catch (const std::runtime_error& failure) {
        EXPECT_NE(std::string(failure.what()).find("index.html"), std::string::npos);
        EXPECT_NE(std::string(failure.what()).find("npm run stage"), std::string::npos);
    }
}

TEST(AssetHandler, ServesShellAndHashedAssetsWithProductionHeaders) {
    test::TestWorkspace fixture;
    AssetServer server(fixture.root() / "web");

    const auto shell = server.client().Get("/");
    ASSERT_TRUE(shell);
    EXPECT_EQ(shell->status, 200);
    EXPECT_EQ(shell->body.find("test shell") == std::string::npos, false);
    EXPECT_EQ(shell->get_header_value("Cache-Control"), "no-cache");
    EXPECT_EQ(
        shell->get_header_value("Content-Security-Policy"),
        "default-src 'none'; script-src 'self'; style-src 'self'; "
        "img-src 'self' data:; font-src 'self'; connect-src 'self'; "
        "base-uri 'none'; form-action 'none'; frame-ancestors 'none'");

    const auto asset = server.client().Get("/assets/app.js");
    ASSERT_TRUE(asset);
    EXPECT_EQ(asset->status, 200);
    EXPECT_EQ(asset->get_header_value("Content-Type"),
              "text/javascript; charset=utf-8");
    EXPECT_EQ(asset->get_header_value("Cache-Control"),
              "public, max-age=31536000, immutable");
}

TEST(AssetHandler, RejectsUnsafeAndUnsupportedAssetPaths) {
    test::TestWorkspace fixture;
    std::ofstream(fixture.root() / "web" / "assets" / "unknown.exe") << "no";
    std::filesystem::create_directories(fixture.root() / "web" / "assets" / "nested");
    std::ofstream(fixture.root() / "web" / "assets" / "nested" / "app.js") << "no";
    AssetServer server(fixture.root() / "web");

    for (const std::string path : {
             "/assets/unknown.exe", "/assets/nested/app.js",
             "/assets/%2e%2e", "/assets/%2fetc%2fpasswd"}) {
        const auto result = server.client().Get(path);
        ASSERT_TRUE(result) << path;
        EXPECT_EQ(result->status, 404) << path;
        const nlohmann::json body = nlohmann::json::parse(result->body);
        EXPECT_EQ(body["error"]["code"], "not_found") << path;
    }
}

TEST(HttpServer, WildcardListenerAcceptsOnlyLocalhostAndIpLiteralAuthorities) {
    httplib::Server server;
    server.Get("/health", [](const httplib::Request&, httplib::Response& response) {
        response.set_content("ok", "text/plain");
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    ASSERT_GT(port, 0);
    configure_http_server(server, {}, "0.0.0.0", port);
    std::thread listener([&server] { server.listen_after_bind(); });
    server.wait_until_ready();

    httplib::Client client("127.0.0.1", port);
    const std::vector<std::string> allowed{
        "127.0.0.1:" + std::to_string(port),
        "192.168.1.20:" + std::to_string(port),
        "localhost:" + std::to_string(port),
        "[::1]:" + std::to_string(port)};
    for (const std::string& host : allowed) {
        const auto result = client.Get(
            "/health", httplib::Headers{{"Host", host}});
        if (!result) {
            ADD_FAILURE() << host;
            continue;
        }
        EXPECT_EQ(result->status, 200) << host;
    }
    const std::vector<std::string> rejected{
        "evil.example:" + std::to_string(port),
        "127.0.0.1:" + std::to_string(port + 1)};
    for (const std::string& host : rejected) {
        const auto result = client.Get(
            "/health", httplib::Headers{{"Host", host}});
        if (!result) {
            ADD_FAILURE() << host;
            continue;
        }
        EXPECT_EQ(result->status, 403) << host;
    }

    server.stop();
    listener.join();
}

} // namespace
} // namespace cha::web
