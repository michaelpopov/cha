#include "web/asset_handler.h"
#include "web/http_server.h"
#include "web/lobby_routes.h"
#include "web/live_session_manager.h"
#include "support/test_live_session.h"
#include "support/test_web_graph.h"
#include "support/test_workspace.h"

#include "workspace/builtins.h"

#include "session/not_found_error.h"
#include "session/session_controller.h"
#include "session/session_repository.h"
#include "session/sqlite_storage.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
               const FullSessionId& key, std::shared_ptr<WakeNotifier> notifier) {
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
            graph.sessions(), LobbyGraph::initial_selection(),
            live_sessions, graph.root() / "backups", settings).install(server_);
        if (installer) installer(server_);
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ < 0) throw std::runtime_error("Could not bind test server");
        configure_http_server(server_, settings);
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
    EXPECT_EQ(json["error"]["code"].get<std::string>(), code);
    EXPECT_TRUE(json["error"]["code"].is_string());
    EXPECT_TRUE(json["error"]["message"].is_string());
    if (!message.empty()) {
        EXPECT_EQ(json["error"]["message"].get<std::string>(), message);
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

bool session_is_live(LiveSessionManager& manager, const FullSessionId& key) {
    const LiveSessionManagerSnapshot snapshot = manager.snapshot();
    return std::find(
               snapshot.running_sessions.begin(),
               snapshot.running_sessions.end(),
               key)
        != snapshot.running_sessions.end();
}

bool listed_not_live(TestServer& server, std::string_view id) {
    const auto listed = server.client().Get("/api/v1/forums/lobby/sessions");
    if (!listed || listed->status != 200) return false;
    for (const auto& session : body(listed)) {
        if (session["id"].get<std::string>() == id) return !session["live"].get<bool>();
    }
    return false;
}

void add_writer_forum(const test::TestWorkspace& fixture) {
    fixture.add_character("writer", "Writer");
    fixture.add_forum("writers", "Writers", "writer");
}

TEST(LobbyRoutes, ServesBootstrapDiscoveryAndHealthWithoutSessionDataInHealth) {
    test::TestWorkspace fixture;
    fixture.write_character_config(
        "display_name = \"Guide\"\n"
        "description = \"Explains the workspace\"\n"
        "provider = \"test\"\n");
    std::ofstream(fixture.root() / "characters" / "guide" / "PROFILE.md")
        << "Profile $${character.display_name}\n";
    std::ofstream(fixture.root() / "characters" / "guide" / "CHARACTER.md")
        << "Agent instructions\n"
           "<character_profile>\n"
           "$$(PROFILE.md)\n"
           "</character_profile>\n";
    std::ofstream(fixture.root() / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\n"
           "description = \"Where visitors arrive\"\n";
    std::ofstream(fixture.root() / "forums" / "lobby" / "FORUM.md")
        << "# House rules\n"
           "\n"
           "A deliberate place to talk.\n";
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
    EXPECT_EQ(bootstrap_body["personas"][0]["id"], "builtin-guest");
    EXPECT_EQ(bootstrap_body["personas"][1]["id"], "reader");
    EXPECT_TRUE(bootstrap_body["recent_sessions"].is_array());
    // Built-ins take their place in display-name order rather than trailing it.
    EXPECT_EQ(bootstrap_body["characters"][0]["id"], "builtin-assistant");
    EXPECT_EQ(bootstrap_body["characters"][1]["id"], "guide");
    EXPECT_EQ(bootstrap_body["forums"][0]["id"], "builtin-entrance");
    EXPECT_EQ(bootstrap_body["forums"][1]["id"], "lobby");
    // The short line rides along with discovery so a roster row needs no
    // request; the long one waits for the detail endpoint.
    EXPECT_EQ(bootstrap_body["forums"][1]["description"], "Where visitors arrive");
    EXPECT_FALSE(bootstrap_body["forums"][1].contains("forum_markdown"));
    EXPECT_FALSE(bootstrap_body["forums"][0].contains("description"));
    const nlohmann::json forums = bootstrap_body["forums"];
    const auto entrance = std::find_if(forums.begin(), forums.end(), [](const nlohmann::json& forum) {
        return forum["id"] == "builtin-entrance";
    });
    ASSERT_NE(entrance, forums.end());
    EXPECT_EQ((*entrance)["default_character_id"], "builtin-assistant");
    EXPECT_EQ((*entrance)["members"], nlohmann::json::array({{
        {"id", "builtin-assistant"}, {"display_name", "Assistant"},
        {"appearance", {{"font", "sans"}, {"style", "normal"},
            {"weight", "normal"}, {"size", "normal"}, {"text_color", "normal"}}},
    }}));

    const auto workspace_character = server.client().Get("/api/v1/characters/guide");
    ASSERT_TRUE(workspace_character);
    ASSERT_EQ(workspace_character->status, 200);
    const nlohmann::json workspace_character_body = body(workspace_character);
    EXPECT_EQ(workspace_character_body["character_markdown"], "Profile Guide");
    EXPECT_EQ(workspace_character_body["appearance"], (*guide)["appearance"]);
    EXPECT_EQ(workspace_character_body["provider"], "test");
    EXPECT_TRUE(workspace_character_body["style"].is_null());
    EXPECT_EQ(workspace_character_body["writable"], true);

    const auto assistant_character = server.client().Get("/api/v1/characters/builtin-assistant");
    ASSERT_TRUE(assistant_character);
    ASSERT_EQ(assistant_character->status, 200);
    const nlohmann::json assistant_body = body(assistant_character);
    EXPECT_FALSE(assistant_body["character_markdown"].get<std::string>().empty());
    EXPECT_EQ(assistant_body["writable"], false);
    EXPECT_TRUE(assistant_body["provider"].is_null());
    EXPECT_TRUE(assistant_body["style"].is_null());

    const auto reader_persona = server.client().Get("/api/v1/personas/reader");
    ASSERT_TRUE(reader_persona);
    ASSERT_EQ(reader_persona->status, 200);
    EXPECT_EQ(body(reader_persona)["persona_markdown"], "");
    expect_error(
        server.client().Get("/api/v1/characters/missing"),
        404, "not_found", "That character was not found.");
    expect_error(
        server.client().Get("/api/v1/personas/missing"),
        404, "not_found", "That persona was not found.");

    const auto lobby_forum = server.client().Get("/api/v1/forums/lobby");
    ASSERT_TRUE(lobby_forum);
    ASSERT_EQ(lobby_forum->status, 200);
    const nlohmann::json lobby_forum_body = body(lobby_forum);
    EXPECT_EQ(lobby_forum_body["display_name"], "The Lobby");
    EXPECT_EQ(lobby_forum_body["description"], "Where visitors arrive");
    EXPECT_EQ(lobby_forum_body["forum_markdown"],
        "# House rules\n\nA deliberate place to talk.\n");
    EXPECT_EQ(lobby_forum_body["members"].size(), 1);

    // The built-in forum keeps no forum directory, so it has nothing to
    // publish and says so rather than borrowing the Assistant's guide.
    const auto entrance_forum = server.client().Get("/api/v1/forums/builtin-entrance");
    ASSERT_TRUE(entrance_forum);
    ASSERT_EQ(entrance_forum->status, 200);
    EXPECT_EQ(body(entrance_forum)["forum_markdown"], "");
    EXPECT_FALSE(body(entrance_forum).contains("description"));
    expect_error(server.client().Get("/api/v1/forums/missing"), 404, "not_found");

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

TEST(LobbyRoutes, ReloadsWorkspaceDiscoveryAndRestartsEveryLiveSession) {
    test::TestWorkspace fixture;
    const LobbyGraph graph(fixture.root());
    LiveSessionManager manager(lobby_settings(2), counting_opener(graph));
    TestServer server(graph, manager);
    const std::string id = create_session(server, "Reloaded");
    ASSERT_FALSE(id.empty());
    ASSERT_EQ(
        server.client().Post(
            "/api/v1/forums/lobby/sessions/" + id + "/open",
            "{}", "application/json")->status,
        200);
    ASSERT_TRUE(session_is_live(manager, {"lobby", id}));

    fixture.add_persona("writer_persona", "Writer Persona");
    add_writer_forum(fixture);
    const auto reloaded = server.client().Post(
        "/api/v1/workspace/reload", "{}", "application/json");
    ASSERT_TRUE(reloaded);
    ASSERT_EQ(reloaded->status, 200) << reloaded->body;
    const nlohmann::json bootstrap = body(reloaded);
    EXPECT_TRUE(std::ranges::any_of(bootstrap["personas"], [](const nlohmann::json& persona) {
        return persona["id"] == "writer_persona";
    }));
    EXPECT_TRUE(std::ranges::any_of(bootstrap["characters"], [](const nlohmann::json& character) {
        return character["id"] == "writer";
    }));
    EXPECT_TRUE(std::ranges::any_of(bootstrap["forums"], [](const nlohmann::json& forum) {
        return forum["id"] == "writers";
    }));
    const std::filesystem::directory_iterator backups(graph.root() / "backups");
    ASSERT_NE(backups, std::filesystem::directory_iterator{});
    EXPECT_TRUE(std::filesystem::is_regular_file(backups->path()));

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (manager.snapshot().live_session_count != 0
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_EQ(manager.snapshot().live_session_count, 0U);

    const auto created = server.client().Post(
        "/api/v1/forums/writers/sessions", R"({"label":"Draft"})", "application/json");
    ASSERT_TRUE(created);
    EXPECT_EQ(created->status, 201) << created->body;
}

TEST(LobbyRoutes, DoesNotBackupWhenCheckpointRemainsBusy) {
    test::TestWorkspace fixture;
    const LobbyGraph graph(fixture.root());
    LiveSessionManager manager(lobby_settings(2), counting_opener(graph));
    TestServer server(graph, manager);

    storage::SqliteDatabase reader(
        graph.sessions()->database_path(),
        storage::SqliteDatabase::Mode::read_only);
    reader.execute("BEGIN");
    {
        storage::SqliteStatement snapshot = reader.prepare(
            "SELECT COUNT(*) FROM forums");
        ASSERT_TRUE(snapshot.step());
    }
    {
        storage::SqliteDatabase writer(
            graph.sessions()->database_path(),
            storage::SqliteDatabase::Mode::read_write);
        writer.execute(
            "INSERT INTO forums (forum_id) VALUES ('checkpoint-order')");
    }
    std::filesystem::path wal = graph.sessions()->database_path();
    wal += "-wal";
    ASSERT_GT(std::filesystem::file_size(wal), 0U);

    httplib::Client client = server.client();
    client.set_read_timeout(10s);
    expect_error(
        client.Post("/api/v1/workspace/reload", "{}", "application/json"),
        422, "workspace_reload_failed");

    EXPECT_GT(std::filesystem::file_size(wal), 0U);
    EXPECT_FALSE(std::filesystem::exists(graph.root() / "backups"));
    reader.execute("ROLLBACK");
}

TEST(LobbyRoutes, KeepsThePublishedWorkspaceWhenReloadFailsAfterBackup) {
    test::TestWorkspace fixture;
    const LobbyGraph graph(fixture.root());
    LiveSessionManager manager(lobby_settings(2), counting_opener(graph));
    TestServer server(graph, manager);
    const std::string id = create_session(server, "Unchanged");
    ASSERT_FALSE(id.empty());
    ASSERT_EQ(
        server.client().Post(
            "/api/v1/forums/lobby/sessions/" + id + "/open",
            "{}", "application/json")->status,
        200);
    ASSERT_TRUE(session_is_live(manager, {"lobby", id}));

    fixture.write_character_config("display_name =\n");
    expect_error(
        server.client().Post("/api/v1/workspace/reload", "{}", "application/json"),
        422, "workspace_reload_failed");

    EXPECT_FALSE(session_is_live(manager, {"lobby", id}));
    const auto bootstrap = server.client().Get("/api/v1/bootstrap");
    ASSERT_TRUE(bootstrap);
    ASSERT_EQ(bootstrap->status, 200);
    EXPECT_TRUE(std::ranges::any_of(body(bootstrap)["characters"], [](const nlohmann::json& character) {
        return character["id"] == "guide";
    }));
}

TEST(LobbyRoutes, RejectsCredentialValidationFailureWithoutPublishingIt) {
    test::TestWorkspace fixture;
    const LobbyGraph graph(fixture.root());
    LiveSessionManager manager(lobby_settings(2), counting_opener(graph));
    TestServer server(graph, manager);
    const std::string id = create_session(server, "Credential reload");
    ASSERT_FALSE(id.empty());
    ASSERT_EQ(
        server.client().Post(
            "/api/v1/forums/lobby/sessions/" + id + "/open",
            "{}", "application/json")->status,
        200);

    fixture.write_provider(
        "test",
        "host = \"test\"\nport = 1\nmode = \"test\"\nmodel = \"fake\"\n"
        "api_key_env = \"CHA_RELOAD_MISSING_CREDENTIAL_7BA5F8\"\n");
    expect_error(
        server.client().Post("/api/v1/workspace/reload", "{}", "application/json"),
        422, "workspace_reload_failed");

    const auto bootstrap = server.client().Get("/api/v1/bootstrap");
    ASSERT_TRUE(bootstrap);
    ASSERT_EQ(bootstrap->status, 200);
    EXPECT_TRUE(std::ranges::any_of(body(bootstrap)["characters"], [](const nlohmann::json& character) {
        return character["id"] == "guide" && character["display_name"] == "Guide";
    }));
    EXPECT_FALSE(session_is_live(manager, {"lobby", id}));
}

TEST(LobbyRoutes, ServesCharacterProviderAndStyleSettingsWithoutLeakingProviderConfig) {
    test::TestWorkspace fixture;
    fixture.write_provider(
        "sol-high", "host = \"secret.example\"\nport = 9\nmode = \"test\"\nmodel = \"fake\"\n");
    fixture.write_provider("broken", "this is not a usable provider\n");
    fixture.write_style("mono-large", "font = \"mono\"\nsize = \"large\"\n");
    fixture.write_style("serif-italic", "font = \"serif\"\nstyle = \"italic\"\n");
    fixture.write_style("broken", "font = \"comic\"\n");
    fixture.write_character_config(
        "display_name = \"Guide\"\nprovider = \"test\"\nstyle = \"serif-italic\"\n");
    const auto montaigne = fixture.root() / "characters" / "montaigne";
    std::filesystem::create_directories(montaigne);
    std::ofstream(montaigne / "character.toml")
        << "display_name = \"Montaigne\"\nprovider = \"test\"\nstyle = \"serif-italic\"\n";
    std::ofstream(montaigne / "CHARACTER.md") << "Prompt\n";
    const auto circle = fixture.root() / "forums" / "circle";
    std::filesystem::create_directories(circle / "members" / "montaigne");
    std::ofstream(circle / "config.toml") << "display_name = \"Circle of Life\"\n";
    std::ofstream(circle / "FORUM.md") << "Forum instructions\n";
    std::ofstream(circle / "members" / "character_defaults.toml")
        << "provider = \"sol-high\"\n";

    const LobbyGraph graph(fixture.root());
    LiveSessionManager manager(lobby_settings(2), counting_opener(graph));
    TestServer server(graph, manager);

    const auto guide = server.client().Get("/api/v1/characters/guide");
    ASSERT_TRUE(guide);
    ASSERT_EQ(guide->status, 200);
    const nlohmann::json guide_body = body(guide);
    EXPECT_EQ(guide_body["provider"], "test");
    EXPECT_EQ(guide_body["style"], "serif-italic");
    EXPECT_EQ(guide_body["writable"], true);
    EXPECT_FALSE(guide_body.contains("provider_overridden_by"));

    std::vector<std::string> provider_ids;
    for (const auto& option : guide_body["available_providers"]) {
        ASSERT_EQ(option.size(), 2);
        EXPECT_TRUE(option.contains("id"));
        EXPECT_TRUE(option.contains("label"));
        EXPECT_FALSE(option.contains("host"));
        EXPECT_FALSE(option.contains("port"));
        EXPECT_FALSE(option.contains("model"));
        provider_ids.push_back(option["id"].get<std::string>());
    }
    EXPECT_EQ(provider_ids, (std::vector<std::string>{"sol-high", "test"}));

    const auto styles = guide_body["available_styles"];
    const auto mono_large = std::find_if(
        styles.begin(), styles.end(),
        [](const nlohmann::json& option) { return option["id"] == "mono-large"; });
    ASSERT_NE(mono_large, styles.end());
    EXPECT_EQ((*mono_large)["label"], "Mono large");
    EXPECT_EQ((*mono_large)["appearance"], nlohmann::json({
        {"font", "mono"}, {"style", "normal"}, {"weight", "normal"},
        {"size", "large"}, {"text_color", "normal"}}));
    for (const auto& option : styles) {
        EXPECT_NE(option["id"], "broken");
    }
    for (const auto& option : guide_body["available_providers"]) {
        EXPECT_NE(option["id"], "broken");
    }

    const auto montaigne_detail = server.client().Get("/api/v1/characters/montaigne");
    ASSERT_TRUE(montaigne_detail);
    ASSERT_EQ(montaigne_detail->status, 200);
    const nlohmann::json montaigne_body = body(montaigne_detail);
    EXPECT_EQ(montaigne_body["provider"], "test");
    EXPECT_FALSE(montaigne_body.contains("provider_overridden_by"));

    // Hand edits do not change the published workspace until an explicit
    // reload. The detail remains the complete snapshot loaded at startup.
    fixture.write_character_config("display_name = \"Guide\"\nstyle = \n");
    const auto damaged = server.client().Get("/api/v1/characters/guide");
    ASSERT_TRUE(damaged);
    ASSERT_EQ(damaged->status, 200);
    const nlohmann::json damaged_body = body(damaged);
    EXPECT_EQ(damaged_body["character_markdown"], guide_body["character_markdown"]);
    EXPECT_EQ(damaged_body["provider"], "test");
    EXPECT_EQ(damaged_body["style"], "serif-italic");
    EXPECT_EQ(damaged_body["writable"], true);
}

// A session reads its definitions on the way up, so one that is still opening
// when the save commits may already hold the old settings. The manager publishes
// it as running only once it has finished, so a fan-out driven by running
// sessions alone could leave it live on values the file no longer has.
TEST(LobbyRoutes, ReloadsASessionThatWasStillOpeningWhenTheSaveCommitted) {
    test::TestWorkspace fixture;
    fixture.write_style("mono-large", "font = \"mono\"\nsize = \"large\"\n");
    const LobbyGraph graph(fixture.root());

    std::mutex mutex;
    std::condition_variable gate;
    bool opening = false;
    bool released = false;
    LiveSessionManager manager(
        lobby_settings(2),
        [open = graph.opener(), &mutex, &gate, &opening, &released](
            const FullSessionId& key, std::shared_ptr<WakeNotifier> notifier) {
            {
                std::unique_lock lock(mutex);
                opening = true;
                gate.notify_all();
                gate.wait(lock, [&released] { return released; });
            }
            return open(key, notifier);
        });
    TestServer server(graph, manager);
    const std::string id = create_session(server, "Opening");
    ASSERT_FALSE(id.empty());

    int opening_status = -1;
    std::string opening_body;
    std::thread opening_request([&server, &id, &opening_status, &opening_body] {
        const auto response = server.client().Post(
            "/api/v1/forums/lobby/sessions/" + id + "/open", "{}", "application/json");
        if (!response) return;
        opening_status = response->status;
        opening_body = response->body;
    });
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(gate.wait_for(lock, 5s, [&opening] { return opening; }));
    }
    // The actor is registered and starting, so it is deliberately absent from
    // the running sessions a listing would report.
    EXPECT_FALSE(session_is_live(manager, {"lobby", id}));

    const auto patched = server.client().Patch(
        "/api/v1/characters/guide",
        R"({"provider":"test","style":"mono-large"})",
        "application/json");
    ASSERT_TRUE(patched);
    ASSERT_EQ(patched->status, 200) << patched->body;

    {
        std::lock_guard lock(mutex);
        released = true;
    }
    gate.notify_all();
    opening_request.join();

    ASSERT_EQ(opening_status, 409);
    EXPECT_EQ(
        nlohmann::json::parse(opening_body)["error"]["code"],
        "session_stopping");

    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (manager.snapshot().live_session_count != 0
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_EQ(manager.snapshot().live_session_count, 0U)
        << "a session that was opening when the save landed stayed live even "
           "though it may have read settings before the write";
}

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

httplib::Result patch_character(
    TestServer& server,
    std::string_view id,
    const nlohmann::json& body) {
    return server.client().Patch(
        "/api/v1/characters/" + std::string(id),
        body.dump(),
        "application/json");
}

TEST(LobbyRoutes, PatchesCharacterSettingsAndLeavesTheFileAloneOnABadName) {
    test::TestWorkspace fixture;
    fixture.write_style("serif-italic", "font = \"serif\"\nstyle = \"italic\"\n");
    fixture.write_provider("broken", "this is not a usable provider\n");
    const auto path = fixture.root() / "characters" / "guide" / "character.toml";
    const LobbyGraph graph(fixture.root());
    LiveSessionManager manager(lobby_settings(2), counting_opener(graph));
    TestServer server(graph, manager);

    const auto saved = patch_character(
        server, "guide", {{"provider", "test"}, {"style", "serif-italic"}});
    ASSERT_TRUE(saved);
    ASSERT_EQ(saved->status, 200);
    EXPECT_EQ(body(saved)["provider"], "test");
    EXPECT_EQ(body(saved)["style"], "serif-italic");
    EXPECT_EQ(body(saved)["writable"], true);

    const auto cleared = patch_character(
        server, "guide", {{"provider", "test"}, {"style", nullptr}});
    ASSERT_TRUE(cleared);
    ASSERT_EQ(cleared->status, 200);
    EXPECT_EQ(body(cleared)["provider"], "test");
    EXPECT_TRUE(body(cleared)["style"].is_null());

    const std::string before = read_bytes(path);
    expect_error(
        patch_character(server, "guide", {{"provider", "broken"}, {"style", nullptr}}),
        400, "bad_request", "Invalid provider or style.");
    EXPECT_EQ(read_bytes(path), before);

    expect_error(
        patch_character(
            server, "builtin-assistant", {{"provider", "test"}, {"style", nullptr}}),
        404, "not_found", "That character was not found.");
    expect_error(
        patch_character(server, "missing", {{"provider", "test"}, {"style", nullptr}}),
        404, "not_found", "That character was not found.");
    expect_error(
        patch_character(server, "guide", {{"provider", "test"}}),
        400, "bad_request");

    fixture.write_character_config("display_name = \"Guide\"\nstyle = \n");
    const std::string damaged = read_bytes(path);
    const auto unchanged = patch_character(
        server, "guide", {{"provider", "test"}, {"style", nullptr}});
    ASSERT_TRUE(unchanged);
    EXPECT_EQ(unchanged->status, 200);
    EXPECT_EQ(read_bytes(path), damaged);
}

TEST(LobbyRoutes, ReloadsEveryForumContainingTheChangedCharacter) {
    test::TestWorkspace fixture;
    fixture.write_provider(
        "sol-high", "host = \"test\"\nport = 1\nmode = \"test\"\nmodel = \"fake\"\n");
    fixture.write_style("serif-italic", "font = \"serif\"\nstyle = \"italic\"\n");
    const auto montaigne = fixture.root() / "characters" / "montaigne";
    std::filesystem::create_directories(montaigne);
    std::ofstream(montaigne / "character.toml")
        << "display_name = \"Montaigne\"\nprovider = \"test\"\n";
    std::ofstream(montaigne / "CHARACTER.md") << "Prompt\n";
    const auto circle = fixture.root() / "forums" / "circle";
    std::filesystem::create_directories(circle / "members" / "montaigne");
    std::ofstream(circle / "config.toml") << "display_name = \"Circle of Life\"\n";
    std::ofstream(circle / "FORUM.md") << "Forum instructions\n";
    std::ofstream(circle / "members" / "character_defaults.toml")
        << "provider = \"sol-high\"\n";

    const LobbyGraph graph(fixture.root());
    LiveSessionManager manager(lobby_settings(4), counting_opener(graph));
    TestServer server(graph, manager);

    const std::string lobby_id = create_session(server);
    ASSERT_FALSE(lobby_id.empty());
    ASSERT_EQ(
        server.client().Post(
            "/api/v1/forums/lobby/sessions/" + lobby_id + "/open",
            "{}", "application/json")->status,
        200);
    const auto circle_created = server.client().Post(
        "/api/v1/forums/circle/sessions",
        R"({"label":"Circle"})",
        "application/json");
    ASSERT_TRUE(circle_created);
    ASSERT_EQ(circle_created->status, 201);
    const std::string circle_id = body(circle_created)["id"];
    ASSERT_EQ(
        server.client().Post(
            "/api/v1/forums/circle/sessions/" + circle_id + "/open",
            "{}", "application/json")->status,
        200);
    EXPECT_EQ(manager.snapshot().live_session_count, 2);

    // A provider change affects every forum containing that character, even
    // when a stale forum default still names another provider.
    const auto provider_only = patch_character(
        server, "montaigne", {{"provider", "sol-high"}, {"style", nullptr}});
    ASSERT_TRUE(provider_only);
    ASSERT_EQ(provider_only->status, 200);
    const auto circle_deadline = std::chrono::steady_clock::now() + 2s;
    while (session_is_live(manager, {"circle", circle_id})
        && std::chrono::steady_clock::now() < circle_deadline) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_FALSE(session_is_live(manager, {"circle", circle_id}));
    EXPECT_TRUE(session_is_live(manager, {"lobby", lobby_id}));

    // Style changes every forum that contains the character.
    const auto style_save = patch_character(
        server, "guide", {{"provider", "test"}, {"style", "serif-italic"}});
    ASSERT_TRUE(style_save);
    ASSERT_EQ(style_save->status, 200);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (session_is_live(manager, {"lobby", lobby_id})
        && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_FALSE(session_is_live(manager, {"lobby", lobby_id}));
    EXPECT_FALSE(session_is_live(manager, {"circle", circle_id}));
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

TEST(LobbyRoutes, SessionLabelErrorsAreDistinctFromMalformedJson) {
    test::TestWorkspace fixture;
    const LobbyGraph graph(fixture.root());
    LiveSessionManager manager(lobby_settings(2), counting_opener(graph));
    TestServer server(graph, manager);
    constexpr std::string_view route = "/api/v1/forums/lobby/sessions";

    const std::vector<std::string> invalid_create_labels{
        " leading",
        "trailing ",
        "line\nbreak",
        "tab\tcharacter",
        std::string(1, '\x01'),
        std::string(201, 'x'),
    };
    for (const std::string& label : invalid_create_labels) {
        expect_error(
            server.client().Post(
                std::string(route),
                nlohmann::json({{"label", label}}).dump(),
                "application/json"),
            400,
            "bad_request",
            "Invalid session label.");
    }

    expect_error(
        server.client().Post(
            std::string(route), R"({"label":)", "application/json"),
        400,
        "bad_request",
        "Invalid JSON request body.");

    const auto empty = server.client().Post(
        std::string(route), R"({"label":""})", "application/json");
    ASSERT_TRUE(empty);
    ASSERT_EQ(empty->status, 201);
    EXPECT_EQ(body(empty)["label"], body(empty)["id"]);
}

TEST(LobbyRoutes, RenamesAndRecoverablyDeletesStoredSessions) {
    test::TestWorkspace fixture;
    const LobbyGraph graph(fixture.root());
    LiveSessionManager manager(lobby_settings(2), counting_opener(graph));
    TestServer server(graph, manager);
    const std::string id = create_session(server, "Before");
    const std::string route = "/api/v1/forums/lobby/sessions/" + id;

    const auto renamed = server.client().Patch(
        route, R"({"label":"After"})", "application/json");
    ASSERT_TRUE(renamed);
    EXPECT_EQ(renamed->status, 200);
    EXPECT_EQ(body(renamed), nlohmann::json({{"id", id}, {"label", "After"}}));
    EXPECT_EQ(graph.sessions()->prepare({"lobby", id}).label, "After");

    const std::vector<std::string> invalid_labels{
        "",
        " leading",
        "trailing ",
        "line\nbreak",
        "tab\tcharacter",
        std::string(1, '\x01'),
        std::string(201, 'x'),
    };
    for (const std::string& label : invalid_labels) {
        expect_error(
            server.client().Patch(
                route,
                nlohmann::json({{"label", label}}).dump(),
                "application/json"),
            400,
            "bad_request",
            "Invalid session label.");
    }

    const auto deleted = server.client().Delete(route, "{}", "application/json");
    ASSERT_TRUE(deleted);
    EXPECT_EQ(deleted->status, 204);
    EXPECT_TRUE(deleted->body.empty());
    EXPECT_TRUE(graph.sessions()->list("lobby").empty());
    EXPECT_THROW(
        (void)graph.sessions()->prepare({"lobby", id}),
        SessionNotFoundError);
}

TEST(LobbyRoutes, DownloadsStoredAndLiveSessionsAsMarkdown) {
    test::TestWorkspace fixture;
    const LobbyGraph graph(fixture.root());
    LiveSessionManager manager(lobby_settings(2), counting_opener(graph));
    TestServer server(graph, manager);
    const std::string id = create_session(server, "Review notes");
    {
        PreparedSession prepared = graph.sessions()->prepare({"lobby", id});
        SessionJournal journal(
            prepared.database_path, prepared.session_key);
        const TranscriptEntry prompt = make_human_entry({
            .id = 1,
            .author = {"reader", "Reader"},
            .addressed_to = {"guide", "Guide"},
            .text = "Review **this**",
            .request_id = 1,
        });
        journal.start_turn(1, prompt);
        journal.complete_turn(1, make_character_entry(
            2, "guide", "Guide", "Looks good.", EntryStatus::complete, 1));
    }

    const auto downloaded = server.client().Get(
        "/api/v1/forums/lobby/sessions/" + id + "/download");
    ASSERT_TRUE(downloaded);
    EXPECT_EQ(downloaded->status, 200);
    EXPECT_EQ(downloaded->get_header_value("Content-Type"),
        "text/markdown; charset=utf-8");
    EXPECT_EQ(downloaded->get_header_value("Cache-Control"), "no-store");
    EXPECT_NE(downloaded->body.find("# Review notes\n"), std::string::npos);
    EXPECT_NE(downloaded->body.find("## Reader\n*"), std::string::npos);
    EXPECT_NE(downloaded->body.find("Review **this**\n"), std::string::npos);
    EXPECT_NE(downloaded->body.find("## Guide\n*"), std::string::npos);
    EXPECT_NE(downloaded->body.find("Looks good.\n"), std::string::npos);

    const std::string route = "/api/v1/forums/lobby/sessions/" + id;
    ASSERT_EQ(server.client().Post(
        route + "/open", "{}", "application/json")->status, 200);
    ASSERT_EQ(server.client().Patch(
        route, R"({"label":"Live review"})", "application/json")->status, 200);
    const auto live_download = server.client().Get(route + "/download");
    ASSERT_TRUE(live_download);
    EXPECT_EQ(live_download->status, 200);
    EXPECT_NE(live_download->body.find("# Live review\n"), std::string::npos);
    EXPECT_NE(live_download->body.find("## Reader\n*"), std::string::npos);
    EXPECT_NE(live_download->body.find("Review **this**\n"), std::string::npos);
    EXPECT_NE(live_download->body.find("## Guide\n*"), std::string::npos);
    EXPECT_NE(live_download->body.find("Looks good.\n"), std::string::npos);
}

TEST(LobbyRoutes, RenamesAndDeletesAnOpenSessionThroughItsOwner) {
    test::TestWorkspace fixture;
    const LobbyGraph graph(fixture.root());
    WebSettings settings = lobby_settings(2);
    settings.sse_drain_deadline = 10ms;
    settings.delete_deadline = 2s;
    LiveSessionManager manager(settings, counting_opener(graph));
    TestServer server(graph, manager, settings);
    const std::string id = create_session(server, "Live before");
    const std::string route = "/api/v1/forums/lobby/sessions/" + id;
    ASSERT_EQ(server.client().Post(route + "/open", "{}", "application/json")->status, 200);

    const auto renamed = server.client().Patch(
        route, R"({"label":"Live after"})", "application/json");
    ASSERT_TRUE(renamed);
    EXPECT_EQ(renamed->status, 200);
    EXPECT_EQ(body(renamed)["label"], "Live after");

    const auto deleted = server.client().Delete(route, "{}", "application/json");
    ASSERT_TRUE(deleted);
    EXPECT_EQ(deleted->status, 204);
    EXPECT_EQ(manager.snapshot().live_session_count, 0U);
    expect_error(
        server.client().Post(route + "/open", "{}", "application/json"),
        404,
        "not_found");
}

TEST(LobbyRoutes, WelcomeCannotBeRenamedOrDeletedWhileLive) {
    test::TestWorkspace fixture;
    const LobbyGraph graph(fixture.root());
    LiveSessionManager manager(lobby_settings(2), counting_opener(graph));
    TestServer server(graph, manager);
    const FullSessionId welcome = LobbyGraph::initial_selection().session;
    const std::string route = "/api/v1/forums/" + welcome.forum_id
        + "/sessions/" + welcome.session_id;

    const auto opened = server.client().Post(
        route + "/open", "{}", "application/json");
    ASSERT_TRUE(opened);
    ASSERT_EQ(opened->status, 200);
    ASSERT_EQ(manager.snapshot().live_session_count, 1U);

    expect_error(
        server.client().Patch(
            route, R"({"label":"HIJACKED"})", "application/json"),
        404,
        "not_found");

    LiveSessionHandle live = manager.lookup(welcome);
    ASSERT_TRUE(live);
    CommandSubmitResult snapshot_result = live->snapshot(5s);
    ASSERT_TRUE(std::holds_alternative<SessionSnapshot>(snapshot_result));
    EXPECT_EQ(std::get<SessionSnapshot>(snapshot_result).session_label, "Welcome");
    ASSERT_EQ(graph.sessions()->list(welcome.forum_id).size(), 1U);
    EXPECT_EQ(graph.sessions()->list(welcome.forum_id).front().label, "Welcome");

    expect_error(
        server.client().Delete(route, "{}", "application/json"),
        404,
        "not_found");
    EXPECT_EQ(manager.snapshot().live_session_count, 1U);
    EXPECT_EQ(manager.lookup(welcome), live);

    manager.begin_shutdown();
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

TEST(LobbyRoutes, AcceptsDnsHostOnReadsAndMutations) {
    test::TestWorkspace fixture;
    const LobbyGraph graph(fixture.root());
    LiveSessionManager manager(lobby_settings(2), counting_opener(graph));
    TestServer server(graph, manager);
    const httplib::Headers rebound{
        {"Host", "evil.example:" + std::to_string(server.port())},
        {"Origin", "http://evil.example:" + std::to_string(server.port())},
    };

    const auto read = server.client().Get("/api/v1/bootstrap", rebound);
    ASSERT_TRUE(read);
    EXPECT_EQ(read->status, 200);
    const auto mutation = server.client().Post(
        "/api/v1/forums/lobby/sessions",
        rebound,
        R"({"label":"hostname"})",
        "application/json");
    ASSERT_TRUE(mutation);
    EXPECT_EQ(mutation->status, 201);
    EXPECT_EQ(graph.sessions()->list("lobby").size(), 1U);
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
    graph.sessions()->archive({"lobby", id});

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
    const StoredSession deleted = graph.sessions()->create("lobby", "Deleted");
    const StoredSession survivor = graph.sessions()->create("lobby", "Survivor");
    const WebSettings settings = lobby_settings(1);
    const SessionOpener open_real = graph.opener();
    LiveSessionManager manager(
        settings,
        [open_real, deleted, sessions = graph.sessions()](
            const FullSessionId& key,
            std::shared_ptr<WakeNotifier> notifier) {
            if (key.session_id == deleted.identity.session_id) {
                sessions->archive(key);
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

TEST(LobbyRoutes, DiskForumRemovalDoesNotChangeThePublishedWorkspace) {
    test::TestWorkspace fixture;
    const LobbyGraph graph(fixture.root());
    const StoredSession stored = graph.sessions()->create("lobby", "Deleted forum");
    const std::filesystem::path forum_directory =
        fixture.root() / "forums" / "lobby";
    const WebSettings settings = lobby_settings(1);
    const SessionOpener open_real = graph.opener();
    LiveSessionManager manager(
        settings,
        [open_real, forum_directory](
            const FullSessionId& key,
            std::shared_ptr<WakeNotifier> notifier) {
            if (std::filesystem::remove_all(forum_directory) == 0) {
                throw std::runtime_error("Test forum was not deleted");
            }
            return open_real(key, notifier);
        });
    TestServer server(graph, manager, settings);

    const auto opened = server.client().Post(
        "/api/v1/forums/lobby/sessions/" + stored.identity.session_id + "/open",
        "{}",
        "application/json");
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened->status, 200);
    EXPECT_EQ(manager.snapshot().live_session_count, 1U);
    manager.begin_shutdown();
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

TEST(LobbyRoutes, NewStoredSessionSurvivesInitializationFailureAndCanBeRetried) {
    test::TestWorkspace fixture;
    const LobbyGraph graph(fixture.root());
    std::atomic<bool> fail{true};
    LiveSessionManager manager(
        lobby_settings(),
        [&fail, open = graph.opener()](
            const FullSessionId& key, std::shared_ptr<WakeNotifier> notifier) {
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
            const FullSessionId& key, std::shared_ptr<WakeNotifier> notifier) {
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
    // A generation that ignores cancellation keeps this actor's owner inside
    // SessionController::shutdown(), which is exactly the stopping state the
    // lobby must report.
    auto controls = std::make_shared<test::BackendControls>();
    std::atomic<int> starts{};
    LiveSessionManager manager(
        lobby_settings(1),
        [&starts, controls, database = graph.sessions()->database_path()](
            const FullSessionId& key, std::shared_ptr<WakeNotifier> notifier) {
            ++starts;
            return test::open_scripted_session(
                key,
                database,
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
        session->submit(RawCommand{"Question"}, 5s)));
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
