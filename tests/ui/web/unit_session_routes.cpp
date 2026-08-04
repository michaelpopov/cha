#include "ui/web/asset_handler.h"
#include "ui/web/http_server.h"
#include "ui/web/session_registry.h"
#include "ui/web/session_routes.h"
#include "support/test_workspace.h"

#include "session/workspace.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

namespace cha::web {
namespace {

PortBackedSession fake_session(
    const SessionIdentity& identity,
    std::unique_ptr<WebSessionController> controller) {
    return {{identity, "Test forum " + identity.forum_id,
             "Test session " + identity.session_id}, std::move(controller)};
}

using namespace std::chrono_literals;

struct Calls {
    std::mutex mutex;
    std::string persona;
    std::string input;
    std::string agent;
    int stops{};
    bool block_input{};
    bool input_entered{};
    bool release_input{};
    int completed_inputs{};
    std::string transcript_text{"hello"};
    std::condition_variable input_changed;
};

class RouteController final : public WebSessionController {
public:
    explicit RouteController(std::shared_ptr<Calls> calls) : calls_(std::move(calls)) {}

    TextInputResult handle_raw_input(std::string_view author_id, std::string input) override {
        std::unique_lock lock(calls_->mutex);
        calls_->persona = std::string(author_id);
        calls_->input = std::move(input);
        if (calls_->block_input) {
            calls_->input_entered = true;
            calls_->input_changed.notify_all();
            calls_->input_changed.wait(lock, [this] {
                return calls_->release_input;
            });
        }
        ++calls_->completed_inputs;
        calls_->input_changed.notify_all();
        if (calls_->input == "rejected") return {};
        if (calls_->input == "append") {
            calls_->transcript_text.append(" world");
            return {.session = {.state_changed = true}, .clear_input = true};
        }
        return {.session = {.notice = "input accepted"}, .clear_input = true};
    }
    SessionChange request_stop() override {
        std::lock_guard lock(calls_->mutex);
        ++calls_->stops;
        return {.notice = "stop requested"};
    }
    SessionChange set_default_agent_id(std::string_view id) override {
        std::lock_guard lock(calls_->mutex);
        calls_->agent = std::string(id);
        return {};
    }
    SessionEventBatch receive(std::size_t) override { return {}; }
    [[nodiscard]] bool is_generating() const override { return false; }
    SessionState state() override {
        std::lock_guard lock(calls_->mutex);
        return {
            .characters = {{"guide", "Guide"}},
            .default_agent_id = "guide",
            .transcript = {{
                .id = 1,
                .kind = EntryKind::agent,
                .display_name = "Guide",
                .text = calls_->transcript_text,
                .status = EntryStatus::streaming,
                .request_id = 1,
            }},
            .revision = 1,
            .open_entry_id = 1,
            .generation = {.active = true, .request_id = 1,
                           .agent_id = "guide", .agent_name = "Guide",
                           .phase = ResponsePhase::answering},
        };
    }
    std::optional<SessionAppendProjection> text_append_since(
        const SessionStateCursor& before) override {
        std::lock_guard lock(calls_->mutex);
        if (before.phase != ResponsePhase::answering
            || calls_->transcript_text.size()
                <= before.answer_length) {
            return std::nullopt;
        }
        SessionStateCursor cursor = before;
        cursor.answer_length = calls_->transcript_text.size();
        return SessionAppendProjection{
            .append = {EntryTextTarget{1}, calls_->transcript_text.substr(before.answer_length)},
            .cursor = std::move(cursor)};
    }
    void shutdown() override {}

private:
    std::shared_ptr<Calls> calls_;
};

class RouteServer {
public:
    RouteServer(SessionRegistry& registry, WebSettings settings = {}) {
        AssetHandler().install(server_);
        SessionRoutes(registry, settings).install(server_);
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ < 0) throw std::runtime_error("Could not bind test server");
        configure_http_server(server_, settings, "127.0.0.1", port_);
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

TEST(SessionRoutes, ServesLivePageSnapshotAndOwnerQueuedCommands) {
    const auto calls = std::make_shared<Calls>();
    SessionRegistry registry({.session_limit = 1}, [calls](const SessionIdentity& key, WakeNotifier&) {
        return fake_session(key, std::make_unique<RouteController>(calls));
    });
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(registry.open({"lobby", "one"}, 1s)));
    RouteServer server(registry);
    const std::string base = "/s/lobby/one";

    const auto page = server.client().Get(base + "/");
    ASSERT_TRUE(page);
    EXPECT_EQ(page->status, 200);
    EXPECT_EQ(page->get_header_value("Content-Type"), "text/html; charset=utf-8");
    EXPECT_NE(page->body.find("<title>cha session</title>"), std::string::npos);
    EXPECT_EQ(page->body.find("Session is not open"), std::string::npos);

    const auto snapshot = server.client().Get(base + "/api/v1/session");
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->status, 200);
    EXPECT_EQ(json_body(snapshot)["session_id"], "one");
    EXPECT_EQ(json_body(snapshot)["forum"]["id"], "lobby");
    EXPECT_FALSE(json_body(snapshot).contains("error"));

    const auto input = server.client().Post(base + "/api/v1/input", R"({"persona":"reader","text":"/@Guide"})", "application/json");
    ASSERT_TRUE(input);
    EXPECT_EQ(input->status, 200);
    EXPECT_EQ(json_body(input), nlohmann::json({{"clear_input", true}, {"notice", "input accepted"}}));
    const auto rejected = server.client().Post(
        base + "/api/v1/input", R"({"persona":"reader","text":"rejected"})", "application/json");
    ASSERT_TRUE(rejected);
    EXPECT_EQ(rejected->status, 200);
    EXPECT_EQ(json_body(rejected), nlohmann::json({{"clear_input", false}}));
    expect_error(
        server.client().Post(base + "/api/v1/input", R"({"text":"missing persona"})", "application/json"),
        400,
        "bad_request");
    expect_error(
        server.client().Post(base + "/api/v1/input", R"({"persona":"","text":"empty persona"})", "application/json"),
        400,
        "bad_request");
    const auto unknown_persona = server.client().Post(
        base + "/api/v1/input", R"({"persona":"not-in-roster","text":"passes through"})", "application/json");
    ASSERT_TRUE(unknown_persona);
    EXPECT_EQ(unknown_persona->status, 200);
    {
        std::lock_guard lock(calls->mutex);
        EXPECT_EQ(calls->persona, "not-in-roster");
        EXPECT_EQ(calls->input, "passes through");
    }
    const auto stop = server.client().Post(base + "/api/v1/actions/stop", "{}", "application/json");
    ASSERT_TRUE(stop);
    EXPECT_EQ(stop->status, 200);
    EXPECT_EQ(json_body(stop)["clear_input"], false);
    EXPECT_EQ(json_body(stop)["notice"], "stop requested");
    const auto agent = server.client().Post(base + "/api/v1/actions/default-agent", R"({"character_id":"guide"})", "application/json");
    ASSERT_TRUE(agent);
    EXPECT_EQ(agent->status, 200);
    EXPECT_EQ(json_body(agent), nlohmann::json({{"clear_input", false}}));
    {
        std::lock_guard lock(calls->mutex);
        EXPECT_EQ(calls->persona, "not-in-roster");
        EXPECT_EQ(calls->input, "passes through");
        EXPECT_EQ(calls->stops, 1);
        EXPECT_EQ(calls->agent, "guide");
    }
    registry.begin_shutdown();
}

TEST(SessionRoutes, EventsStartWithASnapshotAndIgnoreLastEventId) {
    const auto calls = std::make_shared<Calls>();
    SessionRegistry registry({.session_limit = 1}, [calls](const SessionIdentity& key, WakeNotifier&) {
        return fake_session(key, std::make_unique<RouteController>(calls));
    });
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(registry.open({"lobby", "one"}, 1s)));
    RouteServer server(registry);

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
    registry.begin_shutdown();
}

TEST(SessionRoutes, RejectsSecondEventStreamWithBrowserStreamInUse) {
    const auto calls = std::make_shared<Calls>();
    SessionRegistry registry({.session_limit = 1}, [calls](const SessionIdentity& key, WakeNotifier&) {
        return fake_session(key, std::make_unique<RouteController>(calls));
    });
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
        registry.open({"lobby", "one"}, 1s)));
    RouteServer server(registry);

    std::mutex stream_mutex;
    std::condition_variable stream_changed;
    bool received_snapshot = false;
    auto first_stream = std::async(std::launch::async, [&] {
        auto client = server.client();
        client.set_read_timeout(2s);
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
        ASSERT_TRUE(stream_changed.wait_for(lock, 1s, [&] {
            return received_snapshot;
        }));
    }

    expect_error(
        server.client().Get("/s/lobby/one/api/v1/events"),
        409,
        "browser_stream_in_use");

    registry.begin_shutdown();
    ASSERT_EQ(first_stream.wait_for(2s), std::future_status::ready);
    (void)first_stream.get();
}

TEST(SessionRoutes, EventsDeliverAppendWithSequenceOverHttp) {
    const auto calls = std::make_shared<Calls>();
    SessionRegistry registry(
        {.session_limit = 1},
        [calls](const SessionIdentity& key, WakeNotifier&) {
            return fake_session(key, std::make_unique<RouteController>(calls));
        });
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
        registry.open({"lobby", "one"}, 1s)));
    RouteServer server(registry);

    std::mutex stream_mutex;
    std::condition_variable stream_changed;
    std::string content;
    auto stream_request = std::async(std::launch::async, [&] {
        auto client = server.client();
        client.set_read_timeout(2s);
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
        ASSERT_TRUE(stream_changed.wait_for(lock, 1s, [&] {
            return content.find("event: snapshot\n") != std::string::npos;
        }));
    }

    const auto input = server.client().Post(
        "/s/lobby/one/api/v1/input",
        // SessionRoutes checks wire shape only; roster membership belongs to
        // the controller.
        R"({"persona":"not-a-persona","text":"append"})",
        "application/json");
    ASSERT_TRUE(input);
    EXPECT_EQ(input->status, 200);
    ASSERT_EQ(stream_request.wait_for(1s), std::future_status::ready);
    (void)stream_request.get();

    std::lock_guard lock(stream_mutex);
    const std::size_t append_record = content.find("event: append\n");
    ASSERT_NE(append_record, std::string::npos);
    const std::size_t data = content.find("data: ", append_record);
    const std::size_t end = content.find("\n\n", data);
    ASSERT_NE(data, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    const nlohmann::json append =
        nlohmann::json::parse(content.substr(data + 6, end - data - 6));
    EXPECT_EQ(append["seq"], 0);
    EXPECT_EQ(append["text"], " world");
    EXPECT_EQ(append["target"], nlohmann::json({
        {"kind", "entry"}, {"entry_id", 1}}));
    registry.begin_shutdown();
}

TEST(SessionRoutes, DeliberateStreamCloseFinishesChunkedResponse) {
    const auto calls = std::make_shared<Calls>();
    SessionRegistry registry(
        {.session_limit = 1},
        [calls](const SessionIdentity& key, WakeNotifier&) {
            return fake_session(key, std::make_unique<RouteController>(calls));
        });
    const SessionIdentity key{"lobby", "one"};
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
        registry.open(key, 1s)));
    SessionHandle handle = registry.lookup(key);
    ASSERT_TRUE(handle);
    RouteServer server(registry);

    std::mutex stream_mutex;
    std::condition_variable stream_changed;
    std::string content;
    auto stream_request = std::async(std::launch::async, [&] {
        auto client = server.client();
        client.set_read_timeout(2s);
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
        ASSERT_TRUE(stream_changed.wait_for(lock, 1s, [&] {
            return content.find("event: snapshot\n") != std::string::npos;
        }));
    }

    handle.runtime().request_shutdown();
    ASSERT_EQ(stream_request.wait_for(2s), std::future_status::ready);
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
    registry.begin_shutdown();
}

TEST(SessionRoutes, ServesWorkspaceMetadataAndReportsUnavailableMetadata) {
    test::TestWorkspace fixture;
    auto mutable_workspace = std::make_shared<Workspace>(fixture.root());
    const SessionSummary stored =
        mutable_workspace->create_stored_session("lobby", "Named session");
    const std::shared_ptr<const Workspace> workspace = mutable_workspace;
    const WebSettings settings{
        .session_limit = 1,
        .open_deadline = 1s,
        .command_deadline = 1s,
    };
    SessionRegistry registry =
        SessionRegistry::from_workspace(settings, workspace);

    const RegistryOpenResult unavailable =
        registry.open({"lobby", "missing"}, 1s);
    ASSERT_TRUE(std::holds_alternative<Error>(unavailable));
    EXPECT_EQ(std::get<Error>(unavailable).code, ErrorCode::not_found);

    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
        registry.open({"lobby", stored.id}, 1s)));
    RouteServer server(registry, settings);
    const auto snapshot = server.client().Get(
        "/s/lobby/" + stored.id + "/api/v1/session");
    ASSERT_TRUE(snapshot);
    ASSERT_EQ(snapshot->status, 200);
    const nlohmann::json body = json_body(snapshot);
    EXPECT_EQ(
        body["forum"],
        nlohmann::json({{"id", "lobby"}, {"display_name", "The Lobby"}}));
    EXPECT_EQ(body["session_id"], stored.id);
    EXPECT_EQ(body["session_label"], "Named session");
    registry.begin_shutdown();
}

TEST(SessionRoutes, SeparatesNonLivePageFromApiAndRejectsInvalidBodiesBeforeLookup) {
    std::atomic<int> starts{};
    SessionRegistry registry({.session_limit = 1}, [&starts](const SessionIdentity& key, WakeNotifier&) {
        ++starts;
        return fake_session(key, std::make_unique<RouteController>(std::make_shared<Calls>()));
    });
    RouteServer server(registry, {.request_body_limit = 64, .prompt_limit = 8});

    const auto page = server.client().Get("/s/lobby/missing/");
    ASSERT_TRUE(page);
    EXPECT_EQ(page->status, 200);
    EXPECT_NE(page->body.find("href=\"/\""), std::string::npos);
    expect_error(server.client().Get("/s/lobby/missing/api/v1/session"), 409, "session_not_live");
    expect_error(server.client().Get("/s/%2e%2e/missing/api/v1/session"), 404, "not_found");
    expect_error(server.client().Get("/s/lobby/missing%23fragment/api/v1/session"), 404, "not_found");
    expect_error(server.client().Get("/s/lobby/missing%00suffix/api/v1/session"), 404, "not_found");
    expect_error(server.client().Post("/s/lobby/missing/api/v1/input", "{}", "text/plain"), 400, "bad_request");
    expect_error(server.client().Post("/s/lobby/missing/api/v1/input", R"({"persona":"u","text":"123456789"})", "application/json"), 413, "prompt_too_large");
    expect_error(server.client().Post("/s/lobby/missing/api/v1/actions/stop", R"({"extra":true})", "application/json"), 400, "bad_request");
    expect_error(server.client().Post("/s/lobby/missing/api/v1/actions/default-agent", R"({"character_id":""})", "application/json"), 400, "bad_request");
    expect_error(server.client().Post("/s/lobby/missing/api/v1/close", "{}", "application/json"), 404, "not_found");
    expect_error(server.client().Get("/s/lobby/missing/api/v1/status"), 404, "not_found");
    EXPECT_EQ(starts, 0);
}

TEST(SessionRoutes, EnforcesOriginPolicyOnSessionMutations) {
    const auto calls = std::make_shared<Calls>();
    SessionRegistry registry({.session_limit = 1}, [calls](const SessionIdentity& key, WakeNotifier&) {
        return fake_session(key, std::make_unique<RouteController>(calls));
    });
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(registry.open({"lobby", "one"}, 1s)));
    RouteServer server(registry);
    const std::string path = "/s/lobby/one/api/v1/input";

    expect_error(
        server.client().Post(
            path,
            httplib::Headers{{"Origin", "http://other.example"}},
            R"({"persona":"reader","text":"hello"})",
            "application/json"),
        403,
        "forbidden_origin");
    {
        std::lock_guard lock(calls->mutex);
        EXPECT_TRUE(calls->input.empty());
    }
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
    const auto without_origin = server.client().Post(
        path,
        R"({"persona":"reader","text":"hello"})",
        "application/json");
    ASSERT_TRUE(without_origin);
    EXPECT_EQ(without_origin->status, 200);
    registry.begin_shutdown();
}

TEST(SessionRoutes, RejectsDnsRebindingHostBeforeSnapshotsStreamsAndMutations) {
    const auto calls = std::make_shared<Calls>();
    SessionRegistry registry({.session_limit = 1}, [calls](const SessionIdentity& key, WakeNotifier&) {
        return fake_session(key, std::make_unique<RouteController>(calls));
    });
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
        registry.open({"lobby", "one"}, 1s)));
    RouteServer server(registry);
    const std::string base = "/s/lobby/one/api/v1/";
    const httplib::Headers rebound{
        {"Host", "evil.example:" + std::to_string(server.port())},
        {"Origin", "http://evil.example:" + std::to_string(server.port())},
    };

    expect_error(
        server.client().Get(base + "session", rebound),
        403,
        "forbidden_host");
    expect_error(
        server.client().Get(base + "events", rebound),
        403,
        "forbidden_host");
    expect_error(
        server.client().Post(
            base + "input", rebound, R"({"persona":"reader","text":"rebound"})", "application/json"),
        403,
        "forbidden_host");
    {
        std::lock_guard lock(calls->mutex);
        EXPECT_TRUE(calls->input.empty());
    }
    registry.begin_shutdown();
}

TEST(SessionRoutes, MapsAdmissionAndShutdownOutcomesWithoutExecutingRejectedCommands) {
    const auto full_calls = std::make_shared<Calls>();
    full_calls->block_input = true;
    WebSettings full_settings{
        .session_limit = 1,
        .command_queue_capacity = 1,
        .command_deadline = 1s,
    };
    SessionRegistry full_registry(full_settings, [full_calls](const SessionIdentity& key, WakeNotifier&) {
        return fake_session(key, std::make_unique<RouteController>(full_calls));
    });
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(full_registry.open({"lobby", "full"}, 1s)));
    RouteServer full_server(full_registry, full_settings);
    auto running = std::async(std::launch::async, [&] {
        return full_server.client().Post(
            "/s/lobby/full/api/v1/input",
            R"({"persona":"reader","text":"hold"})",
            "application/json");
    });
    {
        std::unique_lock lock(full_calls->mutex);
        ASSERT_TRUE(full_calls->input_changed.wait_for(lock, 1s, [&] {
            return full_calls->input_entered;
        }));
    }
    auto queued = std::async(std::launch::async, [&] {
        return full_server.client().Post(
            "/s/lobby/full/api/v1/input",
            R"({"persona":"reader","text":"queued"})",
            "application/json");
    });
    EXPECT_EQ(queued.wait_for(20ms), std::future_status::timeout);
    expect_error(
        full_server.client().Post(
            "/s/lobby/full/api/v1/actions/stop", "{}", "application/json"),
        503,
        "command_queue_full");
    {
        std::lock_guard lock(full_calls->mutex);
        EXPECT_EQ(full_calls->stops, 0);
        full_calls->release_input = true;
    }
    full_calls->input_changed.notify_all();
    ASSERT_TRUE(running.get());
    ASSERT_TRUE(queued.get());
    full_registry.begin_shutdown();

    const auto stopping_calls = std::make_shared<Calls>();
    SessionRegistry stopping_registry({.session_limit = 1}, [stopping_calls](const SessionIdentity& key, WakeNotifier&) {
        return fake_session(key, std::make_unique<RouteController>(stopping_calls));
    });
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(stopping_registry.open({"lobby", "stopping"}, 1s)));
    const SessionHandle handle = stopping_registry.lookup({"lobby", "stopping"});
    ASSERT_TRUE(handle);
    handle.runtime().request_shutdown();
    RouteServer stopping_server(stopping_registry);
    expect_error(
        stopping_server.client().Post(
            "/s/lobby/stopping/api/v1/actions/stop", "{}", "application/json"),
        409,
        "session_not_live");
    {
        std::lock_guard lock(stopping_calls->mutex);
        EXPECT_EQ(stopping_calls->stops, 0);
    }
    stopping_registry.begin_shutdown();
}

TEST(SessionRoutes, MapsProcessShutdownDrainToServerStopping) {
    const auto calls = std::make_shared<Calls>();
    calls->block_input = true;
    WebSettings settings{
        .session_limit = 1,
        .command_queue_capacity = 2,
        .command_deadline = 1s,
    };
    SessionRegistry registry(settings, [calls](const SessionIdentity& key, WakeNotifier&) {
        return fake_session(key, std::make_unique<RouteController>(calls));
    });
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(registry.open({"lobby", "shutdown"}, 1s)));
    RouteServer server(registry, settings);

    auto running = std::async(std::launch::async, [&] {
        return server.client().Post(
            "/s/lobby/shutdown/api/v1/input",
            R"({"persona":"reader","text":"hold"})",
            "application/json");
    });
    {
        std::unique_lock lock(calls->mutex);
        ASSERT_TRUE(calls->input_changed.wait_for(lock, 1s, [&] {
            return calls->input_entered;
        }));
    }
    auto queued = std::async(std::launch::async, [&] {
        return server.client().Post(
            "/s/lobby/shutdown/api/v1/actions/stop", "{}", "application/json");
    });
    ASSERT_EQ(queued.wait_for(20ms), std::future_status::timeout);
    registry.begin_shutdown();
    expect_error(
        server.client().Post(
            "/s/lobby/shutdown/api/v1/input",
            R"({"persona":"reader","text":"late"})",
            "application/json"),
        503,
        "server_stopping");
    {
        std::lock_guard lock(calls->mutex);
        calls->release_input = true;
    }
    calls->input_changed.notify_all();
    expect_error(queued.get(), 503, "server_stopping");
    ASSERT_TRUE(running.get());
}

TEST(SessionRoutes, TimeoutAndClientDisconnectLeaveAcceptedCommandRunning) {
    const auto calls = std::make_shared<Calls>();
    calls->block_input = true;
    WebSettings settings{
        .session_limit = 1,
        .command_deadline = 10ms,
    };
    SessionRegistry registry(settings, [calls](const SessionIdentity& key, WakeNotifier&) {
        return fake_session(key, std::make_unique<RouteController>(calls));
    });
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(registry.open({"lobby", "slow"}, 1s)));
    RouteServer server(registry, settings);
    const std::string path = "/s/lobby/slow/api/v1/input";

    expect_error(
        server.client().Post(path, R"({"persona":"reader","text":"timeout"})", "application/json"),
        503,
        "command_timeout");
    {
        std::unique_lock lock(calls->mutex);
        ASSERT_TRUE(calls->input_changed.wait_for(lock, 1s, [&] {
            return calls->input_entered;
        }));
        calls->release_input = true;
    }
    calls->input_changed.notify_all();
    {
        std::unique_lock lock(calls->mutex);
        ASSERT_TRUE(calls->input_changed.wait_for(lock, 1s, [&] {
            return calls->completed_inputs == 1;
        }));
    }
    {
        RouteServer snapshot_server(registry);
        const auto snapshot =
            snapshot_server.client().Get("/s/lobby/slow/api/v1/session");
        ASSERT_TRUE(snapshot);
        EXPECT_EQ(snapshot->status, 200);
    }

    registry.begin_shutdown();

    // A request whose client gives up after enqueue still leaves its owning
    // completion with the runtime. The handler may finish after the client is
    // gone without touching request-owned state.
    const auto disconnect_calls = std::make_shared<Calls>();
    disconnect_calls->block_input = true;
    WebSettings disconnect_settings{
        .session_limit = 1,
        .command_deadline = 1s,
    };
    SessionRegistry disconnect_registry(disconnect_settings, [disconnect_calls](const SessionIdentity& key, WakeNotifier&) {
        return fake_session(key, std::make_unique<RouteController>(disconnect_calls));
    });
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(disconnect_registry.open({"lobby", "disconnect"}, 1s)));
    RouteServer disconnect_server(disconnect_registry, disconnect_settings);
    auto disconnected = std::async(std::launch::async, [&] {
        auto client = disconnect_server.client();
        client.set_read_timeout(0, 20000);
        return client.Post(
            "/s/lobby/disconnect/api/v1/input",
            R"({"persona":"reader","text":"disconnect"})",
            "application/json");
    });
    {
        std::unique_lock lock(disconnect_calls->mutex);
        ASSERT_TRUE(disconnect_calls->input_changed.wait_for(lock, 1s, [&] {
            return disconnect_calls->input_entered;
        }));
    }
    EXPECT_FALSE(disconnected.get());
    {
        std::lock_guard lock(disconnect_calls->mutex);
        disconnect_calls->release_input = true;
    }
    disconnect_calls->input_changed.notify_all();
    {
        std::unique_lock lock(disconnect_calls->mutex);
        ASSERT_TRUE(disconnect_calls->input_changed.wait_for(lock, 1s, [&] {
            return disconnect_calls->completed_inputs == 1;
        }));
    }
    const auto after_disconnect = disconnect_server.client().Get(
        "/s/lobby/disconnect/api/v1/session");
    ASSERT_TRUE(after_disconnect);
    EXPECT_EQ(after_disconnect->status, 200);
    disconnect_registry.begin_shutdown();
}

} // namespace
} // namespace cha::web
