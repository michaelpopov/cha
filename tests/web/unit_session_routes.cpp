#include "web/asset_handler.h"
#include "web/http_server.h"
#include "web/live_session_manager.h"
#include "web/session_routes.h"
#include "support/test_workspace.h"

#include "session/session_repository.h"
#include "support/test_live_session.h"
#include "support/test_web_graph.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <variant>

namespace cha::web {
namespace {

using namespace std::chrono_literals;

// One temporary database per identity, so route tests drive a real controller
// without needing a whole workspace fixture behind every session.
class SessionFiles {
public:
    const std::filesystem::path& path_for(const SessionIdentity& key) {
        std::lock_guard lock(mutex_);
        auto found = files_.find(key);
        if (found == files_.end()) {
            found = files_.emplace(
                key,
                std::make_unique<test::TemporarySessionFile>("routes", key)).first;
        }
        return found->second->path();
    }

private:
    std::mutex mutex_;
    std::map<SessionIdentity, std::unique_ptr<test::TemporarySessionFile>> files_;
};

// A rendezvous the controller's activation hook enters on the owner thread,
// which is how these tests hold one command in flight.
class OwnerGate {
public:
    void wait() {
        std::unique_lock lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return released_; });
    }
    bool wait_until_entered(std::chrono::milliseconds timeout = 2s) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [this] { return entered_; });
    }
    void release() {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        changed_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_{};
    bool released_{};
};

SessionOpener session_opener(
    SessionFiles& files,
    std::shared_ptr<test::BackendControls> controls,
    SessionController::ActivationHook before_activation = {},
    std::atomic<int>* starts = nullptr) {
    return [&files, controls, before_activation, starts](
               const SessionIdentity& identity, WakeNotifier& notifier) {
        if (starts) ++*starts;
        return test::open_scripted_session(
            identity,
            files.path_for(identity),
            notifier,
            test::one_backend(test::scripted_backend(controls)),
            before_activation);
    };
}

class RouteServer {
public:
    RouteServer(LiveSessionManager& live_sessions, WebSettings settings = {}) {
        const AssetHandler assets(fixture_.root() / "web");
        assets.install(server_);
        SessionRoutes(live_sessions, settings, assets).install(server_);
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ < 0) throw std::runtime_error("Could not bind test server");
        configure_http_server(server_, settings);
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }
    ~RouteServer() {
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
    test::TestWorkspace fixture_;
    httplib::Server server_;
    int port_{};
    std::thread thread_;
};

nlohmann::json json_body(const httplib::Result& result) {
    EXPECT_TRUE(result);
    return result ? nlohmann::json::parse(result->body) : nlohmann::json{};
}

void expect_error(const httplib::Result& result, int status, std::string_view code) {
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, status);
    const nlohmann::json body = json_body(result);
    ASSERT_TRUE(body.contains("error"));
    EXPECT_EQ(body["error"].size(), 2);
    EXPECT_EQ(body["error"]["code"], code);
}

WebSettings route_settings(std::size_t session_limit = 1) {
    WebSettings settings;
    settings.session_limit = session_limit;
    settings.command_deadline = 2s;
    settings.open_deadline = 5s;
    return settings;
}

TEST(SessionRoutes, ServesLivePageSnapshotAndOwnerQueuedCommands) {
    SessionFiles files;
    auto controls = std::make_shared<test::BackendControls>();
    LiveSessionManager manager(route_settings(), session_opener(files, controls));
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(
        manager.open({"lobby", "one"}, 5s)));
    RouteServer server(manager, route_settings());
    const std::string base = "/s/lobby/one";

    const auto page = server.client().Get(base + "/");
    const auto root_page = server.client().Get("/");
    ASSERT_TRUE(page);
    ASSERT_TRUE(root_page);
    EXPECT_EQ(page->status, 200);
    EXPECT_EQ(page->body, root_page->body);
    EXPECT_EQ(page->get_header_value("Content-Type"), "text/html; charset=utf-8");
    EXPECT_NE(page->body.find("<title>cha</title>"), std::string::npos);

    const auto snapshot = server.client().Get(base + "/api/v1/session");
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->status, 200);
    EXPECT_EQ(json_body(snapshot)["session_id"], "one");
    EXPECT_EQ(json_body(snapshot)["forum"]["id"], "lobby");
    EXPECT_FALSE(json_body(snapshot).contains("error"));

    // A prompt reaches the controller through the owner queue and clears the
    // browser editor.
    const auto input = server.client().Post(
        base + "/api/v1/input",
        R"({"persona":"reader","text":"Question"})",
        "application/json");
    ASSERT_TRUE(input);
    EXPECT_EQ(input->status, 200);
    EXPECT_EQ(json_body(input)["clear_input"], true);
    ASSERT_TRUE(controls->wait_until_running());
    controls->finish();

    expect_error(
        server.client().Post(base + "/api/v1/input", R"({"text":"missing persona"})", "application/json"),
        400,
        "bad_request");
    expect_error(
        server.client().Post(base + "/api/v1/input", R"({"persona":"","text":"empty persona"})", "application/json"),
        400,
        "bad_request");
    // Roster membership is a controller decision, not a route one: the request
    // is well formed, so it is accepted and answered with a notice.
    const auto unknown_persona = server.client().Post(
        base + "/api/v1/input", R"({"persona":"not-in-roster","text":"passes through"})", "application/json");
    ASSERT_TRUE(unknown_persona);
    EXPECT_EQ(unknown_persona->status, 200);
    EXPECT_EQ(json_body(unknown_persona)["clear_input"], false);
    EXPECT_TRUE(json_body(unknown_persona).contains("notice"));

    const auto stop = server.client().Post(base + "/api/v1/actions/stop", "{}", "application/json");
    ASSERT_TRUE(stop);
    EXPECT_EQ(stop->status, 200);
    EXPECT_EQ(json_body(stop)["clear_input"], false);

    const auto character = server.client().Post(
        base + "/api/v1/actions/default-character",
        R"({"character_id":"guide"})",
        "application/json");
    ASSERT_TRUE(character);
    EXPECT_EQ(character->status, 200);
    const auto legacy_alias = server.client().Post(
        base + "/api/v1/actions/default-agent",
        R"({"character_id":"guide"})",
        "application/json");
    ASSERT_TRUE(legacy_alias);
    EXPECT_EQ(legacy_alias->status, 200);
    const auto after = server.client().Get(base + "/api/v1/session");
    EXPECT_EQ(json_body(after)["default_character_id"], "guide");
    manager.begin_shutdown();
}

TEST(SessionRoutes, EventsStartWithASnapshotAndIgnoreLastEventId) {
    SessionFiles files;
    auto controls = std::make_shared<test::BackendControls>();
    LiveSessionManager manager(route_settings(), session_opener(files, controls));
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(
        manager.open({"lobby", "one"}, 5s)));
    RouteServer server(manager, route_settings());

    int status{};
    std::string content;
    const httplib::Headers headers{{"Last-Event-ID", "999"}};
    (void)server.client().Get(
        "/s/lobby/one/api/v1/events", headers,
        [&status](const httplib::Response& response) {
            status = response.status;
            return true;
        },
        [&content](const char* data, std::size_t size) {
            content.append(data, size);
            return false; // Close after the initial record.
        });
    EXPECT_EQ(status, 200);
    EXPECT_TRUE(content.starts_with("event: snapshot\n"));
    EXPECT_NE(content.find("data: "), std::string::npos);
    const std::size_t record_end = content.find("\n\n");
    ASSERT_NE(record_end, std::string::npos);
    std::istringstream record(content.substr(0, record_end));
    for (std::string line; std::getline(record, line);) {
        EXPECT_TRUE(line.starts_with("event: ") || line.starts_with("data: "));
    }
    manager.begin_shutdown();
}

TEST(SessionRoutes, RejectsSecondEventStreamWithBrowserStreamInUse) {
    SessionFiles files;
    auto controls = std::make_shared<test::BackendControls>();
    LiveSessionManager manager(route_settings(), session_opener(files, controls));
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(
        manager.open({"lobby", "one"}, 5s)));
    RouteServer server(manager, route_settings());

    std::mutex stream_mutex;
    std::condition_variable stream_changed;
    bool received_snapshot = false;
    auto first_stream = std::async(std::launch::async, [&] {
        auto client = server.client();
        client.set_read_timeout(5s);
        return client.Get(
            "/s/lobby/one/api/v1/events",
            [](const httplib::Response& response) { return response.status == 200; },
            [&](const char*, std::size_t) {
                std::lock_guard lock(stream_mutex);
                received_snapshot = true;
                stream_changed.notify_all();
                return true;
            });
    });
    {
        std::unique_lock lock(stream_mutex);
        ASSERT_TRUE(stream_changed.wait_for(lock, 5s, [&] {
            return received_snapshot;
        }));
    }

    expect_error(
        server.client().Get("/s/lobby/one/api/v1/events"),
        409,
        "browser_stream_in_use");

    manager.begin_shutdown();
    ASSERT_EQ(first_stream.wait_for(5s), std::future_status::ready);
    (void)first_stream.get();
}

TEST(SessionRoutes, EventsDeliverAppendWithSequenceOverHttp) {
    SessionFiles files;
    auto controls = std::make_shared<test::BackendControls>();
    LiveSessionManager manager(route_settings(), session_opener(files, controls));
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(
        manager.open({"lobby", "one"}, 5s)));
    RouteServer server(manager, route_settings());

    std::mutex stream_mutex;
    std::condition_variable stream_changed;
    std::string content;
    auto stream_request = std::async(std::launch::async, [&] {
        auto client = server.client();
        client.set_read_timeout(5s);
        return client.Get(
            "/s/lobby/one/api/v1/events",
            [](const httplib::Response& response) {
                return response.status == 200;
            },
            [&](const char* data, std::size_t size) {
                std::lock_guard lock(stream_mutex);
                content.append(data, size);
                stream_changed.notify_all();
                return content.find("event: append\n") == std::string::npos;
            });
    });
    {
        std::unique_lock lock(stream_mutex);
        ASSERT_TRUE(stream_changed.wait_for(lock, 5s, [&] {
            return content.find("event: snapshot\n") != std::string::npos;
        }));
    }

    const auto input = server.client().Post(
        "/s/lobby/one/api/v1/input",
        R"({"persona":"reader","text":"Question"})",
        "application/json");
    ASSERT_TRUE(input);
    EXPECT_EQ(input->status, 200);
    ASSERT_TRUE(controls->wait_until_running());
    // The first fragment opens the answer entry, which is a structural change;
    // later fragments are exact appends against that base.
    for (int index = 0; index != 10; ++index) {
        controls->emit_answer("world ");
        std::this_thread::sleep_for(20ms);
        std::lock_guard lock(stream_mutex);
        if (content.find("event: append\n") != std::string::npos) break;
    }
    ASSERT_EQ(stream_request.wait_for(5s), std::future_status::ready);
    (void)stream_request.get();
    controls->finish();

    std::lock_guard lock(stream_mutex);
    const std::size_t append_record = content.find("event: append\n");
    ASSERT_NE(append_record, std::string::npos) << content;
    const std::size_t data = content.find("data: ", append_record);
    const std::size_t end = content.find("\n\n", data);
    ASSERT_NE(data, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    const nlohmann::json append =
        nlohmann::json::parse(content.substr(data + 6, end - data - 6));
    EXPECT_TRUE(append.contains("seq"));
    EXPECT_NE(append["text"].get<std::string>().find("world"), std::string::npos);
    EXPECT_EQ(append["target"]["kind"], "entry");
    manager.begin_shutdown();
}

TEST(SessionRoutes, DeliberateStreamCloseFinishesChunkedResponse) {
    SessionFiles files;
    auto controls = std::make_shared<test::BackendControls>();
    LiveSessionManager manager(route_settings(), session_opener(files, controls));
    const SessionIdentity key{"lobby", "one"};
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open(key, 5s)));
    LiveSessionHandle session = manager.lookup(key);
    ASSERT_TRUE(session);
    RouteServer server(manager, route_settings());

    std::mutex stream_mutex;
    std::condition_variable stream_changed;
    std::string content;
    auto stream_request = std::async(std::launch::async, [&] {
        auto client = server.client();
        client.set_read_timeout(5s);
        return client.Get(
            "/s/lobby/one/api/v1/events",
            [](const httplib::Response& response) {
                return response.status == 200;
            },
            [&](const char* data, std::size_t size) {
                std::lock_guard lock(stream_mutex);
                content.append(data, size);
                stream_changed.notify_all();
                return true;
            });
    });
    {
        std::unique_lock lock(stream_mutex);
        ASSERT_TRUE(stream_changed.wait_for(lock, 5s, [&] {
            return content.find("event: snapshot\n") != std::string::npos;
        }));
    }

    session->request_shutdown();
    ASSERT_EQ(stream_request.wait_for(5s), std::future_status::ready);
    const httplib::Result result = stream_request.get();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 200);
    {
        std::lock_guard lock(stream_mutex);
        EXPECT_NE(
            content.find(R"("lifecycle":"stopping")"),
            std::string::npos);
        EXPECT_NE(
            content.find(R"("shutdown_reason":"browser_disconnected")"),
            std::string::npos);
    }
    manager.begin_shutdown();
}

TEST(SessionRoutes, ServesWorkspaceMetadataAndReportsUnavailableMetadata) {
    test::TestWorkspace fixture;
    fixture.write_character_config(
        "display_name = \"Guide\"\n"
        "description = \"Explains the workspace\"\n");
    const test::WebGraph graph(fixture.root());
    const StoredSession stored = graph.sessions->create("lobby", "Named session");
    const WebSettings settings{
        .session_limit = 1,
        .open_deadline = 5s,
        .command_deadline = 2s,
    };
    LiveSessionManager manager(settings, graph.opener());

    const LiveSessionOpenResult unavailable = manager.open({"lobby", "missing"}, 5s);
    ASSERT_TRUE(std::holds_alternative<LiveSessionOpenFailure>(unavailable));
    EXPECT_EQ(
        std::get<LiveSessionOpenFailure>(unavailable),
        LiveSessionOpenFailure::not_found);

    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(
        manager.open(stored.identity, 5s)));
    RouteServer server(manager, settings);
    const auto snapshot = server.client().Get(
        "/s/lobby/" + stored.identity.session_id + "/api/v1/session");
    ASSERT_TRUE(snapshot);
    ASSERT_EQ(snapshot->status, 200);
    const nlohmann::json body = json_body(snapshot);
    EXPECT_EQ(body["forum"].at("id"), "lobby");
    EXPECT_EQ(body["forum"].at("display_name"), "The Lobby");
    EXPECT_EQ(body["forum"].at("default_character_id"), "guide");
    EXPECT_EQ(body["forum"].at("members"), nlohmann::json::array({{
        {"id", "guide"},
        {"display_name", "Guide"},
        {"description", "Explains the workspace"},
        {"appearance", {{"font", "sans"}, {"style", "normal"},
            {"weight", "normal"}, {"size", "normal"}}},
    }}));
    EXPECT_EQ(body["characters"], body["forum"].at("members"));
    EXPECT_EQ(body["session_id"], stored.identity.session_id);
    EXPECT_EQ(body["session_label"], "Named session");
    manager.begin_shutdown();
}

TEST(SessionRoutes, ServesTheShellForANonLiveSessionAndRejectsInvalidBodiesBeforeLookup) {
    SessionFiles files;
    auto controls = std::make_shared<test::BackendControls>();
    std::atomic<int> starts{};
    LiveSessionManager manager(
        route_settings(), session_opener(files, controls, {}, &starts));
    WebSettings settings = route_settings();
    settings.request_body_limit = 64;
    settings.prompt_limit = 8;
    RouteServer server(manager, settings);

    const auto page = server.client().Get("/s/lobby/missing/");
    ASSERT_TRUE(page);
    EXPECT_EQ(page->status, 200);
    EXPECT_NE(page->body.find("test shell"), std::string::npos);
    // The deep link is the same shell as '/', so it must carry the same policy
    // and caching. Asserting it here is what keeps the two from drifting.
    EXPECT_EQ(page->get_header_value("Cache-Control"), "no-cache");
    EXPECT_EQ(
        page->get_header_value("Content-Security-Policy"),
        "default-src 'none'; script-src 'self'; style-src 'self'; "
        "img-src 'self' data:; font-src 'self'; connect-src 'self'; "
        "base-uri 'none'; form-action 'none'; frame-ancestors 'none'");
    expect_error(server.client().Get("/s/lobby/missing/api/v1/session"), 409, "session_not_live");
    expect_error(server.client().Get("/s/%2e%2e/missing/api/v1/session"), 404, "not_found");
    expect_error(server.client().Get("/s/lobby/missing%23fragment/api/v1/session"), 404, "not_found");
    expect_error(server.client().Get("/s/lobby/missing%00suffix/api/v1/session"), 404, "not_found");
    expect_error(server.client().Post("/s/lobby/missing/api/v1/input", "{}", "text/plain"), 400, "bad_request");
    expect_error(server.client().Post("/s/lobby/missing/api/v1/input", R"({"persona":"u","text":"123456789"})", "application/json"), 413, "prompt_too_large");
    expect_error(server.client().Post("/s/lobby/missing/api/v1/actions/stop", R"({"extra":true})", "application/json"), 400, "bad_request");
    expect_error(server.client().Post("/s/lobby/missing/api/v1/actions/default-character", R"({"character_id":""})", "application/json"), 400, "bad_request");
    expect_error(server.client().Post("/s/lobby/missing/api/v1/close", "{}", "application/json"), 404, "not_found");
    expect_error(server.client().Get("/s/lobby/missing/api/v1/status"), 404, "not_found");
    EXPECT_EQ(starts, 0);
}

TEST(SessionRoutes, EnforcesOriginPolicyOnSessionMutations) {
    SessionFiles files;
    auto controls = std::make_shared<test::BackendControls>();
    LiveSessionManager manager(route_settings(), session_opener(files, controls));
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(
        manager.open({"lobby", "one"}, 5s)));
    RouteServer server(manager, route_settings());
    const std::string path = "/s/lobby/one/api/v1/input";

    expect_error(
        server.client().Post(
            path,
            httplib::Headers{{"Origin", "http://other.example"}},
            R"({"persona":"reader","text":"hello"})",
            "application/json"),
        403,
        "forbidden_origin");
    const auto matching = server.client().Post(
        path,
        httplib::Headers{
            {"Host", "localhost:" + std::to_string(server.port())},
            {"Origin", "https://localhost:" + std::to_string(server.port())},
        },
        R"({"persona":"reader","text":"matching"})",
        "application/json");
    ASSERT_TRUE(matching);
    EXPECT_EQ(matching->status, 200);
    ASSERT_TRUE(controls->wait_until_running());
    controls->finish();
    ASSERT_TRUE(controls->wait_until_idle());

    const auto without_origin = server.client().Post(
        path,
        R"({"persona":"reader","text":"hello"})",
        "application/json");
    ASSERT_TRUE(without_origin);
    EXPECT_EQ(without_origin->status, 200);
    manager.begin_shutdown();
}

TEST(SessionRoutes, AcceptsDnsHostForSnapshotsAndMutations) {
    SessionFiles files;
    auto controls = std::make_shared<test::BackendControls>();
    LiveSessionManager manager(route_settings(), session_opener(files, controls));
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(
        manager.open({"lobby", "one"}, 5s)));
    RouteServer server(manager, route_settings());
    const std::string base = "/s/lobby/one/api/v1/";
    const httplib::Headers rebound{
        {"Host", "evil.example:" + std::to_string(server.port())},
        {"Origin", "http://evil.example:" + std::to_string(server.port())},
    };

    const auto snapshot = server.client().Get(base + "session", rebound);
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->status, 200);
    const auto mutation = server.client().Post(
        base + "input", rebound,
        R"({"persona":"reader","text":"hostname"})", "application/json");
    ASSERT_TRUE(mutation);
    EXPECT_EQ(mutation->status, 200);
    ASSERT_TRUE(controls->wait_until_running());
    controls->finish();
    ASSERT_TRUE(controls->wait_until_idle());
    manager.begin_shutdown();
}

TEST(SessionRoutes, MapsAdmissionAndShutdownOutcomesWithoutExecutingRejectedCommands) {
    SessionFiles files;
    auto controls = std::make_shared<test::BackendControls>();
    OwnerGate gate;
    WebSettings full_settings = route_settings();
    full_settings.command_queue_capacity = 1;
    full_settings.command_deadline = 2s;
    LiveSessionManager full_manager(
        full_settings,
        session_opener(files, controls, [&gate](std::size_t) { gate.wait(); }));
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(
        full_manager.open({"lobby", "full"}, 5s)));
    RouteServer full_server(full_manager, full_settings);
    auto running = std::async(std::launch::async, [&] {
        return full_server.client().Post(
            "/s/lobby/full/api/v1/input",
            R"({"persona":"reader","text":"hold"})",
            "application/json");
    });
    ASSERT_TRUE(gate.wait_until_entered());
    auto queued = std::async(std::launch::async, [&] {
        return full_server.client().Post(
            "/s/lobby/full/api/v1/input",
            R"({"persona":"reader","text":"queued"})",
            "application/json");
    });
    EXPECT_EQ(queued.wait_for(50ms), std::future_status::timeout);
    expect_error(
        full_server.client().Post(
            "/s/lobby/full/api/v1/actions/stop", "{}", "application/json"),
        503,
        "command_queue_full");
    gate.release();
    ASSERT_TRUE(running.get());
    ASSERT_TRUE(queued.get());
    controls->finish();
    full_manager.begin_shutdown();

    SessionFiles stopping_files;
    auto stopping_controls = std::make_shared<test::BackendControls>();
    LiveSessionManager stopping_manager(
        route_settings(), session_opener(stopping_files, stopping_controls));
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(
        stopping_manager.open({"lobby", "stopping"}, 5s)));
    LiveSessionHandle stopping = stopping_manager.lookup({"lobby", "stopping"});
    ASSERT_TRUE(stopping);
    stopping->request_shutdown();
    RouteServer stopping_server(stopping_manager, route_settings());
    expect_error(
        stopping_server.client().Post(
            "/s/lobby/stopping/api/v1/actions/stop", "{}", "application/json"),
        409,
        "session_not_live");
    stopping_manager.begin_shutdown();
}

TEST(SessionRoutes, MapsProcessShutdownDrainToServerStopping) {
    SessionFiles files;
    auto controls = std::make_shared<test::BackendControls>();
    OwnerGate gate;
    WebSettings settings = route_settings();
    settings.command_queue_capacity = 2;
    LiveSessionManager manager(
        settings,
        session_opener(files, controls, [&gate](std::size_t) { gate.wait(); }));
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(
        manager.open({"lobby", "shutdown"}, 5s)));
    RouteServer server(manager, settings);

    auto running = std::async(std::launch::async, [&] {
        return server.client().Post(
            "/s/lobby/shutdown/api/v1/input",
            R"({"persona":"reader","text":"hold"})",
            "application/json");
    });
    ASSERT_TRUE(gate.wait_until_entered());
    auto queued = std::async(std::launch::async, [&] {
        return server.client().Post(
            "/s/lobby/shutdown/api/v1/actions/stop", "{}", "application/json");
    });
    ASSERT_EQ(queued.wait_for(50ms), std::future_status::timeout);
    auto shutting_down = std::async(std::launch::async, [&] {
        manager.begin_shutdown();
    });
    expect_error(
        server.client().Post(
            "/s/lobby/shutdown/api/v1/input",
            R"({"persona":"reader","text":"late"})",
            "application/json"),
        503,
        "server_stopping");
    gate.release();
    expect_error(queued.get(), 503, "server_stopping");
    ASSERT_TRUE(running.get());
    controls->finish();
    shutting_down.get();
}

TEST(SessionRoutes, TimeoutAndClientDisconnectLeaveAcceptedCommandRunning) {
    SessionFiles files;
    auto controls = std::make_shared<test::BackendControls>();
    OwnerGate gate;
    WebSettings settings = route_settings();
    settings.command_deadline = 10ms;
    LiveSessionManager manager(
        settings,
        session_opener(files, controls, [&gate](std::size_t) { gate.wait(); }));
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(
        manager.open({"lobby", "slow"}, 5s)));
    RouteServer server(manager, settings);
    const std::string path = "/s/lobby/slow/api/v1/input";

    expect_error(
        server.client().Post(path, R"({"persona":"reader","text":"timeout"})", "application/json"),
        503,
        "command_timeout");
    ASSERT_TRUE(gate.wait_until_entered());
    gate.release();
    ASSERT_TRUE(controls->wait_until_running());
    controls->finish();
    {
        WebSettings snapshot_settings = route_settings();
        RouteServer snapshot_server(manager, snapshot_settings);
        const auto snapshot =
            snapshot_server.client().Get("/s/lobby/slow/api/v1/session");
        ASSERT_TRUE(snapshot);
        EXPECT_EQ(snapshot->status, 200);
    }
    manager.begin_shutdown();

    // A request whose client gives up after enqueue still leaves its owning
    // reply with the actor. The handler may finish after the client is
    // gone without touching request-owned state.
    SessionFiles disconnect_files;
    auto disconnect_controls = std::make_shared<test::BackendControls>();
    OwnerGate disconnect_gate;
    WebSettings disconnect_settings = route_settings();
    LiveSessionManager disconnect_manager(
        disconnect_settings,
        session_opener(
            disconnect_files,
            disconnect_controls,
            [&disconnect_gate](std::size_t) { disconnect_gate.wait(); }));
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(
        disconnect_manager.open({"lobby", "disconnect"}, 5s)));
    RouteServer disconnect_server(disconnect_manager, disconnect_settings);
    auto disconnected = std::async(std::launch::async, [&] {
        auto client = disconnect_server.client();
        client.set_read_timeout(0, 20000);
        return client.Post(
            "/s/lobby/disconnect/api/v1/input",
            R"({"persona":"reader","text":"disconnect"})",
            "application/json");
    });
    ASSERT_TRUE(disconnect_gate.wait_until_entered());
    EXPECT_FALSE(disconnected.get());
    disconnect_gate.release();
    ASSERT_TRUE(disconnect_controls->wait_until_running());
    disconnect_controls->finish();
    const auto after_disconnect = disconnect_server.client().Get(
        "/s/lobby/disconnect/api/v1/session");
    ASSERT_TRUE(after_disconnect);
    EXPECT_EQ(after_disconnect->status, 200);
    disconnect_manager.begin_shutdown();
}

} // namespace
} // namespace cha::web
