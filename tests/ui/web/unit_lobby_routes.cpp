#include "ui/web/asset_handler.h"
#include "ui/web/http_server.h"
#include "ui/web/lobby_routes.h"
#include "ui/web/session_registry.h"
#include "support/test_workspace.h"

#include "session/session_controller.h"
#include "session/session_lease.h"
#include "session/workspace.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>

namespace cha::web {
namespace {

using namespace std::chrono_literals;

class IdleController final : public WebSessionController {
public:
    SessionUpdate handle_raw_input(std::string) override { return {}; }
    SessionUpdate request_stop() override { return {}; }
    SessionUpdate set_default_agent_id(std::string_view) override { return {}; }
    SessionEventBatch receive(std::size_t) override { return {}; }
    [[nodiscard]] bool is_generating() const override { return false; }
    void shutdown() override {}
};

class TestServer {
public:
    using Installer = std::function<void(httplib::Server&)>;

    TestServer(
        std::shared_ptr<const Workspace> workspace,
        SessionRegistry& registry,
        WebSettings settings = {.open_deadline = 500ms},
        Installer installer = {}) {
        AssetHandler().install(server_);
        LobbyRoutes(workspace, registry, settings).install(server_);
        if (installer) installer(server_);
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ < 0) throw std::runtime_error("Could not bind test server");
        configure_http_server(server_, settings, "127.0.0.1", port_);
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }
    ~TestServer() {
        server_.stop();
        thread_.join();
    }
    httplib::Client client() const {
        httplib::Client client("127.0.0.1", port_);
        client.set_keep_alive(false);
        return client;
    }
    int port() const noexcept { return port_; }

private:
    httplib::Server server_;
    int port_{};
    std::thread thread_;
};

nlohmann::json body(const httplib::Result& result) {
    if (!result) {
        ADD_FAILURE() << "HTTP request failed with error " << static_cast<int>(result.error());
        return {};
    }
    return nlohmann::json::parse(result->body);
}

void expect_error(
    const httplib::Result& result,
    int status,
    std::string_view code) {
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, status);
    const nlohmann::json json = body(result);
    ASSERT_TRUE(json.contains("error"));
    ASSERT_TRUE(json["error"].is_object());
    EXPECT_EQ(json["error"].size(), 2);
    EXPECT_EQ(json["error"]["code"], code);
    EXPECT_TRUE(json["error"]["code"].is_string());
    EXPECT_TRUE(json["error"]["message"].is_string());
}

std::string create_session(TestServer& server, std::string_view label = "Notes") {
    const auto created = server.client().Post(
        "/api/v1/forums/lobby/sessions",
        std::string(R"({"label":")") + std::string(label) + R"("})",
        "application/json");
    if (!created) {
        ADD_FAILURE() << "Create request failed with error " << static_cast<int>(created.error());
        return {};
    }
    EXPECT_EQ(created->status, 201);
    const nlohmann::json json = body(created);
    EXPECT_FALSE(json.contains("error"));
    return json.at("id").get<std::string>();
}

bool listed_not_live(TestServer& server, std::string_view id) {
    const auto listed = server.client().Get("/api/v1/forums/lobby/sessions");
    if (!listed || listed->status != 200) return false;
    for (const auto& session : body(listed)) {
        if (session["id"] == id) return !session["live"].get<bool>();
    }
    return false;
}

WebSessionMetadata load_workspace_metadata(
    const Workspace& workspace,
    const SessionKey& key) {
    const Forum forum = workspace.load_forum(key.forum);
    const SessionSummary session = workspace.session_summary(
        key.forum, key.session_id);
    return {
        .forum = {forum.name, forum.display_name},
        .session_id = session.id,
        .session_label = session.label,
    };
}

TEST(LobbyRoutes, ServesPlaceholderHealthAndForumListingWithoutSessionDataInHealth) {
    test::TestWorkspace fixture;
    auto workspace = std::make_shared<const Workspace>(fixture.root());
    SessionRegistry registry({.session_limit = 2}, [](const SessionKey&, WakeNotifier&) {
        return std::make_unique<IdleController>();
    });
    TestServer server(workspace, registry);
    const auto root = server.client().Get("/");
    ASSERT_TRUE(root);
    EXPECT_EQ(root->status, 200);
    EXPECT_EQ(root->get_header_value("Content-Type"), "text/html; charset=utf-8");

    const auto health = server.client().Get("/health");
    ASSERT_TRUE(health);
    EXPECT_EQ(health->status, 200);
    EXPECT_EQ(body(health), nlohmann::json({{"ready", true}, {"live_session_count", 0}}));

    const auto forums = server.client().Get("/api/v1/forums");
    ASSERT_TRUE(forums);
    EXPECT_EQ(forums->status, 200);
    EXPECT_EQ(body(forums), nlohmann::json::array({{{"id", "lobby"}, {"display_name", "The Lobby"}}}));
}

TEST(LobbyRoutes, SkipsMalformedForumsAndAllowsAnEmptyForumDirectory) {
    {
        test::TestWorkspace fixture;
        std::filesystem::create_directories(
            fixture.root() / "forums" / "broken" / "personas");
        std::ofstream(fixture.root() / "forums" / "broken" / "config.toml")
            << "display_name = \"Broken\"\n";
        std::filesystem::create_directories(
            fixture.root() / "forums" / "bad-config" / "personas" / "guide");
        std::ofstream(
            fixture.root() / "forums" / "bad-config" / "config.toml")
            << "display_name = [not valid TOML";
        auto workspace = std::make_shared<const Workspace>(fixture.root());
        SessionRegistry registry(
            {.session_limit = 2},
            [](const SessionKey&, WakeNotifier&) {
                return std::make_unique<IdleController>();
            });
        TestServer server(workspace, registry);

        const auto forums = server.client().Get("/api/v1/forums");
        ASSERT_TRUE(forums);
        EXPECT_EQ(forums->status, 200);
        EXPECT_EQ(
            body(forums),
            nlohmann::json::array({
                {{"id", "lobby"}, {"display_name", "The Lobby"}},
            }));
        expect_error(
            server.client().Post(
                "/api/v1/forums/broken/sessions",
                R"({"label":"Rejected"})",
                "application/json"),
            500,
            "internal_error");
    }

    {
        test::TestWorkspace fixture;
        std::filesystem::remove_all(fixture.root() / "forums" / "lobby");
        auto workspace = std::make_shared<const Workspace>(fixture.root());
        SessionRegistry registry(
            {.session_limit = 2},
            [](const SessionKey&, WakeNotifier&) {
                return std::make_unique<IdleController>();
            });
        TestServer server(workspace, registry);

        const auto forums = server.client().Get("/api/v1/forums");
        ASSERT_TRUE(forums);
        EXPECT_EQ(forums->status, 200);
        EXPECT_EQ(body(forums), nlohmann::json::array());
    }
}

TEST(LobbyRoutes, CreateIsSeparateFromOpenAndListingsMarkOnlyRunningSessions) {
    test::TestWorkspace fixture;
    auto workspace = std::make_shared<const Workspace>(fixture.root());
    std::atomic<int> starts{};
    SessionRegistry registry({.session_limit = 2}, [&starts](const SessionKey&, WakeNotifier&) {
        ++starts;
        return std::make_unique<IdleController>();
    });
    TestServer server(workspace, registry);
    const auto created = server.client().Post("/api/v1/forums/lobby/sessions", R"({"label":"Notes"})", "application/json");
    ASSERT_TRUE(created);
    ASSERT_EQ(created->status, 201);
    const nlohmann::json created_body = body(created);
    EXPECT_EQ(created_body.size(), 2);
    EXPECT_EQ(created_body["label"], "Notes");
    const std::string id = created_body["id"];
    EXPECT_EQ(starts, 0);
    EXPECT_EQ(registry.snapshot().live_entry_count, 0);

    const auto listed = server.client().Get("/api/v1/forums/lobby/sessions");
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed->status, 200);
    ASSERT_EQ(body(listed).size(), 1);
    EXPECT_FALSE(body(listed)[0]["live"]);

    const auto opened = server.client().Post("/api/v1/forums/lobby/sessions/" + id + "/open", "{}", "application/json");
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened->status, 200);
    EXPECT_EQ(body(opened), nlohmann::json({{"path", "/s/lobby/" + id + "/"}}));
    EXPECT_EQ(starts, 1);

    const auto reopened = server.client().Post("/api/v1/forums/lobby/sessions/" + id + "/open", "{}", "application/json");
    ASSERT_TRUE(reopened);
    EXPECT_EQ(reopened->status, 200);
    EXPECT_EQ(starts, 1);

    const auto live_listed = server.client().Get("/api/v1/forums/lobby/sessions");
    ASSERT_TRUE(live_listed);
    EXPECT_TRUE(body(live_listed)[0]["live"]);
    registry.begin_shutdown();
}

TEST(LobbyRoutes, UsesCommonErrorsAndRejectsInvalidMutationInputsBeforeRegistry) {
    test::TestWorkspace fixture;
    auto workspace = std::make_shared<const Workspace>(fixture.root());
    std::atomic<int> starts{};
    SessionRegistry registry({.session_limit = 1}, [&starts](const SessionKey&, WakeNotifier&) {
        ++starts;
        return std::make_unique<IdleController>();
    });
    TestServer server(workspace, registry);
    const auto invalid = server.client().Post("/api/v1/forums/%2e%2e/sessions/nope/open", "{}", "application/json");
    expect_error(invalid, 404, "not_found");
    const auto backslash = server.client().Post(
        "/api/v1/forums/lobby%5cother/sessions/nope/open",
        "{}",
        "application/json");
    expect_error(backslash, 404, "not_found");
    const auto fragment = server.client().Post(
        "/api/v1/forums/lobby%23other/sessions/nope/open",
        "{}",
        "application/json");
    expect_error(fragment, 404, "not_found");
    expect_error(
        server.client().Get("/api/v1/forums/lobby%00other/sessions"),
        404,
        "not_found");
    EXPECT_EQ(starts, 0);

    const auto wrong_type = server.client().Post("/api/v1/forums/lobby/sessions", "{}", "text/plain");
    expect_error(wrong_type, 400, "bad_request");

    httplib::Headers headers{
        {"Content-Type", "application/json"},
        {"Origin", "http://other.example"},
    };
    const auto foreign = server.client().Post("/api/v1/forums/lobby/sessions", headers, R"({"label":"x"})", "application/json");
    expect_error(foreign, 403, "forbidden_origin");
    EXPECT_EQ(starts, 0);
}

TEST(LobbyRoutes, AcceptsConfiguredAndLoopbackAliasOrigins) {
    test::TestWorkspace fixture;
    auto workspace = std::make_shared<const Workspace>(fixture.root());
    SessionRegistry registry({.session_limit = 2}, [](const SessionKey&, WakeNotifier&) {
        return std::make_unique<IdleController>();
    });
    TestServer server(workspace, registry);

    const auto loopback = server.client().Post(
        "/api/v1/forums/lobby/sessions",
        httplib::Headers{{
            "Origin",
            "http://127.0.0.1:" + std::to_string(server.port()),
        }},
        R"({"label":"Loopback"})",
        "application/json");
    ASSERT_TRUE(loopback);
    EXPECT_EQ(loopback->status, 201);

    const auto localhost = server.client().Post(
        "/api/v1/forums/lobby/sessions",
        httplib::Headers{
            {"Host", "localhost:" + std::to_string(server.port())},
            {"Origin", "http://localhost:" + std::to_string(server.port())},
        },
        R"({"label":"localhost"})",
        "application/json");
    ASSERT_TRUE(localhost);
    EXPECT_EQ(localhost->status, 201);
}

TEST(LobbyRoutes, RejectsDnsRebindingHostOnReadsAndMutations) {
    test::TestWorkspace fixture;
    auto workspace = std::make_shared<const Workspace>(fixture.root());
    SessionRegistry registry({.session_limit = 2}, [](const SessionKey&, WakeNotifier&) {
        return std::make_unique<IdleController>();
    });
    TestServer server(workspace, registry);
    const httplib::Headers rebound{
        {"Host", "evil.example:" + std::to_string(server.port())},
        {"Origin", "http://evil.example:" + std::to_string(server.port())},
    };

    expect_error(
        server.client().Get("/api/v1/forums", rebound),
        403,
        "forbidden_host");
    const int other_port = server.port() == 65535
        ? server.port() - 1
        : server.port() + 1;
    expect_error(
        server.client().Get(
            "/api/v1/forums",
            httplib::Headers{{
                "Host",
                "127.0.0.1:" + std::to_string(other_port),
            }}),
        403,
        "forbidden_host");
    expect_error(
        server.client().Get(
            "/api/v1/forums",
            httplib::Headers{
                {"Host", "127.0.0.1:" + std::to_string(server.port())},
                {"Host", "evil.example:" + std::to_string(server.port())},
            }),
        403,
        "forbidden_host");
    expect_error(
        server.client().Post(
            "/api/v1/forums/lobby/sessions",
            rebound,
            R"({"label":"rebound"})",
            "application/json"),
        403,
        "forbidden_host");
    EXPECT_TRUE(workspace->sessions("lobby").empty());
}

TEST(LobbyRoutes, ValidatesMissingIdentifiersMethodsAndOriginWithoutOrigin) {
    test::TestWorkspace fixture;
    auto workspace = std::make_shared<const Workspace>(fixture.root());
    SessionRegistry registry({.session_limit = 2}, [](const SessionKey&, WakeNotifier&) {
        return std::make_unique<IdleController>();
    });
    TestServer server(workspace, registry);

    expect_error(server.client().Get("/api/v1/forums/missing/sessions"), 404, "not_found");
    expect_error(
        server.client().Post(
            "/api/v1/forums/missing/sessions",
            "garbage",
            "application/json"),
        400,
        "bad_request");
    expect_error(
        server.client().Post(
            "/api/v1/forums/missing/sessions",
            R"({"label":"Missing"})",
            "application/json"),
        404,
        "not_found");
    expect_error(
        server.client().Post(
            "/api/v1/forums/lobby/sessions/missing/open", "{}", "application/json"),
        404,
        "not_found");
    expect_error(server.client().Get("/api/v1/forums/lobby/sessions/nope/open"), 404, "not_found");

    const std::string id = create_session(server, "No origin");
    EXPECT_TRUE(listed_not_live(server, id));
    registry.begin_shutdown();
}

TEST(LobbyRoutes, ReportsSessionListingStorageFailuresAsInternalErrors) {
    test::TestWorkspace fixture;
    std::ofstream(fixture.root() / "forums" / "lobby" / "sessions")
        << "not a directory";
    auto workspace = std::make_shared<const Workspace>(fixture.root());
    SessionRegistry registry({.session_limit = 2}, [](const SessionKey&, WakeNotifier&) {
        return std::make_unique<IdleController>();
    });
    TestServer server(workspace, registry);

    expect_error(
        server.client().Get("/api/v1/forums/lobby/sessions"),
        500,
        "internal_error");
    expect_error(
        server.client().Get("/api/v1/forums/missing/sessions"),
        404,
        "not_found");
}

TEST(LobbyRoutes, ReportsCorruptSessionMetadataAsInternalError) {
    test::TestWorkspace fixture;
    auto workspace = std::make_shared<const Workspace>(fixture.root());
    std::atomic<int> starts{};
    SessionRegistry registry({.session_limit = 2}, [&starts](const SessionKey&, WakeNotifier&) {
        ++starts;
        return std::make_unique<IdleController>();
    });
    TestServer server(workspace, registry);
    const std::string id = create_session(server, "Corrupt");
    std::ofstream(
        fixture.root() / "forums" / "lobby" / "sessions"
            / (id + ".sqlite3"),
        std::ios::binary | std::ios::trunc)
        << "not SQLite";

    expect_error(
        server.client().Post(
            "/api/v1/forums/lobby/sessions/" + id + "/open",
            "{}",
            "application/json"),
        500,
        "internal_error");
    EXPECT_EQ(starts, 0);
}

TEST(LobbyRoutes, ReattachesFromRegistryWithoutReadingStorageAgain) {
    test::TestWorkspace fixture;
    auto workspace = std::make_shared<const Workspace>(fixture.root());
    std::atomic<int> starts{};
    SessionRegistry registry({.session_limit = 2}, [&starts](const SessionKey&, WakeNotifier&) {
        ++starts;
        return std::make_unique<IdleController>();
    });
    TestServer server(workspace, registry);
    const std::string id = create_session(server, "Live");
    const std::string route =
        "/api/v1/forums/lobby/sessions/" + id + "/open";
    const auto opened =
        server.client().Post(route, "{}", "application/json");
    ASSERT_TRUE(opened);
    ASSERT_EQ(opened->status, 200);
    ASSERT_TRUE(std::filesystem::remove(
        fixture.root() / "forums" / "lobby" / "sessions"
            / (id + ".sqlite3")));

    const auto reattached =
        server.client().Post(route, "{}", "application/json");
    ASSERT_TRUE(reattached);
    EXPECT_EQ(reattached->status, 200);
    EXPECT_EQ(
        body(reattached),
        nlohmann::json({{"path", "/s/lobby/" + id + "/"}}));
    EXPECT_EQ(starts, 1);
    registry.begin_shutdown();
}

TEST(LobbyRoutes, DeletedSessionBetweenValidationAndOpenReturnsNotFoundAndReleasesCapacity) {
    test::TestWorkspace fixture;
    auto workspace = std::make_shared<const Workspace>(fixture.root());
    const SessionSummary deleted =
        workspace->create_stored_session("lobby", "Deleted");
    const SessionSummary survivor =
        workspace->create_stored_session("lobby", "Survivor");
    const std::filesystem::path deleted_database =
        fixture.root() / "forums" / "lobby" / "sessions"
        / (deleted.id + ".sqlite3");
    const WebSettings settings{.session_limit = 1, .open_deadline = 500ms};
    SessionRegistry registry(
        settings,
        [workspace, deleted, deleted_database](
            const SessionKey& key,
            WakeNotifier& notifier) {
            if (key.session_id == deleted.id
                && !std::filesystem::remove(deleted_database)) {
                throw std::runtime_error("Test session was not deleted");
            }
            return adapt_session_controller(workspace->open_session(
                key.forum, key.session_id, notifier));
        },
        [workspace](const SessionKey& key) {
            return load_workspace_metadata(*workspace, key);
        });
    TestServer server(workspace, registry, settings);

    expect_error(
        server.client().Post(
            "/api/v1/forums/lobby/sessions/" + deleted.id + "/open",
            "{}",
            "application/json"),
        404,
        "not_found");
    EXPECT_EQ(registry.snapshot().live_entry_count, 0U);

    const auto opened = server.client().Post(
        "/api/v1/forums/lobby/sessions/" + survivor.id + "/open",
        "{}",
        "application/json");
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened->status, 200);
    registry.begin_shutdown();
}

TEST(LobbyRoutes, DeletedForumBetweenValidationAndOpenReturnsNotFound) {
    test::TestWorkspace fixture;
    auto workspace = std::make_shared<const Workspace>(fixture.root());
    const SessionSummary stored =
        workspace->create_stored_session("lobby", "Deleted forum");
    const std::filesystem::path forum_directory =
        fixture.root() / "forums" / "lobby";
    const WebSettings settings{.session_limit = 1, .open_deadline = 500ms};
    SessionRegistry registry(
        settings,
        [workspace, forum_directory](
            const SessionKey& key,
            WakeNotifier& notifier) {
            if (std::filesystem::remove_all(forum_directory) == 0) {
                throw std::runtime_error("Test forum was not deleted");
            }
            return adapt_session_controller(workspace->open_session(
                key.forum, key.session_id, notifier));
        },
        [workspace](const SessionKey& key) {
            return load_workspace_metadata(*workspace, key);
        });
    TestServer server(workspace, registry, settings);

    expect_error(
        server.client().Post(
            "/api/v1/forums/lobby/sessions/" + stored.id + "/open",
            "{}",
            "application/json"),
        404,
        "not_found");
    EXPECT_EQ(registry.snapshot().live_entry_count, 0U);
}

TEST(LobbyRoutes, ValidatesOpenBodyAndAppliesTheRequestLimit) {
    test::TestWorkspace fixture;
    auto workspace = std::make_shared<const Workspace>(fixture.root());
    std::atomic<int> starts{};
    SessionRegistry registry({.session_limit = 2}, [&starts](const SessionKey&, WakeNotifier&) {
        ++starts;
        return std::make_unique<IdleController>();
    });
    WebSettings settings{.open_deadline = 500ms, .request_body_limit = 64};
    TestServer server(workspace, registry, settings);
    const std::string id = create_session(server, "Body validation");
    const std::string route =
        "/api/v1/forums/lobby/sessions/" + id + "/open";

    expect_error(
        server.client().Post(route, "garbage", "application/json"),
        400,
        "bad_request");
    expect_error(
        server.client().Post(
            route,
            R"({"unexpected":true})",
            "application/json"),
        400,
        "bad_request");
    expect_error(
        server.client().Post(
            route,
            std::string(65, 'x'),
            "application/json"),
        413,
        "body_too_large");
    EXPECT_EQ(starts, 0);

    const auto opened =
        server.client().Post(route, "{}", "application/json");
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened->status, 200);
    EXPECT_EQ(starts, 1);
    registry.begin_shutdown();
}

TEST(LobbyRoutes, WrapsGeneratedAndUnhandledErrorsInTheCommonEnvelope) {
    test::TestWorkspace fixture;
    auto workspace = std::make_shared<const Workspace>(fixture.root());
    SessionRegistry registry({.session_limit = 2}, [](const SessionKey&, WakeNotifier&) {
        return std::make_unique<IdleController>();
    });
    TestServer server(
        workspace,
        registry,
        {.open_deadline = 500ms},
        [](httplib::Server& http) {
            http.Get(
                "/throws",
                [](const httplib::Request&, httplib::Response&) {
                    throw std::runtime_error("test route failure");
                });
        });

    expect_error(
        server.client().Get(
            "/api/v1/forums",
            httplib::Headers{{"Accept", "not-a-media-type"}}),
        400,
        "bad_request");
    expect_error(
        server.client().Get("/" + std::string(9000, 'x')),
        414,
        "bad_request");
    expect_error(
        server.client().Get(
            "/api/v1/forums",
            httplib::Headers{{"Range", "not-a-range"}}),
        416,
        "bad_request");
    expect_error(
        server.client().Get(
            "/api/v1/forums",
            httplib::Headers{{"X-Oversized", std::string(9000, 'x')}}),
        400,
        "bad_request");
    expect_error(server.client().Get("/throws"), 500, "internal_error");

    const auto unknown = server.client().Get("/nope");
    expect_error(unknown, 404, "not_found");
    EXPECT_EQ(
        body(unknown)["error"]["message"],
        "The requested resource was not found.");
}

TEST(LobbyRoutes, CreatedSessionSurvivesBusyOpenAndCanBeRetried) {
    test::TestWorkspace fixture;
    auto workspace = std::make_shared<const Workspace>(fixture.root());
    std::atomic<bool> busy{true};
    SessionRegistry registry({.session_limit = 2}, [&busy](const SessionKey&, WakeNotifier&) {
        if (busy) throw SessionBusyError("held by test");
        return std::make_unique<IdleController>();
    });
    TestServer server(workspace, registry);
    const std::string id = create_session(server);
    const std::string route = "/api/v1/forums/lobby/sessions/" + id + "/open";

    expect_error(server.client().Post(route, "{}", "application/json"), 409, "session_busy");
    EXPECT_TRUE(listed_not_live(server, id));

    busy = false;
    const auto opened = server.client().Post(route, "{}", "application/json");
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened->status, 200);
    EXPECT_EQ(body(opened), nlohmann::json({{"path", "/s/lobby/" + id + "/"}}));
    registry.begin_shutdown();
}

TEST(LobbyRoutes, CreateSucceedsWhileAnotherStoredSessionLeaseIsHeld) {
    test::TestWorkspace fixture;
    auto workspace = std::make_shared<const Workspace>(fixture.root());
    const WebSettings settings{.session_limit = 1, .open_deadline = 500ms};
    SessionRegistry registry =
        SessionRegistry::from_workspace(settings, workspace);
    TestServer server(workspace, registry, settings);
    const std::string leased_id = create_session(server, "Externally held");
    SessionLease held = SessionLease::acquire(
        fixture.root() / "forums" / "lobby" / "sessions"
            / (leased_id + ".sqlite3"));
    (void)held;

    const auto created = server.client().Post(
        "/api/v1/forums/lobby/sessions",
        R"({"label":"Created while busy"})",
        "application/json");
    ASSERT_TRUE(created);
    EXPECT_EQ(created->status, 201);
    EXPECT_FALSE(body(created).contains("error"));

    expect_error(
        server.client().Post(
            "/api/v1/forums/lobby/sessions/" + leased_id + "/open",
            "{}",
            "application/json"),
        409,
        "session_busy");
    registry.begin_shutdown();
}

TEST(LobbyRoutes, CreatedSessionSurvivesInitializationFailureAndCanBeRetried) {
    test::TestWorkspace fixture;
    auto workspace = std::make_shared<const Workspace>(fixture.root());
    std::atomic<bool> fail{true};
    SessionRegistry registry({.session_limit = 2}, [&fail](const SessionKey&, WakeNotifier&) {
        if (fail) throw std::runtime_error("initialization failed");
        return std::make_unique<IdleController>();
    });
    TestServer server(workspace, registry);
    const std::string id = create_session(server);
    const std::string route = "/api/v1/forums/lobby/sessions/" + id + "/open";

    expect_error(server.client().Post(route, "{}", "application/json"), 500, "internal_error");
    EXPECT_TRUE(listed_not_live(server, id));

    fail = false;
    const auto opened = server.client().Post(route, "{}", "application/json");
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened->status, 200);
    registry.begin_shutdown();
}

TEST(LobbyRoutes, CreatedSessionSurvivesLimitOpenAndCanBeRetried) {
    test::TestWorkspace fixture;
    auto workspace = std::make_shared<const Workspace>(fixture.root());
    SessionRegistry registry({.session_limit = 1}, [](const SessionKey&, WakeNotifier&) {
        return std::make_unique<IdleController>();
    });
    TestServer server(workspace, registry);
    const std::string occupied = create_session(server, "Occupied");
    const std::string occupied_route = "/api/v1/forums/lobby/sessions/" + occupied + "/open";
    const auto occupied_open =
        server.client().Post(occupied_route, "{}", "application/json");
    ASSERT_TRUE(occupied_open);
    ASSERT_EQ(occupied_open->status, 200);

    // Creation does not consult the full registry and therefore cannot return
    // an open-lifecycle error even while the live-session limit is consumed.
    const std::string id = create_session(server, "Retry later");
    const std::string route = "/api/v1/forums/lobby/sessions/" + id + "/open";

    expect_error(server.client().Post(route, "{}", "application/json"), 503, "session_limit_reached");
    EXPECT_TRUE(listed_not_live(server, id));

    SessionHandle occupied_handle = registry.lookup({"lobby", occupied});
    ASSERT_TRUE(occupied_handle);
    occupied_handle.runtime().request_shutdown();
    httplib::Result opened;
    for (int attempt = 0; attempt != 50; ++attempt) {
        opened = server.client().Post(route, "{}", "application/json");
        if (opened && opened->status == 200) break;
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened->status, 200);
    registry.begin_shutdown();
}

TEST(LobbyRoutes, OpenTimeoutUsesTheLobbyErrorEnvelope) {
    test::TestWorkspace fixture;
    auto workspace = std::make_shared<const Workspace>(fixture.root());
    SessionRegistry registry({.session_limit = 2}, [](const SessionKey&, WakeNotifier&) {
        std::this_thread::sleep_for(30ms);
        return std::make_unique<IdleController>();
    });
    TestServer server(workspace, registry, {.open_deadline = 1ms});
    const std::string id = create_session(server);
    const std::string route = "/api/v1/forums/lobby/sessions/" + id + "/open";

    expect_error(server.client().Post(route, "{}", "application/json"), 503, "session_open_timeout");
    EXPECT_TRUE(listed_not_live(server, id));

    std::this_thread::sleep_for(40ms);
    const auto opened = server.client().Post(route, "{}", "application/json");
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened->status, 200);
    registry.begin_shutdown();
}

} // namespace
} // namespace cha::web
