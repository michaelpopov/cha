#include "support/mock_http_server.h"
#include "support/test_live_session.h"
#include "support/test_web_graph.h"
#include "support/test_workspace.h"
#include "support/web_server_process.h"
#include "session/session_database.h"
#include "web/http_server.h"
#include "web/asset_handler.h"
#include "web/live_session_manager.h"
#include "web/lobby_routes.h"
#include "web/server_shutdown.h"
#include "web/session_routes.h"
#include "util/logging.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace cha::web {
namespace {

using namespace std::chrono_literals;

httplib::Client web_client(int port) {
    httplib::Client client("127.0.0.1", port);
    client.set_connection_timeout(1s);
    client.set_read_timeout(2s);
    return client;
}

std::string log_contents(const std::filesystem::path& path) {
    std::ifstream log(path);
    return {
        std::istreambuf_iterator<char>(log),
        std::istreambuf_iterator<char>()};
}

// Diagnostic logging flushes every record as it is written, so the log is what
// a test waits on for work the server performs on its own threads.
[[nodiscard]] bool wait_for_log_record(
    const std::filesystem::path& path,
    std::string_view record,
    std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (log_contents(path).find(record) == std::string::npos) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(10ms);
    }
    return true;
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
    const nlohmann::json result = nlohmann::json::parse(opened->body);
    const std::string path = "/s/" + result.at("forum_id").get<std::string>()
        + "/" + result.at("session_id").get<std::string>() + "/";
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
            return last_snapshot_locked().has_value();
        });
    }

    [[nodiscard]] bool wait_for_shutdown_reason(
        std::string_view reason,
        std::chrono::milliseconds timeout = 2s) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [this, reason] {
            const auto snapshot = last_snapshot_locked();
            return snapshot
                && snapshot->contains("shutdown_reason")
                && (*snapshot)["shutdown_reason"] == reason;
        });
    }

    [[nodiscard]] std::optional<nlohmann::json> last_snapshot() const {
        std::lock_guard lock(mutex_);
        return last_snapshot_locked();
    }

    [[nodiscard]] bool wait_for_end(
        std::chrono::milliseconds timeout = 2s) {
        return request_.wait_for(timeout) == std::future_status::ready;
    }

private:
    [[nodiscard]] std::optional<nlohmann::json> last_snapshot_locked() const {
        const std::string marker = "event: snapshot\n";
        const auto event = content_.rfind(marker);
        if (event == std::string::npos) return std::nullopt;
        const auto data = content_.find("data: ", event);
        if (data == std::string::npos) return std::nullopt;
        const auto line = content_.find('\n', data);
        const auto payload = content_.substr(
            data + 6, (line == std::string::npos ? content_.size() : line) - (data + 6));
        try {
            return nlohmann::json::parse(payload);
        } catch (const nlohmann::json::exception&) {
            return std::nullopt;
        }
    }

    httplib::Client client_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::string content_;
    std::future<httplib::Result> request_;
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

class RawHttpSocket {
public:
    RawHttpSocket(int port, int receive_buffer = 0) : port_(port) {
        descriptor_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (descriptor_ == -1) {
            throw std::runtime_error("Could not create raw HTTP test socket");
        }
        if (receive_buffer > 0
            && ::setsockopt(
                descriptor_, SOL_SOCKET, SO_RCVBUF,
                &receive_buffer, sizeof(receive_buffer)) != 0) {
            close();
            throw std::runtime_error("Could not bound raw HTTP receive buffer");
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(static_cast<std::uint16_t>(port));
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(
                descriptor_, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0) {
            close();
            throw std::runtime_error("Could not connect raw HTTP test socket");
        }
    }

    ~RawHttpSocket() { close(); }
    RawHttpSocket(const RawHttpSocket&) = delete;
    RawHttpSocket& operator=(const RawHttpSocket&) = delete;

    void send_bytes(std::string_view bytes) {
        std::size_t offset = 0;
        while (offset != bytes.size()) {
            const ssize_t sent = ::send(
                descriptor_, bytes.data() + offset, bytes.size() - offset, 0);
            if (sent > 0) {
                offset += static_cast<std::size_t>(sent);
                continue;
            }
            if (sent == -1 && errno == EINTR) continue;
            throw std::runtime_error("Could not write raw HTTP test request");
        }
    }

    void send_get(std::string_view path) {
        send_bytes(
            "GET " + std::string(path)
            + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port_)
            + "\r\nConnection: close\r\n\r\n");
    }

    [[nodiscard]] int read_status(
        std::chrono::milliseconds timeout = 1s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::string response;
        std::array<char, 1024> bytes{};
        while (response.find("\r\n\r\n") == std::string::npos
            && std::chrono::steady_clock::now() < deadline) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            pollfd descriptor{descriptor_, POLLIN | POLLHUP | POLLERR, 0};
            const int ready = ::poll(
                &descriptor, 1, static_cast<int>(std::max(left, 1ms).count()));
            if (ready == -1 && errno == EINTR) continue;
            if (ready <= 0) return 0;
            const ssize_t count = ::recv(
                descriptor_, bytes.data(), bytes.size(), 0);
            if (count <= 0) return 0;
            response.append(bytes.data(), static_cast<std::size_t>(count));
        }
        const std::size_t first_space = response.find(' ');
        if (first_space == std::string::npos || response.size() < first_space + 4) {
            return 0;
        }
        return std::stoi(response.substr(first_space + 1, 3));
    }

    [[nodiscard]] std::size_t read_some(
        std::size_t maximum,
        std::chrono::milliseconds timeout) {
        pollfd descriptor{descriptor_, POLLIN | POLLHUP | POLLERR, 0};
        int ready{};
        do {
            ready = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
        } while (ready == -1 && errno == EINTR);
        if (ready <= 0) return 0;
        std::vector<char> bytes(maximum);
        const ssize_t count = ::recv(
            descriptor_, bytes.data(), bytes.size(), 0);
        return count > 0 ? static_cast<std::size_t>(count) : 0;
    }

    void close() noexcept {
        if (descriptor_ == -1) return;
        (void)::close(descriptor_);
        descriptor_ = -1;
    }

private:
    int descriptor_{-1};
    int port_{};
};

// A real controller whose restored transcript is large enough that one
// snapshot cannot fit in a socket buffer.
SessionRestore large_transcript(std::size_t text_size) {
    SessionRestore restored;
    restored.entries.push_back({
        .id = 1,
        .kind = EntryKind::character,
        .participant_id = "guide",
        .display_name = "Guide",
        .text = std::string(text_size, 'x'),
        .status = EntryStatus::complete,
    });
    restored.next_entry_id = 2;
    return restored;
}

// Instrumented builds run the server several times slower, which starves the
// absolute socket timings these fixtures depend on. Scaling one constant keeps
// the ratios each test actually asserts on unchanged, and keeps the assertions
// tight for ordinary builds instead of loosening them for everyone. The scale
// stays well under cpp-httplib's 5s default write timeout, which is what these
// tests exist to distinguish the configured timeout from.
#if defined(__has_feature)
#  if __has_feature(thread_sanitizer) || __has_feature(address_sanitizer)
#    define CHA_SOCKET_TIMING_INSTRUMENTED 1
#  endif
#endif
#if defined(__SANITIZE_THREAD__) || defined(__SANITIZE_ADDRESS__)
#  define CHA_SOCKET_TIMING_INSTRUMENTED 1
#endif

#if defined(CHA_SOCKET_TIMING_INSTRUMENTED)
constexpr int socket_timing_scale = 4;
// Release latency measured under ThreadSanitizer is the configured timeout
// plus roughly two seconds of instrumentation overhead, which is additive
// rather than proportional, so it is not folded into the scale above.
constexpr std::chrono::milliseconds socket_release_margin{2500};
#else
constexpr int socket_timing_scale = 1;
constexpr std::chrono::milliseconds socket_release_margin{500};
#endif

class RealSocketSseServer {
public:
    static constexpr std::chrono::milliseconds write_timeout{
        150 * socket_timing_scale};

    RealSocketSseServer()
        : settings_(make_settings()),
          live_sessions_(
              settings_,
              [this](const FullSessionId& key, std::shared_ptr<WakeNotifier> notifier) {
                  return test::open_restored_session(
                      key,
                      database_.path(),
                      notifier,
                      large_transcript(8U * 1024U * 1024U),
                      controls_);
              }) {
        server_.set_socket_options([](int socket) {
            const int send_buffer = 16 * 1024;
            (void)::setsockopt(
                socket, SOL_SOCKET, SO_SNDBUF,
                &send_buffer, sizeof(send_buffer));
        });
        SessionRoutes(
            live_sessions_, settings_, AssetHandler(fixture_.root() / "web"))
            .install(server_);
        if (!std::holds_alternative<LiveSessionReady>(
                live_sessions_.open({"forum", "session"}, 5s))) {
            throw std::runtime_error("Could not open real-socket SSE fixture session");
        }
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ <= 0) {
            throw std::runtime_error("Could not bind real-socket SSE fixture");
        }
        configure_http_server(server_, settings_);
        listener_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }

    ~RealSocketSseServer() {
        ServerShutdownCoordinator(live_sessions_, server_)
            .shutdown_now(listener_, 5s);
    }

    [[nodiscard]] int port() const noexcept { return port_; }
    static constexpr std::string_view events_path =
        "/s/forum/session/api/v1/events";
    static constexpr std::size_t worker_count = 3;

private:
    static WebSettings make_settings() {
        WebSettings settings;
        settings.session_limit = 1;
        settings.http_request_headroom = 2;
        settings.http_thread_pool_size = worker_count;
        settings.http_pending_request_limit = 3;
        settings.http_write_timeout = write_timeout;
        settings.sse_heartbeat_interval = 25ms;
        return settings;
    }

    test::TestWorkspace fixture_;
    test::TemporarySessionFile database_{"real_socket_sse"};
    std::shared_ptr<test::BackendControls> controls_ =
        std::make_shared<test::BackendControls>();
    WebSettings settings_;
    LiveSessionManager live_sessions_;
    httplib::Server server_;
    int port_{};
    std::thread listener_;
};

int request_status(int port, std::string_view path) {
    RawHttpSocket socket(port);
    socket.send_get(path);
    return socket.read_status();
}

TEST(WebServerSocketLimits, RejectsARequestThatExceedsInjectedReadTimeout) {
    WebSettings settings;
    settings.session_limit = 1;
    settings.http_request_headroom = 1;
    settings.http_thread_pool_size = 2;
    settings.http_pending_request_limit = 2;
    settings.http_read_timeout = 100ms;
    httplib::Server server;
    server.Get("/health", [](const httplib::Request&, httplib::Response& response) {
        response.set_content("ok", "text/plain");
    });
    const int port = server.bind_to_any_port("127.0.0.1");
    ASSERT_GT(port, 0);
    configure_http_server(server, settings);
    std::thread listener([&server] { server.listen_after_bind(); });
    server.wait_until_ready();

    RawHttpSocket client(port);
    client.send_bytes(
        "GET /health HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Half-Sent:");
    const auto started = std::chrono::steady_clock::now();
    EXPECT_EQ(client.read_status(1s), 400);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);

    server.stop();
    listener.join();
}

// A reader who stops reading pins the request worker writing to it. The write
// timeout is what gives that worker back; without it the pool drains one
// abandoned device at a time. Worker exhaustion is the observable, because a
// newer stream is now always admitted whatever the older ones are doing.
TEST(WebServerSocketLimits, StalledSseReadersReleaseWorkersAfterWriteTimeout) {
    RealSocketSseServer server;
    std::vector<std::unique_ptr<RawHttpSocket>> stalled;
    for (std::size_t worker = 0; worker != RealSocketSseServer::worker_count;
         ++worker) {
        auto socket = std::make_unique<RawHttpSocket>(server.port(), 4096);
        socket->send_get(RealSocketSseServer::events_path);
        ASSERT_EQ(socket->read_status(), 200);
        stalled.push_back(std::move(socket));
    }

    // Allow scheduler and owner-notification latency around the configured
    // no-progress timeout while still proving this is not the library's 5s
    // default or an indefinitely pinned request worker. The poll loop and the
    // assertion share the margin so the loop can never give up before the
    // bound it is measuring against.
    constexpr auto release_margin = socket_release_margin;
    const auto bound = RealSocketSseServer::write_timeout + release_margin;

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + bound;
    int status{};
    while (status != 200 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(20ms);
        status = request_status(
            server.port(), "/s/forum/session/api/v1/session");
    }
    EXPECT_EQ(status, 200);
    EXPECT_LT(std::chrono::steady_clock::now() - started, bound);
}

TEST(WebServerSocketLimits, SlowProgressingSseReaderStaysConnected) {
    RealSocketSseServer server;
    RawHttpSocket slow(server.port(), 64U * 1024U);
    slow.send_get(RealSocketSseServer::events_path);
    ASSERT_EQ(slow.read_status(), 200);

    const auto started = std::chrono::steady_clock::now();
    const auto duration = RealSocketSseServer::write_timeout * 5;
    std::size_t reads{};
    while (std::chrono::steady_clock::now() - started < duration) {
        if (slow.read_some(64U * 1024U, 100ms) != 0) ++reads;
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_GT(reads, 2U);
    // A reader who is keeping up still hands the session over to the device
    // they just opened it on: the new request is served, not refused.
    EXPECT_EQ(
        request_status(server.port(), RealSocketSseServer::events_path),
        200);
}

void run_blocked_shutdown(const std::filesystem::path& log_path) {
    // A policy regression must fail this death test quickly instead of
    // inheriting ctest's much larger timeout.
    (void)::alarm(2);
    shutdown_diagnostic_logging();
    initialize_diagnostic_logging(log_path, "critical");
    // A generation that ignores cancellation wedges this owner inside
    // SessionController::shutdown(), which is the exact state the coordinator
    // must report rather than joining.
    test::TemporarySessionFile database("blocked_shutdown");
    const auto remove_database = [&database] {
        std::error_code error;
        std::filesystem::remove(database.path(), error);
        std::filesystem::remove(SessionLease::companion_path(database.path()), error);
        std::filesystem::remove(database.path().string() + "-journal", error);
        std::filesystem::remove(database.path().string() + "-shm", error);
        std::filesystem::remove(database.path().string() + "-wal", error);
    };
    auto controls = std::make_shared<test::BackendControls>();
    controls->ignore_cancellation();
    WebSettings settings;
    settings.session_limit = 1;
    LiveSessionManager live_sessions(
        settings,
        [&](const FullSessionId& key, std::shared_ptr<WakeNotifier> notifier) {
            return test::open_scripted_session(
                key, database.path(), notifier, controls);
        });
    const FullSessionId key{"blocked-forum", "blocked-session"};
    if (!std::holds_alternative<LiveSessionReady>(
            live_sessions.open(key, 5s))) {
        remove_database();
        _exit(2);
    }
    LiveSessionHandle session = live_sessions.lookup(key);
    if (!session
        || !std::holds_alternative<CommandResult>(
            session->submit(RawCommand{"Question"}, 5s))
        || !controls->wait_until_running()) {
        remove_database();
        _exit(4);
    }
    httplib::Server server;
    ServerShutdownCoordinator coordinator(live_sessions, server);
    std::thread listener;
    // shutdown_now deliberately calls _exit when the wedged owner outlives
    // its grace period, so destructors cannot clean this test database.
    remove_database();
    coordinator.shutdown_now(listener, 20ms);
    _exit(3);
}

// The whole point of the two-root split is that an upgrade can replace the
// application directory without touching the customer's files. Every other
// process test hands the same path to both flags, which would pass just as
// happily if the two were secretly one root.
TEST(WebServerProcess, RunsWithAnApplicationRootThatHoldsNoWorkspace) {
    test::TestWorkspace workspace;
    workspace.write_workspace_config();

    const std::filesystem::path application =
        std::filesystem::temp_directory_path()
        / ("cha_application_root_"
           + std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(application / "web" / "assets");
    std::ofstream(application / "web" / "index.html")
        << "<!doctype html><html><body>split shell</body></html>\n";
    std::ofstream(application / "web" / "assets" / "app.js")
        << "console.log('split asset');\n";

    const int port = test::reserve_loopback_port();
    ASSERT_NE(port, 0);
    test::WebServerProcess server(application, workspace.root(), port);
    ASSERT_TRUE(server.wait_until_ready()) << server.errors();

    httplib::Client client = web_client(port);
    // The shell comes from the application root...
    const auto shell = client.Get("/");
    ASSERT_TRUE(shell);
    EXPECT_EQ(shell->status, 200);
    EXPECT_NE(shell->body.find("split shell"), std::string::npos);

    // ...while the conversations come from the workspace somewhere else.
    const auto bootstrap = client.Get("/api/v1/bootstrap");
    ASSERT_TRUE(bootstrap);
    EXPECT_EQ(bootstrap->status, 200);
    EXPECT_NE(bootstrap->body.find("The Lobby"), std::string::npos);

    // Neither directory has grown the other's files.
    EXPECT_FALSE(std::filesystem::exists(application / "workspace.toml"));
    EXPECT_FALSE(std::filesystem::exists(workspace.root() / "app.toml"));

    EXPECT_EQ(server.stop(SIGTERM).exit_code, 0);
    std::error_code removal;
    std::filesystem::remove_all(application, removal);
}

TEST(WebServerProcess, RefusesIncompleteManualCutoverBeforeBinding) {
    test::TestWorkspace workspace;
    workspace.write_workspace_config();
    const std::filesystem::path legacy = workspace.root()
        / "forums" / "lobby" / "sessions" / "legacy.sqlite3";
    std::filesystem::create_directories(legacy.parent_path());
    std::ofstream(legacy) << "legacy";
    const int port = test::reserve_loopback_port();
    ASSERT_NE(port, 0);

    {
        test::WebServerProcess server(workspace.root(), port);
        EXPECT_FALSE(server.wait_until_ready());
        const std::string errors = server.errors();
        EXPECT_NE(
            errors.find("This build cannot migrate them"),
            std::string::npos)
            << errors;
        EXPECT_NE(
            errors.find("archived migration-capable CHA build"),
            std::string::npos)
            << errors;
        EXPECT_NE(
            errors.find("chaweb --migration --workspace <workspace>"),
            std::string::npos)
            << errors;
        EXPECT_NE(errors.find("verify 'workspace.sqlite3'"), std::string::npos)
            << errors;
        EXPECT_NE(
            errors.find("remove the legacy session files"),
            std::string::npos)
            << errors;
        EXPECT_FALSE(std::filesystem::exists(
            workspace.root() / "workspace.sqlite3"));
    }

    std::filesystem::remove(legacy);
    {
        const test::WebGraph graph(workspace.root());
        EXPECT_TRUE(std::filesystem::is_regular_file(
            workspace.root() / "workspace.sqlite3"));
    }
    std::ofstream(legacy) << "legacy";
    {
        test::WebServerProcess server(workspace.root(), port);
        EXPECT_FALSE(server.wait_until_ready());
        const std::string errors = server.errors();
        EXPECT_NE(errors.find("Verify 'workspace.sqlite3'"), std::string::npos)
            << errors;
        EXPECT_NE(
            errors.find("remove the legacy session files"),
            std::string::npos)
            << errors;
    }
}

TEST(WebServerProcess, RejectsASecondProcessWithWorkspaceDiagnostic) {
    test::TestWorkspace workspace;
    workspace.write_workspace_config();
    const int first_port = test::reserve_loopback_port();
    int second_port = test::reserve_loopback_port();
    for (int attempt = 0; second_port == first_port && attempt != 10; ++attempt) {
        second_port = test::reserve_loopback_port();
    }
    ASSERT_NE(first_port, 0);
    ASSERT_NE(second_port, 0);
    ASSERT_NE(first_port, second_port);

    test::WebServerProcess first(workspace.root(), first_port);
    ASSERT_TRUE(first.wait_until_ready()) << first.errors();
    test::WebServerProcess second(workspace.root(), second_port);
    EXPECT_FALSE(second.wait_until_ready());
    EXPECT_NE(
        second.errors().find(
            "Workspace already in use: '" + workspace.root().string() + "'"),
        std::string::npos)
        << second.errors();

    const test::ProcessExit stopped = first.stop(SIGINT);
    EXPECT_FALSE(stopped.timed_out) << first.errors();
    EXPECT_EQ(stopped.exit_code, 0) << first.errors();
}

TEST(WebServerProcess, ServesConcurrentSseAndOrdinaryRequestsOnOneOrigin) {
    test::TestWorkspace workspace;
    const int port = test::reserve_loopback_port();
    ASSERT_NE(port, 0);
    workspace.write_workspace_config();
    test::WebServerProcess server(workspace.root(), port);
    ASSERT_TRUE(server.wait_until_ready()) << server.errors();

    httplib::Client client = web_client(port);
    const auto shell = client.Get("/");
    ASSERT_TRUE(shell);
    EXPECT_EQ(shell->status, 200);
    EXPECT_NE(shell->body.find("test shell"), std::string::npos);
    const auto asset = client.Get("/assets/app.js");
    ASSERT_TRUE(asset);
    EXPECT_EQ(asset->status, 200);
    const auto lobby = client.Get("/");
    ASSERT_TRUE(lobby);
    EXPECT_EQ(lobby->status, 200);
    EXPECT_EQ(lobby->body.find("http://"), std::string::npos);
    EXPECT_EQ(lobby->body.find("https://"), std::string::npos);
    const auto bootstrap = client.Get("/api/v1/bootstrap");
    ASSERT_TRUE(bootstrap);
    ASSERT_EQ(bootstrap->status, 200);
    expect_same_origin_payload(nlohmann::json::parse(bootstrap->body));

    constexpr std::size_t session_limit = 8;
    std::vector<std::string> session_ids;
    session_ids.reserve(session_limit);
    for (std::size_t index = 0; index != session_limit; ++index) {
        session_ids.push_back(create_session(
            client, "Session " + std::to_string(index)));
        ASSERT_FALSE(session_ids.back().empty());
    }
    const auto sessions = client.Get("/api/v1/forums/lobby/sessions");
    ASSERT_TRUE(sessions);
    ASSERT_EQ(sessions->status, 200);
    expect_same_origin_payload(nlohmann::json::parse(sessions->body));
    std::vector<std::string> session_paths;
    std::vector<std::unique_ptr<StreamingRequest>> streams;
    session_paths.reserve(session_limit);
    streams.reserve(session_limit);
    for (const std::string& id : session_ids) {
        session_paths.push_back(open_session(client, id));
        ASSERT_EQ(session_paths.back(), "/s/lobby/" + id + "/");
        streams.push_back(std::make_unique<StreamingRequest>(
            port, session_paths.back() + "api/v1/events"));
    }
    for (const auto& stream : streams) {
        ASSERT_TRUE(stream->wait_for_snapshot());
    }

    const auto snapshot = client.Get(
        session_paths.front() + "api/v1/session");
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->status, 200);
    EXPECT_EQ(
        nlohmann::json::parse(snapshot->body).at("forum").at("id"),
        "lobby");
    expect_same_origin_payload(nlohmann::json::parse(snapshot->body));
    const auto page = client.Get(session_paths.front());
    ASSERT_TRUE(page);
    EXPECT_EQ(page->status, 200);
    EXPECT_EQ(page->body.find("://"), std::string::npos);
    const auto non_live_page = client.Get("/s/lobby/not-open/");
    ASSERT_TRUE(non_live_page);
    EXPECT_EQ(non_live_page->status, 200);
    EXPECT_NE(non_live_page->body.find("test shell"), std::string::npos);
    EXPECT_EQ(non_live_page->body.find("://"), std::string::npos);
    const auto health = client.Get("/health");
    ASSERT_TRUE(health);
    EXPECT_EQ(health->status, 200);
    EXPECT_EQ(
        nlohmann::json::parse(health->body).at("live_session_count"),
        session_limit);
    expect_same_origin_payload(nlohmann::json::parse(health->body));

    const test::ProcessExit stopped = server.stop(SIGINT);
    EXPECT_FALSE(stopped.timed_out) << server.errors();
    EXPECT_EQ(stopped.exit_code, 0) << server.errors();
    for (const auto& stream : streams) {
        EXPECT_TRUE(stream->wait_for_end());
    }
}

TEST(WebServerProcess, ForumPersonaReachesTheLiveTranscript) {
    test::TestWorkspace workspace;
    const int port = test::reserve_loopback_port();
    ASSERT_NE(port, 0);
    workspace.write_workspace_config();
    test::WebServerProcess server(workspace.root(), port);
    ASSERT_TRUE(server.wait_until_ready()) << server.errors();

    httplib::Client client = web_client(port);
    const std::string id = create_session(client, "Attributed");
    ASSERT_FALSE(id.empty());
    const std::string path = open_session(client, id);
    ASSERT_FALSE(path.empty());
    const auto submitted = client.Post(
        path + "api/v1/input",
        R"({"text":"Who wrote this?"})",
        "application/json");
    ASSERT_TRUE(submitted);
    ASSERT_EQ(submitted->status, 200) << submitted->body;

    const auto snapshot = client.Get(path + "api/v1/session");
    ASSERT_TRUE(snapshot);
    ASSERT_EQ(snapshot->status, 200) << snapshot->body;
    const nlohmann::json transcript =
        nlohmann::json::parse(snapshot->body).at("transcript");
    const auto human = std::find_if(
        transcript.begin(), transcript.end(), [](const nlohmann::json& entry) {
            return entry.at("kind") == "human";
        });
    ASSERT_NE(human, transcript.end());
    EXPECT_EQ(human->at("participant_id"), guest_id);
    EXPECT_EQ(human->at("display_name"), guest_name);
    EXPECT_EQ(human->at("text"), "Who wrote this?");

    const test::ProcessExit stopped = server.stop(SIGINT);
    EXPECT_FALSE(stopped.timed_out) << server.errors();
    EXPECT_EQ(stopped.exit_code, 0) << server.errors();
}

TEST(WebServerProcess, RestartReopensLeasesAfterCleanAndForcedExit) {
    test::TestWorkspace workspace;
    const int port = test::reserve_loopback_port();
    ASSERT_NE(port, 0);
    workspace.write_workspace_config();
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

TEST(WebServerProcess, LogsServerAndSessionLifecycleWithoutPromptBodies) {
    test::TestWorkspace workspace;
    const int port = test::reserve_loopback_port();
    ASSERT_NE(port, 0);
    workspace.write_workspace_config("info");
    test::WebServerProcess server(workspace.root(), port);
    ASSERT_TRUE(server.wait_until_ready()) << server.errors();

    httplib::Client client = web_client(port);
    const std::string id = create_session(client, "Observable");
    const std::string other_id = create_session(client, "Concurrent");
    ASSERT_FALSE(id.empty());
    ASSERT_FALSE(other_id.empty());
    const std::string path = open_session(client, id);
    const std::string other_path = open_session(client, other_id);
    ASSERT_FALSE(path.empty());
    ASSERT_FALSE(other_path.empty());
    ASSERT_EQ(open_session(client, id), path);
    {
        StreamingRequest stream(port, path + "api/v1/events");
        ASSERT_TRUE(stream.wait_for_snapshot());
    }
    // The stream destructor waits only for the client's own request to end; the
    // server learns of the close on a later write attempt.
    ASSERT_TRUE(client.Get(path + "api/v1/session"));
    constexpr std::string_view prompt = "very-secret-prompt-body";
    constexpr std::string_view other_prompt = "another-secret-prompt-body";
    auto submit = [port](std::string route, std::string_view text) {
        httplib::Client concurrent_client = web_client(port);
        return concurrent_client.Post(
            std::move(route),
            std::string(R"({"text":")") + std::string(text) + R"("})",
            "application/json");
    };
    auto first_submission = std::async(
        std::launch::async, submit, path + "api/v1/input", prompt);
    auto second_submission = std::async(
        std::launch::async, submit, other_path + "api/v1/input", other_prompt);
    const httplib::Result submitted = first_submission.get();
    const httplib::Result other_submitted = second_submission.get();
    ASSERT_TRUE(submitted);
    ASSERT_TRUE(other_submitted);
    ASSERT_EQ(submitted->status, 200) << submitted->body;
    ASSERT_EQ(other_submitted->status, 200) << other_submitted->body;

    // cpp-httplib releases a closed stream on a worker thread, and the owner
    // logs that disconnect only when it drains the resulting notification.
    // Shutdown cannot order either step: HTTP workers are joined after every
    // owner has finished, so a release that lands late is a documented no-op on
    // a finished session. Wait for the record instead of signalling into that
    // race. The bound covers one heartbeat interval, the longest a released
    // stream can go unnoticed; the submissions above normally wake it at once.
    const std::filesystem::path log_path =
        workspace.root() / "logs" / "cha.log";
    ASSERT_TRUE(wait_for_log_record(
        log_path, "event=sse_disconnected collapsed_payloads=", 20s))
        << log_contents(log_path);

    const test::ProcessExit stopped = server.stop(SIGINT);
    ASSERT_FALSE(stopped.timed_out) << server.errors();
    ASSERT_EQ(stopped.exit_code, 0) << server.errors();
    const std::string contents = log_contents(log_path);
    EXPECT_NE(contents.find("web server event=startup"), std::string::npos);
    EXPECT_NE(contents.find("web server event=bound"), std::string::npos);
    EXPECT_NE(contents.find("web server event=shutdown"), std::string::npos);
    EXPECT_NE(contents.find("event=registry_running"), std::string::npos);
    EXPECT_NE(contents.find("event=reattached"), std::string::npos);
    EXPECT_NE(contents.find("event=registry_stopping"), std::string::npos);
    EXPECT_NE(contents.find("event=registry_sweep_joined"), std::string::npos);
    EXPECT_NE(contents.find("event=sse_connected"), std::string::npos);
    EXPECT_NE(contents.find("event=sse_disconnected collapsed_payloads="), std::string::npos);
    EXPECT_NE(contents.find("forum_id=lobby session_id=" + id), std::string::npos);
    EXPECT_EQ(contents.find(prompt), std::string::npos);
    EXPECT_EQ(contents.find(other_prompt), std::string::npos);

    std::istringstream lines(contents);
    for (std::string line; std::getline(lines, line);) {
        if (line.find("web session ") == std::string::npos) continue;
        EXPECT_NE(line.find("forum_id=lobby"), std::string::npos) << line;
        const bool first_identity = line.find(
            "session_id=" + id + " event=") != std::string::npos;
        const bool second_identity = line.find(
            "session_id=" + other_id + " event=") != std::string::npos;
        EXPECT_NE(first_identity, second_identity) << line;
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
    workspace.write_provider("blocking",
        "host = \"127.0.0.1\"\n"
        "port = " + std::to_string(provider.port()) + "\n"
        "mode = \"net\"\n"
        "model = \"blocking-test-model\"\n"
        "stream = true\n");
    workspace.write_character_config(
        "display_name = \"Guide\"\nprovider = \"blocking\"\n");
    const int port = test::reserve_loopback_port();
    ASSERT_NE(port, 0);
    workspace.write_workspace_config();
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
    const test::WebGraph graph(fixture.root());
    const StoredSession stored = graph.sessions()->create("lobby", "Opening");
    OpeningGate gate;
    WebSettings settings;
    settings.open_deadline = 5s;
    test::TemporarySessionFile database("opening_gate");
    LiveSessionManager live_sessions(
        settings,
        [&gate, &database](const FullSessionId& key, std::shared_ptr<WakeNotifier> notifier) {
            gate.wait();
            return test::open_test_session(key, database.path(), notifier);
        });
    ReleaseOpeningGateOnExit release_gate(gate);
    httplib::Server server;
    LobbyRoutes(
        graph.sessions(), test::WebGraph::initial_selection(),
        live_sessions, settings).install(server);
    const int port = server.bind_to_any_port("127.0.0.1");
    ASSERT_GT(port, 0);
    configure_http_server(server, settings);
    std::thread listener([&server] { server.listen_after_bind(); });
    server.wait_until_ready();
    ServerShutdownCoordinator coordinator(live_sessions, server);

    auto opening = std::async(std::launch::async, [port, id = stored.identity.session_id] {
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

bool open_after_reload(
    httplib::Client& client,
    std::string_view forum,
    std::string_view id) {
    for (int attempt = 0; attempt != 20; ++attempt) {
        const auto opened = client.Post(
            "/api/v1/forums/" + std::string(forum) + "/sessions/"
                + std::string(id) + "/open",
            "{}",
            "application/json");
        if (opened && opened->status == 200) return true;
        if (opened && opened->status == 409) {
            std::this_thread::sleep_for(50ms);
            continue;
        }
        return false;
    }
    return false;
}

TEST(WebServerProcess, ReloadsALiveSessionAfterAStyleSave) {
    test::TestWorkspace workspace;
    workspace.write_style("mono-large", "font = \"mono\"\nsize = \"large\"\n");
    const int port = test::reserve_loopback_port();
    ASSERT_NE(port, 0);
    test::WebServerProcess server(workspace.root(), port);
    ASSERT_TRUE(server.wait_until_ready()) << server.errors();

    httplib::Client client = web_client(port);
    const std::string id = create_session(client, "Reload");
    ASSERT_FALSE(id.empty());
    const std::string path = open_session(client, id);
    ASSERT_FALSE(path.empty());
    StreamingRequest stream(port, path + "api/v1/events");
    ASSERT_TRUE(stream.wait_for_snapshot());

    const auto patched = client.Patch(
        "/api/v1/characters/guide",
        R"({"provider":"test","style":"mono-large"})",
        "application/json");
    ASSERT_TRUE(patched);
    ASSERT_EQ(patched->status, 200) << patched->body;
    ASSERT_TRUE(stream.wait_for_shutdown_reason("reloading"));
    ASSERT_TRUE(stream.wait_for_end(5s));

    ASSERT_TRUE(open_after_reload(client, "lobby", id));
    const auto snapshot = client.Get(path + "api/v1/session");
    ASSERT_TRUE(snapshot);
    ASSERT_EQ(snapshot->status, 200) << snapshot->body;
    const nlohmann::json characters =
        nlohmann::json::parse(snapshot->body).at("characters");
    ASSERT_FALSE(characters.empty());
    EXPECT_EQ(characters.front().at("appearance"), nlohmann::json({
        {"font", "mono"}, {"style", "normal"},
        {"weight", "normal"}, {"size", "large"}, {"text_color", "normal"}}));
}

TEST(WebServerProcess, ReloadsASessionDespiteAStaleForumProvider) {
    test::TestWorkspace workspace;
    workspace.write_provider(
        "sol-high", "host = \"test\"\nport = 1\nmode = \"test\"\nmodel = \"fake\"\n");
    const auto montaigne = workspace.root() / "characters" / "montaigne";
    std::filesystem::create_directories(montaigne);
    std::ofstream(montaigne / "character.toml")
        << "display_name = \"Montaigne\"\nprovider = \"test\"\n";
    std::ofstream(montaigne / "CHARACTER.md") << "Prompt\n";
    const auto circle = workspace.root() / "forums" / "circle";
    std::filesystem::create_directories(circle / "members" / "montaigne");
    std::ofstream(circle / "config.toml") << "display_name = \"Circle of Life\"\n";
    std::ofstream(circle / "FORUM.md") << "Forum instructions\n";
    std::ofstream(circle / "members" / "character_defaults.toml")
        << "provider = \"sol-high\"\n";

    const int port = test::reserve_loopback_port();
    ASSERT_NE(port, 0);
    test::WebServerProcess server(workspace.root(), port);
    ASSERT_TRUE(server.wait_until_ready()) << server.errors();

    httplib::Client client = web_client(port);
    const auto created = client.Post(
        "/api/v1/forums/circle/sessions",
        R"({"label":"Circle"})",
        "application/json");
    ASSERT_TRUE(created);
    ASSERT_EQ(created->status, 201) << created->body;
    const std::string id = nlohmann::json::parse(created->body).at("id");
    const auto opened = client.Post(
        "/api/v1/forums/circle/sessions/" + id + "/open",
        "{}", "application/json");
    ASSERT_TRUE(opened);
    ASSERT_EQ(opened->status, 200) << opened->body;
    const std::string path = "/s/circle/" + id + "/";
    StreamingRequest stream(port, path + "api/v1/events");
    ASSERT_TRUE(stream.wait_for_snapshot());

    const auto patched = client.Patch(
        "/api/v1/characters/montaigne",
        R"({"provider":"sol-high","style":null})",
        "application/json");
    ASSERT_TRUE(patched);
    ASSERT_EQ(patched->status, 200) << patched->body;
    EXPECT_TRUE(stream.wait_for_shutdown_reason("reloading"));
    EXPECT_TRUE(stream.wait_for_end(5s));
}

TEST(WebServerProcess, ReloadsAForumsLiveSessionsAfterAPersonaSwitch) {
    test::TestWorkspace workspace;
    const auto circle = workspace.root() / "forums" / "circle";
    std::filesystem::create_directories(circle / "members" / "guide");
    std::ofstream(circle / "config.toml") << "display_name = \"The Circle\"\n";
    std::ofstream(circle / "FORUM.md") << "Forum instructions\n";

    const int port = test::reserve_loopback_port();
    ASSERT_NE(port, 0);
    test::WebServerProcess server(workspace.root(), port);
    ASSERT_TRUE(server.wait_until_ready()) << server.errors();

    httplib::Client client = web_client(port);
    const std::string first_id = create_session(client, "First");
    const std::string second_id = create_session(client, "Second");
    ASSERT_FALSE(first_id.empty());
    ASSERT_FALSE(second_id.empty());
    const std::string first_path = open_session(client, first_id);
    const std::string second_path = open_session(client, second_id);
    ASSERT_FALSE(first_path.empty());
    ASSERT_FALSE(second_path.empty());
    StreamingRequest first_stream(port, first_path + "api/v1/events");
    StreamingRequest second_stream(port, second_path + "api/v1/events");
    ASSERT_TRUE(first_stream.wait_for_snapshot());
    ASSERT_TRUE(second_stream.wait_for_snapshot());

    const auto created = client.Post(
        "/api/v1/forums/circle/sessions",
        R"({"label":"Other"})",
        "application/json");
    ASSERT_TRUE(created);
    ASSERT_EQ(created->status, 201) << created->body;
    const std::string other_id = nlohmann::json::parse(created->body).at("id");
    const auto opened = client.Post(
        "/api/v1/forums/circle/sessions/" + other_id + "/open",
        "{}", "application/json");
    ASSERT_TRUE(opened);
    ASSERT_EQ(opened->status, 200) << opened->body;
    const std::string other_path = "/s/circle/" + other_id + "/";
    StreamingRequest other_stream(port, other_path + "api/v1/events");
    ASSERT_TRUE(other_stream.wait_for_snapshot());

    const auto switched = client.Post(
        first_path + "api/v1/input",
        R"({"text":"/!Reader"})",
        "application/json");
    ASSERT_TRUE(switched);
    ASSERT_EQ(switched->status, 200) << switched->body;
    ASSERT_TRUE(first_stream.wait_for_shutdown_reason("reloading"));
    ASSERT_TRUE(second_stream.wait_for_shutdown_reason("reloading"));
    EXPECT_FALSE(other_stream.wait_for_shutdown_reason("reloading", 200ms));
    const auto other_live = client.Get(other_path + "api/v1/session");
    ASSERT_TRUE(other_live);
    EXPECT_EQ(other_live->status, 200) << other_live->body;
    EXPECT_EQ(
        nlohmann::json::parse(other_live->body).at("lifecycle"),
        "running");
}

} // namespace
} // namespace cha::web
