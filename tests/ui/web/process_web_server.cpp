#include "support/mock_http_server.h"
#include "support/test_workspace.h"
#include "support/web_server_process.h"
#include "session/workspace.h"
#include "ui/web/http_server.h"
#include "ui/web/lobby_routes.h"
#include "ui/web/server_shutdown.h"
#include "util/logging.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <csignal>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

namespace cha::web {
namespace {

using namespace std::chrono_literals;

httplib::Client web_client(int port) {
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(1s);
    client.set_read_timeout(2s);
    return client;
}

void expect_same_origin_payload(const nlohmann::json& value) {
    if (value.is_array()) {
        for (const auto& child : value) expect_same_origin_payload(child);
        return;
    }
    if (value.is_object()) {
        for (const auto& [key, child] : value.items()) {
            EXPECT_NE(key, "host");
            EXPECT_NE(key, "port");
            EXPECT_NE(key, "url");
            EXPECT_NE(key, "lobby_address");
            if (key == "path" && child.is_string()) {
                const std::string path = child.get<std::string>();
                EXPECT_TRUE(path.starts_with('/'));
                EXPECT_FALSE(path.starts_with("//"));
            }
            expect_same_origin_payload(child);
        }
        return;
    }
    if (value.is_string()) {
        const std::string text = value.get<std::string>();
        EXPECT_EQ(text.find("://"), std::string::npos);
    }
}

std::string create_session(httplib::Client& client, std::string_view label) {
    const auto created = client.Post(
        "/api/v1/forums/lobby/sessions",
        std::string(R"({"label":")") + std::string(label) + R"("})",
        "application/json");
    EXPECT_TRUE(created);
    if (!created) return {};
    EXPECT_EQ(created->status, 201) << created->body;
    if (created->status != 201) return {};
    return nlohmann::json::parse(created->body).at("id").get<std::string>();
}

std::string open_session(httplib::Client& client, std::string_view id) {
    const auto opened = client.Post(
        "/api/v1/forums/lobby/sessions/" + std::string(id) + "/open",
        "{}",
        "application/json");
    EXPECT_TRUE(opened);
    if (!opened) return {};
    EXPECT_EQ(opened->status, 200) << opened->body;
    if (opened->status != 200) return {};
    const std::string path =
        nlohmann::json::parse(opened->body).at("path").get<std::string>();
    EXPECT_TRUE(path.starts_with('/'));
    EXPECT_FALSE(path.starts_with("//"));
    EXPECT_EQ(path.find("://"), std::string::npos);
    return path;
}

class StreamingRequest {
public:
    StreamingRequest(int port, std::string path)
        : client_("127.0.0.1", port) {
        client_.set_connection_timeout(1s);
        client_.set_read_timeout(30s);
        request_ = std::async(std::launch::async, [this, path = std::move(path)] {
            return client_.Get(
                path,
                [](const httplib::Response& response) {
                    return response.status == 200;
                },
                [this](const char* data, std::size_t size) {
                    {
                        std::lock_guard lock(mutex_);
                        content_.append(data, size);
                    }
                    changed_.notify_all();
                    return true;
                });
        });
    }

    ~StreamingRequest() {
        client_.stop();
        if (request_.valid()) request_.wait();
    }

    StreamingRequest(const StreamingRequest&) = delete;
    StreamingRequest& operator=(const StreamingRequest&) = delete;

    [[nodiscard]] bool wait_for_snapshot(
        std::chrono::milliseconds timeout = 2s) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [this] {
            return content_.find("event: snapshot\n") != std::string::npos;
        });
    }

    [[nodiscard]] bool wait_for_end(
        std::chrono::milliseconds timeout = 2s) {
        return request_.wait_for(timeout) == std::future_status::ready;
    }

private:
    httplib::Client client_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::string content_;
    std::future<httplib::Result> request_;
};

class PermanentlyBlockedShutdownController final
    : public WebSessionController {
public:
    SessionUpdate handle_raw_input(std::string) override { return {}; }
    SessionUpdate request_stop() override { return {}; }
    SessionUpdate set_default_agent_id(std::string_view) override { return {}; }
    SessionEventBatch receive(std::size_t) override { return {}; }
    [[nodiscard]] bool is_generating() const override { return false; }
    void shutdown() override {
        std::unique_lock lock(mutex_);
        changed_.wait(lock, [] { return false; });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
};

class OpeningGate {
public:
    void wait() {
        std::unique_lock lock(mutex_);
        entered_ = true;
        changed_.notify_all();
        changed_.wait(lock, [this] { return released_; });
    }

    [[nodiscard]] bool wait_until_entered(
        std::chrono::milliseconds timeout = 1s) {
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

class ReleaseOpeningGateOnExit {
public:
    explicit ReleaseOpeningGateOnExit(OpeningGate& gate) : gate_(gate) {}
    ~ReleaseOpeningGateOnExit() { gate_.release(); }

private:
    OpeningGate& gate_;
};

class IdleController final : public WebSessionController {
public:
    SessionUpdate handle_raw_input(std::string) override { return {}; }
    SessionUpdate request_stop() override { return {}; }
    SessionUpdate set_default_agent_id(std::string_view) override { return {}; }
    SessionEventBatch receive(std::size_t) override { return {}; }
    [[nodiscard]] bool is_generating() const override { return false; }
    void shutdown() override {}
};

void run_blocked_shutdown(const std::filesystem::path& log_path) {
    // A policy regression must fail this death test quickly instead of
    // inheriting ctest's much larger timeout.
    (void)::alarm(2);
    shutdown_diagnostic_logging();
    initialize_diagnostic_logging(log_path, "critical");
    SessionRegistry registry(
        {.session_limit = 1},
        [](const SessionKey&, WakeNotifier&) {
            return std::make_unique<PermanentlyBlockedShutdownController>();
        });
    if (!std::holds_alternative<OpenSessionSuccess>(
            registry.open({"blocked-forum", "blocked-session"}, 500ms))) {
        _exit(2);
    }
    httplib::Server server;
    ServerShutdownCoordinator coordinator(registry, server);
    std::thread listener;
    coordinator.shutdown_now(listener, 20ms);
    _exit(3);
}

TEST(WebServerProcess, ServesConcurrentSseAndOrdinaryRequestsOnOneOrigin) {
    test::TestWorkspace workspace;
    const int port = test::reserve_loopback_port();
    ASSERT_NE(port, 0);
    workspace.write_app_config(port);
    test::WebServerProcess server(workspace.root(), port);
    ASSERT_TRUE(server.wait_until_ready()) << server.errors();

    httplib::Client client = web_client(port);
    const auto lobby = client.Get("/");
    ASSERT_TRUE(lobby);
    EXPECT_EQ(lobby->status, 200);
    EXPECT_EQ(lobby->body.find("http://"), std::string::npos);
    EXPECT_EQ(lobby->body.find("https://"), std::string::npos);
    const auto forums = client.Get("/api/v1/forums");
    ASSERT_TRUE(forums);
    ASSERT_EQ(forums->status, 200);
    expect_same_origin_payload(nlohmann::json::parse(forums->body));

    const std::string first = create_session(client, "First");
    const std::string second = create_session(client, "Second");
    ASSERT_FALSE(first.empty());
    ASSERT_FALSE(second.empty());
    const auto sessions = client.Get("/api/v1/forums/lobby/sessions");
    ASSERT_TRUE(sessions);
    ASSERT_EQ(sessions->status, 200);
    expect_same_origin_payload(nlohmann::json::parse(sessions->body));
    const std::string first_path = open_session(client, first);
    const std::string second_path = open_session(client, second);
    ASSERT_EQ(first_path, "/s/lobby/" + first + "/");
    ASSERT_EQ(second_path, "/s/lobby/" + second + "/");

    StreamingRequest first_stream(
        port, first_path + "api/v1/events");
    StreamingRequest second_stream(
        port, second_path + "api/v1/events");
    ASSERT_TRUE(first_stream.wait_for_snapshot());
    ASSERT_TRUE(second_stream.wait_for_snapshot());

    const auto snapshot = client.Get(first_path + "api/v1/session");
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->status, 200);
    EXPECT_EQ(
        nlohmann::json::parse(snapshot->body).at("forum").at("id"),
        "lobby");
    expect_same_origin_payload(nlohmann::json::parse(snapshot->body));
    const auto page = client.Get(first_path);
    ASSERT_TRUE(page);
    EXPECT_EQ(page->status, 200);
    EXPECT_EQ(page->body.find("://"), std::string::npos);
    const auto not_open_page = client.Get("/s/lobby/not-open/");
    ASSERT_TRUE(not_open_page);
    EXPECT_EQ(not_open_page->status, 200);
    EXPECT_NE(not_open_page->body.find("href=\"/\""), std::string::npos);
    EXPECT_EQ(not_open_page->body.find("://"), std::string::npos);
    const auto health = client.Get("/health");
    ASSERT_TRUE(health);
    EXPECT_EQ(health->status, 200);
    EXPECT_EQ(
        nlohmann::json::parse(health->body).at("live_session_count"),
        2);
    expect_same_origin_payload(nlohmann::json::parse(health->body));

    const test::ProcessExit stopped = server.stop(SIGINT);
    EXPECT_FALSE(stopped.timed_out) << server.errors();
    EXPECT_EQ(stopped.exit_code, 0) << server.errors();
    EXPECT_TRUE(first_stream.wait_for_end());
    EXPECT_TRUE(second_stream.wait_for_end());
}

TEST(WebServerProcess, RestartReopensLeasesAfterCleanAndForcedExit) {
    test::TestWorkspace workspace;
    const int port = test::reserve_loopback_port();
    ASSERT_NE(port, 0);
    workspace.write_app_config(port);
    std::string session_id;

    {
        test::WebServerProcess first(workspace.root(), port);
        ASSERT_TRUE(first.wait_until_ready()) << first.errors();
        httplib::Client client = web_client(port);
        session_id = create_session(client, "Restarted");
        ASSERT_FALSE(session_id.empty());
        ASSERT_FALSE(open_session(client, session_id).empty());
        const test::ProcessExit stopped = first.stop(SIGINT);
        ASSERT_FALSE(stopped.timed_out) << first.errors();
        ASSERT_EQ(stopped.exit_code, 0) << first.errors();
    }

    {
        test::WebServerProcess second(workspace.root(), port);
        ASSERT_TRUE(second.wait_until_ready()) << second.errors();
        httplib::Client client = web_client(port);
        ASSERT_FALSE(open_session(client, session_id).empty());
        const test::ProcessExit killed = second.stop(SIGKILL);
        ASSERT_FALSE(killed.timed_out);
        ASSERT_EQ(killed.exit_code, 128 + SIGKILL);
    }

    {
        test::WebServerProcess third(workspace.root(), port);
        ASSERT_TRUE(third.wait_until_ready()) << third.errors();
        httplib::Client client = web_client(port);
        EXPECT_FALSE(open_session(client, session_id).empty());
        const test::ProcessExit stopped = third.stop(SIGINT);
        EXPECT_FALSE(stopped.timed_out) << third.errors();
        EXPECT_EQ(stopped.exit_code, 0) << third.errors();
    }
}

TEST(WebServerProcess, SignalShutdownCancelsAndJoinsActiveGeneration) {
    const std::string partial_event =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Partial\"}}]}\n\n";
    MockHttpServer provider({
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream\r\n"
        "Connection: close\r\n\r\n" + partial_event,
    }, true);
    provider.start();

    test::TestWorkspace workspace;
    workspace.write_persona_config(
        "display_name = \"Guide\"\n"
        "host = \"127.0.0.1\"\n"
        "port = " + std::to_string(provider.port()) + "\n"
        "mode = \"net\"\n"
        "model = \"blocking-test-model\"\n"
        "stream = true\n");
    const int port = test::reserve_loopback_port();
    ASSERT_NE(port, 0);
    workspace.write_app_config(port);
    test::WebServerProcess server(workspace.root(), port);
    ASSERT_TRUE(server.wait_until_ready()) << server.errors();

    httplib::Client client = web_client(port);
    const std::string id = create_session(client, "Generating");
    ASSERT_FALSE(id.empty());
    const std::string path = open_session(client, id);
    ASSERT_FALSE(path.empty());
    const auto input = client.Post(
        path + "api/v1/input",
        R"({"text":"Question"})",
        "application/json");
    ASSERT_TRUE(input);
    ASSERT_EQ(input->status, 200) << input->body;
    ASSERT_TRUE(provider.wait_for_requests(1, 2s));

    const test::ProcessExit stopped = server.stop(SIGINT);
    EXPECT_FALSE(stopped.timed_out) << server.errors();
    EXPECT_EQ(stopped.exit_code, 0) << server.errors();
    EXPECT_NO_THROW(provider.join());
    EXPECT_EQ(provider.requests().size(), 1U);
}

TEST(ServerShutdownCoordinatorProcess, BlockedOwnerForcesExitAndLogsIdentity) {
    test::TestWorkspace workspace;
    const std::filesystem::path log_path =
        workspace.root() / "logs" / "forced-shutdown.log";
    const auto started = std::chrono::steady_clock::now();
    EXPECT_EXIT(
        run_blocked_shutdown(log_path),
        ::testing::ExitedWithCode(1),
        "");
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);

    std::ifstream log(log_path);
    const std::string contents{
        std::istreambuf_iterator<char>(log),
        std::istreambuf_iterator<char>()};
    EXPECT_NE(
        contents.find(
            "Web shutdown grace expired: forum_id=blocked-forum "
            "session_id=blocked-session"),
        std::string::npos);
}

TEST(ServerShutdownCoordinatorProcess, ShutdownWakesARealHttpOpenBeforeOwnerCommits) {
    test::TestWorkspace fixture;
    auto workspace = std::make_shared<const Workspace>(fixture.root());
    const SessionSummary stored =
        workspace->create_stored_session("lobby", "Opening");
    OpeningGate gate;
    WebSettings settings;
    settings.open_deadline = 5s;
    SessionRegistry registry(
        settings,
        [&gate](const SessionKey&, WakeNotifier&) {
            gate.wait();
            return std::make_unique<IdleController>();
        });
    ReleaseOpeningGateOnExit release_gate(gate);
    httplib::Server server;
    configure_http_server(server, settings);
    LobbyRoutes(workspace, registry, settings).install(server);
    const int port = server.bind_to_any_port("127.0.0.1");
    ASSERT_GT(port, 0);
    std::thread listener([&server] { server.listen_after_bind(); });
    server.wait_until_ready();
    ServerShutdownCoordinator coordinator(registry, server);

    auto opening = std::async(std::launch::async, [port, id = stored.id] {
        httplib::Client client = web_client(port);
        return client.Post(
            "/api/v1/forums/lobby/sessions/" + id + "/open",
            "{}",
            "application/json");
    });
    const bool owner_entered = gate.wait_until_entered();
    EXPECT_TRUE(owner_entered);
    if (!owner_entered) {
        gate.release();
        server.stop();
        listener.join();
        (void)opening.get();
        return;
    }

    auto shutdown = std::async(std::launch::async, [&] {
        coordinator.shutdown_now(listener, 1s);
    });
    const std::future_status open_status = opening.wait_for(250ms);
    EXPECT_EQ(open_status, std::future_status::ready);
    if (open_status == std::future_status::ready) {
        const httplib::Result response = opening.get();
        EXPECT_TRUE(response);
        if (response) {
            EXPECT_EQ(response->status, 503);
            EXPECT_EQ(
                nlohmann::json::parse(response->body)
                    .at("error")
                    .at("code"),
                "server_stopping");
        }
    }

    gate.release();
    EXPECT_EQ(shutdown.wait_for(1s), std::future_status::ready);
    shutdown.get();
}

} // namespace
} // namespace cha::web
