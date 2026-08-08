#include "ui/web/asset_handler.h"
#include "ui/web/http_server.h"
#include "ui/web/lobby_routes.h"
#include "ui/web/live_session_manager.h"
#include "support/test_live_session.h"
#include "support/test_web_graph.h"
#include "support/test_workspace.h"

#include "application/builtins.h"

#include "session/session_controller.h"
#include "session/session_lease.h"
#include "session/session_repository.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace cha::web {
namespace {

using namespace std::chrono_literals;

using LobbyGraph = test::WebGraph;

// Lobby routes only need one real opener; every session behind them is a real
// SessionController over the fixture workspace's own storage.
SessionOpener counting_opener(
    const LobbyGraph& graph,
    std::atomic<int>* starts = nullptr) {
    return [open = graph.opener(), starts](
               const SessionIdentity& key, WakeNotifier& notifier) {
        if (starts) ++*starts;
        return open(key, notifier);
    };
}

WebSettings lobby_settings(std::size_t session_limit = 2) {
    WebSettings settings;
    settings.session_limit = session_limit;
    settings.open_deadline = 5s;
    return settings;
}

class TestServer {
public:
    using Installer = std::function<void(httplib::Server&)>;

    TestServer(
        const LobbyGraph& graph,
        LiveSessionManager& live_sessions,
        WebSettings settings = lobby_settings(),
        Installer installer = {}) {
        AssetHandler(graph.root() / "web").install(server_);
        LobbyRoutes(
            graph.model, graph.sessions, LobbyGraph::initial_selection(),
            live_sessions, settings).install(server_);
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
    std::string_view code,
    std::string_view message = {}) {
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, status);
    const nlohmann::json json = body(result);
    ASSERT_TRUE(json.contains("error"));
    ASSERT_TRUE(json["error"].is_object());
    EXPECT_EQ(json["error"].size(), 2);
    EXPECT_EQ(json["error"]["code"], code);
    EXPECT_TRUE(json["error"]["code"].is_string());
    EXPECT_TRUE(json["error"]["message"].is_string());
    if (!message.empty()) {
        EXPECT_EQ(json["error"]["message"], message);
    }
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

TEST(LobbyRoutes, ServesBootstrapDiscoveryAndHealthWithoutSessionDataInHealth) {
    test::TestWorkspace fixture;
    fixture.write_character_config(
        "display_name = \"Guide\"\n"
        "description = \"Explains the workspace\"\n");
    const LobbyGraph graph(fixture.root());
    LiveSessionManager manager(lobby_settings(2), counting_opener(graph));
    TestServer server(graph, manager);
    const auto root = server.client().Get("/");
    ASSERT_TRUE(root);
    EXPECT_EQ(root->status, 200);
    EXPECT_EQ(root->get_header_value("Content-Type"), "text/html; charset=utf-8");
    EXPECT_NE(root->body.find("test shell"), std::string::npos);

    const auto health = server.client().Get("/health");
    ASSERT_TRUE(health);
    EXPECT_EQ(health->status, 200);
    EXPECT_EQ(body(health), nlohmann::json({{"ready", true}, {"live_session_count", 0}}));

    const auto bootstrap = server.client().Get("/api/v1/bootstrap");
    ASSERT_TRUE(bootstrap);
    EXPECT_EQ(bootstrap->status, 200);
    const nlohmann::json bootstrap_body = body(bootstrap);
    EXPECT_EQ(bootstrap_body["initial_persona_id"], "builtin-guest");
    EXPECT_EQ(bootstrap_body["initial_forum_id"], "builtin-entrance");
    EXPECT_EQ(bootstrap_body["initial_session_id"], "builtin-welcome");
    EXPECT_EQ(bootstrap_body["forums"].size(), 2);
    EXPECT_EQ(bootstrap_body["personas"].size(), 2);
    EXPECT_EQ(bootstrap_body["characters"].size(), 2);
    const auto guide = std::find_if(
        bootstrap_body["characters"].begin(), bootstrap_body["characters"].end(),
        [](const nlohmann::json& character) { return character["id"] == "guide"; });
    ASSERT_NE(guide, bootstrap_body["characters"].end());
    EXPECT_EQ((*guide)["description"], "Explains the workspace");
    EXPECT_TRUE(bootstrap_body["recent_sessions"].is_array());
    // Built-ins take their place in display-name order rather than trailing it.
    EXPECT_EQ(bootstrap_body["characters"][0]["id"], "builtin-assistant");
    EXPECT_EQ(bootstrap_body["characters"][1]["id"], "guide");
    EXPECT_EQ(bootstrap_body["forums"][0]["id"], "builtin-entrance");
    EXPECT_EQ(bootstrap_body["forums"][1]["id"], "lobby");
    const nlohmann::json forums = bootstrap_body["forums"];
    const auto entrance = std::find_if(forums.begin(), forums.end(), [](const nlohmann::json& forum) {
        return forum["id"] == "builtin-entrance";
    });
    ASSERT_NE(entrance, forums.end());
    EXPECT_EQ((*entrance)["default_character_id"], "builtin-assistant");
    EXPECT_EQ((*entrance)["members"], nlohmann::json::array({{
        {"id", "builtin-assistant"}, {"display_name", "Assistant"},
        {"appearance", {{"font", "sans"}, {"style", "normal"},
            {"weight", "normal"}, {"size", "normal"}}},
    }}));

    const auto workspace_character = server.client().Get("/api/v1/characters/guide");
    ASSERT_TRUE(workspace_character);
    ASSERT_EQ(workspace_character->status, 200);
    const nlohmann::json workspace_character_body = body(workspace_character);
    EXPECT_EQ(workspace_character_body["character_markdown"], "Character instructions\n");
    EXPECT_EQ(workspace_character_body["appearance"], (*guide)["appearance"]);

    const auto assistant_character = server.client().Get("/api/v1/characters/builtin-assistant");
    ASSERT_TRUE(assistant_character);
    ASSERT_EQ(assistant_character->status, 200);
    EXPECT_FALSE(body(assistant_character)["character_markdown"].get<std::string>().empty());

    const auto entrance_sessions = server.client().Get("/api/v1/forums/builtin-entrance/sessions");
    ASSERT_TRUE(entrance_sessions);
    ASSERT_EQ(entrance_sessions->status, 200);
    const nlohmann::json sessions = body(entrance_sessions);
    ASSERT_EQ(sessions.size(), 1);
    EXPECT_EQ(sessions[0]["id"], "builtin-welcome");
    EXPECT_EQ(sessions[0]["label"], "Welcome");
    EXPECT_FALSE(sessions[0]["live"]);
    EXPECT_GT(sessions[0]["updated_at"].get<std::int64_t>(), 0);
}

TEST(LobbyRoutes, CreateIsSeparateFromOpenAndListingsMarkOnlyRunningSessions) {
    test::TestWorkspace fixture;
    const LobbyGraph graph(fixture.root());
    std::atomic<int> starts{};
    LiveSessionManager manager(lobby_settings(2), counting_opener(graph, &starts));
    TestServer server(graph, manager);
    const auto created = server.client().Post("/api/v1/forums/lobby/sessions", R"({"label":"Notes"})", "application/json");
    ASSERT_TRUE(created);
    ASSERT_EQ(created->status, 201);
    const nlohmann::json created_body = body(created);
    EXPECT_EQ(created_body.size(), 2);
    EXPECT_EQ(created_body["label"], "Notes");
    const std::string id = created_body["id"];
    EXPECT_EQ(starts, 0);
    EXPECT_EQ(manager.snapshot().live_session_count, 0);

    const auto listed = server.client().Get("/api/v1/forums/lobby/sessions");
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed->status, 200);
    ASSERT_EQ(body(listed).size(), 1);
    EXPECT_FALSE(body(listed)[0]["live"]);
    EXPECT_GT(body(listed)[0]["updated_at"].get<std::int64_t>(), 0);

    const auto bootstrap = server.client().Get("/api/v1/bootstrap");
    ASSERT_TRUE(bootstrap);
    ASSERT_EQ(bootstrap->status, 200);
    const nlohmann::json recent_sessions = body(bootstrap)["recent_sessions"];
    ASSERT_EQ(recent_sessions.size(), 2);
    EXPECT_GE(recent_sessions[0]["updated_at"], recent_sessions[1]["updated_at"]);
    EXPECT_TRUE(std::any_of(recent_sessions.begin(), recent_sessions.end(), [&id](const nlohmann::json& recent) {
        return recent["forum_id"] == "lobby" && recent["session_id"] == id
            && recent["session_label"] == "Notes";
    }));

    const auto opened = server.client().Post("/api/v1/forums/lobby/sessions/" + id + "/open", "{}", "application/json");
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened->status, 200);
    EXPECT_EQ(body(opened), nlohmann::json({{"forum_id", "lobby"}, {"session_id", id}}));
    EXPECT_EQ(starts, 1);

    const auto reopened = server.client().Post("/api/v1/forums/lobby/sessions/" + id + "/open", "{}", "application/json");
    ASSERT_TRUE(reopened);
    EXPECT_EQ(reopened->status, 200);
    EXPECT_EQ(starts, 1);

    const auto live_listed = server.client().Get("/api/v1/forums/lobby/sessions");
    ASSERT_TRUE(live_listed);
    EXPECT_TRUE(body(live_listed)[0]["live"]);
    manager.begin_shutdown();
}

TEST(LobbyRoutes, KeyBasedCreationAllowsDuplicateLabels) {
    test::TestWorkspace fixture;
    const LobbyGraph graph(fixture.root());
    LiveSessionManager manager(lobby_settings(2), counting_opener(graph));
    TestServer server(graph, manager);

    const std::string first = create_session(server, "Repeated");
    const std::string second = create_session(server, "Repeated");
    EXPECT_NE(first, second);
    const auto listed = server.client().Get("/api/v1/forums/lobby/sessions");
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed->status, 200);
    ASSERT_EQ(body(listed).size(), 2U);
    EXPECT_EQ(body(listed)[0]["label"], "Repeated");
    EXPECT_EQ(body(listed)[1]["label"], "Repeated");
}

TEST(LobbyRoutes, UsesCommonErrorsAndRejectsInvalidMutationInputsBeforeOpening) {
    test::TestWorkspace fixture;
    const LobbyGraph graph(fixture.root());
    std::atomic<int> starts{};
    LiveSessionManager manager(lobby_settings(1), counting_opener(graph, &starts));
    TestServer server(graph, manager);
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
    const LobbyGraph graph(fixture.root());
    LiveSessionManager manager(lobby_settings(2), counting_opener(graph));
    TestServer server(graph, manager);

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
    const LobbyGraph graph(fixture.root());
    LiveSessionManager manager(lobby_settings(2), counting_opener(graph));
    TestServer server(graph, manager);
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
    EXPECT_TRUE(graph.sessions->list("lobby").empty());
}

TEST(LobbyRoutes, ValidatesMissingIdentifiersMethodsAndOriginWithoutOrigin) {
    test::TestWorkspace fixture;
    const LobbyGraph graph(fixture.root());
    LiveSessionManager manager(lobby_settings(2), counting_opener(graph));
    TestServer server(graph, manager);

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
    manager.begin_shutdown();
}

TEST(LobbyRoutes, ReportsSessionListingStorageFailuresAsInternalErrors) {
    test::TestWorkspace fixture;
    std::ofstream(fixture.root() / "forums" / "lobby" / "sessions")
        << "not a directory";
    const LobbyGraph graph(fixture.root());
    LiveSessionManager manager(lobby_settings(2), counting_opener(graph));
    TestServer server(graph, manager);

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
    const LobbyGraph graph(fixture.root());
    std::atomic<int> starts{};
    LiveSessionManager manager(lobby_settings(2), counting_opener(graph, &starts));
    TestServer server(graph, manager);
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

TEST(LobbyRoutes, ReattachesFromTheLiveSessionMapWithoutReadingStorageAgain) {
    test::TestWorkspace fixture;
    const LobbyGraph graph(fixture.root());
    std::atomic<int> starts{};
    LiveSessionManager manager(lobby_settings(2), counting_opener(graph, &starts));
    TestServer server(graph, manager);
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
        nlohmann::json({{"forum_id", "lobby"}, {"session_id", id}}));
    EXPECT_EQ(starts, 1);
    manager.begin_shutdown();
}

TEST(LobbyRoutes, DeletedSessionBetweenValidationAndOpenReturnsNotFoundAndReleasesCapacity) {
    test::TestWorkspace fixture;
    const LobbyGraph graph(fixture.root());
    const SessionEntry deleted = graph.sessions->create("lobby", "Deleted");
    const SessionEntry survivor = graph.sessions->create("lobby", "Survivor");
    const std::filesystem::path deleted_database =
        fixture.root() / "forums" / "lobby" / "sessions"
        / (deleted.identity.session_id + ".sqlite3");
    const WebSettings settings = lobby_settings(1);
    const SessionOpener open_real = graph.opener();
    LiveSessionManager manager(
        settings,
        [open_real, deleted, deleted_database](
            const SessionIdentity& key,
            WakeNotifier& notifier) {
            if (key.session_id == deleted.identity.session_id
                && !std::filesystem::remove(deleted_database)) {
                throw std::runtime_error("Test session was not deleted");
            }
            return open_real(key, notifier);
        });
    TestServer server(graph, manager, settings);

    expect_error(
        server.client().Post(
            "/api/v1/forums/lobby/sessions/" + deleted.identity.session_id + "/open",
            "{}",
            "application/json"),
        404,
        "not_found");
    EXPECT_EQ(manager.snapshot().live_session_count, 0U);

    const auto opened = server.client().Post(
        "/api/v1/forums/lobby/sessions/" + survivor.identity.session_id + "/open",
        "{}",
        "application/json");
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened->status, 200);
    manager.begin_shutdown();
}

TEST(LobbyRoutes, DeletedForumBetweenValidationAndOpenReturnsNotFound) {
    test::TestWorkspace fixture;
    const LobbyGraph graph(fixture.root());
    const SessionEntry stored = graph.sessions->create("lobby", "Deleted forum");
    const std::filesystem::path forum_directory =
        fixture.root() / "forums" / "lobby";
    const WebSettings settings = lobby_settings(1);
    const SessionOpener open_real = graph.opener();
    LiveSessionManager manager(
        settings,
        [open_real, forum_directory](
            const SessionIdentity& key,
            WakeNotifier& notifier) {
            if (std::filesystem::remove_all(forum_directory) == 0) {
                throw std::runtime_error("Test forum was not deleted");
            }
            return open_real(key, notifier);
        });
    TestServer server(graph, manager, settings);

    expect_error(
        server.client().Post(
            "/api/v1/forums/lobby/sessions/" + stored.identity.session_id + "/open",
            "{}",
            "application/json"),
        404,
        "not_found");
    EXPECT_EQ(manager.snapshot().live_session_count, 0U);
}

TEST(LobbyRoutes, ValidatesOpenBodyAndAppliesTheRequestLimit) {
    test::TestWorkspace fixture;
    const LobbyGraph graph(fixture.root());
    std::atomic<int> starts{};
    LiveSessionManager manager(lobby_settings(2), counting_opener(graph, &starts));
    WebSettings settings{.open_deadline = 500ms, .request_body_limit = 64};
    TestServer server(graph, manager, settings);
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
    manager.begin_shutdown();
}

TEST(LobbyRoutes, WrapsGeneratedAndUnhandledErrorsInTheCommonEnvelope) {
    test::TestWorkspace fixture;
    const LobbyGraph graph(fixture.root());
    LiveSessionManager manager(lobby_settings(2), counting_opener(graph));
    TestServer server(
        graph,
        manager,
        lobby_settings(),
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

TEST(LobbyRoutes, NewStoredSessionSurvivesBusyOpenAndCanBeRetried) {
    test::TestWorkspace fixture;
    const LobbyGraph graph(fixture.root());
    std::atomic<bool> busy{true};
    LiveSessionManager manager(
        lobby_settings(),
        [&busy, open = graph.opener()](
            const SessionIdentity& key, WakeNotifier& notifier) {
            if (busy) throw SessionBusyError("held by test");
            return open(key, notifier);
        });
    TestServer server(graph, manager);
    const std::string id = create_session(server);
    const std::string route = "/api/v1/forums/lobby/sessions/" + id + "/open";

    expect_error(
        server.client().Post(route, "{}", "application/json"),
        409,
        "session_busy",
        "Session is busy.");
    EXPECT_TRUE(listed_not_live(server, id));

    busy = false;
    const auto opened = server.client().Post(route, "{}", "application/json");
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened->status, 200);
    EXPECT_EQ(body(opened), nlohmann::json({{"forum_id", "lobby"}, {"session_id", id}}));
    manager.begin_shutdown();
}

TEST(LobbyRoutes, CreateSucceedsWhileAnotherStoredSessionLeaseIsHeld) {
    test::TestWorkspace fixture;
    const LobbyGraph graph(fixture.root());
    const WebSettings settings = lobby_settings(1);
    LiveSessionManager manager(settings, graph.opener());
    TestServer server(graph, manager, settings);
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
        "session_busy",
        "Session is busy.");
    manager.begin_shutdown();
}

TEST(LobbyRoutes, NewStoredSessionSurvivesInitializationFailureAndCanBeRetried) {
    test::TestWorkspace fixture;
    const LobbyGraph graph(fixture.root());
    std::atomic<bool> fail{true};
    LiveSessionManager manager(
        lobby_settings(),
        [&fail, open = graph.opener()](
            const SessionIdentity& key, WakeNotifier& notifier) {
            if (fail) throw std::runtime_error("initialization failed");
            return open(key, notifier);
        });
    TestServer server(graph, manager);
    const std::string id = create_session(server);
    const std::string route = "/api/v1/forums/lobby/sessions/" + id + "/open";

    expect_error(
        server.client().Post(route, "{}", "application/json"),
        500,
        "internal_error",
        "Session could not be opened.");
    EXPECT_TRUE(listed_not_live(server, id));

    fail = false;
    const auto opened = server.client().Post(route, "{}", "application/json");
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened->status, 200);
    manager.begin_shutdown();
}

TEST(LobbyRoutes, NewStoredSessionSurvivesLimitOpenAndCanBeRetried) {
    test::TestWorkspace fixture;
    const LobbyGraph graph(fixture.root());
    LiveSessionManager manager(lobby_settings(1), counting_opener(graph));
    TestServer server(graph, manager);
    const std::string occupied = create_session(server, "Occupied");
    const std::string occupied_route = "/api/v1/forums/lobby/sessions/" + occupied + "/open";
    const auto occupied_open =
        server.client().Post(occupied_route, "{}", "application/json");
    ASSERT_TRUE(occupied_open);
    ASSERT_EQ(occupied_open->status, 200);

    // Creation does not consult the live-session map and therefore cannot return
    // an open-lifecycle error even while the live-session limit is consumed.
    const std::string id = create_session(server, "Retry later");
    const std::string route = "/api/v1/forums/lobby/sessions/" + id + "/open";

    expect_error(
        server.client().Post(route, "{}", "application/json"),
        503,
        "session_limit_reached",
        "Session limit reached.");
    EXPECT_TRUE(listed_not_live(server, id));

    LiveSessionHandle occupied_session = manager.lookup({"lobby", occupied});
    ASSERT_TRUE(occupied_session);
    occupied_session->request_shutdown();
    httplib::Result opened;
    for (int attempt = 0; attempt != 50; ++attempt) {
        opened = server.client().Post(route, "{}", "application/json");
        if (opened && opened->status == 200) break;
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened->status, 200);
    manager.begin_shutdown();
}

TEST(LobbyRoutes, OpenTimeoutUsesTheLobbyErrorEnvelope) {
    test::TestWorkspace fixture;
    const LobbyGraph graph(fixture.root());
    LiveSessionManager manager(
        lobby_settings(),
        [open = graph.opener()](
            const SessionIdentity& key, WakeNotifier& notifier) {
            std::this_thread::sleep_for(100ms);
            return open(key, notifier);
        });
    WebSettings timeout_settings = lobby_settings();
    timeout_settings.open_deadline = 1ms;
    TestServer server(graph, manager, timeout_settings);
    const std::string id = create_session(server);
    const std::string route = "/api/v1/forums/lobby/sessions/" + id + "/open";

    expect_error(
        server.client().Post(route, "{}", "application/json"),
        503,
        "session_open_timeout",
        "Session is still opening.");
    EXPECT_TRUE(listed_not_live(server, id));

    // The abandoned open was never cancelled, so a later request observes the
    // same actor as ready.
    httplib::Result opened;
    for (int attempt = 0; attempt != 100; ++attempt) {
        opened = server.client().Post(route, "{}", "application/json");
        if (opened && opened->status == 200) break;
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened->status, 200);
    manager.begin_shutdown();
}

TEST(LobbyRoutes, MapsStoppingAndManagerShutdownOpenFailuresToExistingEnvelopes) {
    test::TestWorkspace fixture;
    const LobbyGraph graph(fixture.root());
    // A completion that ignores cancellation keeps this actor's owner inside
    // SessionController::shutdown(), which is exactly the stopping state the
    // lobby must report.
    auto controls = std::make_shared<test::BackendControls>();
    std::atomic<int> starts{};
    LiveSessionManager manager(
        lobby_settings(1),
        [&starts, controls, root = graph.root()](
            const SessionIdentity& key, WakeNotifier& notifier) {
            ++starts;
            return test::open_scripted_session(
                key,
                root / "forums" / key.forum_id / "sessions"
                    / (key.session_id + ".sqlite3"),
                notifier,
                controls);
        });
    TestServer server(graph, manager);
    const std::string id = create_session(server, "Stopping");
    const std::string route = "/api/v1/forums/lobby/sessions/" + id + "/open";

    const auto opened = server.client().Post(route, "{}", "application/json");
    ASSERT_TRUE(opened);
    ASSERT_EQ(opened->status, 200);
    LiveSessionHandle session = manager.lookup({"lobby", id});
    ASSERT_TRUE(session);
    controls->ignore_cancellation();
    ASSERT_TRUE(std::holds_alternative<CommandResult>(
        session->submit(RawCommand{"reader", "Question"}, 5s)));
    ASSERT_TRUE(controls->wait_until_running());
    session->request_shutdown();
    const auto stopping_deadline = std::chrono::steady_clock::now() + 5s;
    while (session->lifecycle() != LiveSessionState::stopping
        && std::chrono::steady_clock::now() < stopping_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_EQ(session->lifecycle(), LiveSessionState::stopping);

    expect_error(
        server.client().Post(route, "{}", "application/json"),
        409,
        "session_stopping",
        "Session is stopping.");
    EXPECT_EQ(starts, 1);

    controls->finish();
    const auto finished_deadline = std::chrono::steady_clock::now() + 5s;
    while (session->lifecycle() != LiveSessionState::finished
        && std::chrono::steady_clock::now() < finished_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    manager.sweep();
    manager.begin_shutdown();
    expect_error(
        server.client().Post(route, "{}", "application/json"),
        503,
        "server_stopping",
        "Server is stopping.");
}

} // namespace
} // namespace cha::web
