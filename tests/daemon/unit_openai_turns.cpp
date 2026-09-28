#include "app/application.h"
#include "daemon/openai_adapter.h"
#include "daemon/scgi.h"
#include "runtime/runtime_settings.h"
#include "support/mock_http_server.h"
#include "support/test_workspace.h"
#include "util/logging.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace cha::daemon {
namespace {

using app::Application;
using namespace std::chrono_literals;

ApplicationCommand make_command(
    const test::TestWorkspace& workspace,
    const std::filesystem::path& database) {
    const std::filesystem::path config_directory =
        workspace.root() / "cha-config";
    std::filesystem::create_directories(config_directory);
    {
        std::ofstream app(config_directory / "app.toml");
        app << "vault = \"Test\"\n"
            << "[logging]\nfile = \"runtime.log\"\nlevel = \"off\"\n";
    }
    {
        std::ofstream vault(config_directory / "test.toml");
        vault << "vault_name = \"Test\"\n"
              << "data = " << std::quoted(database.string()) << "\n";
    }
    const ConfigurationDirectory loaded =
        load_configuration_directory(config_directory);
    const VaultDefinition* const vault =
        find_vault(loaded.vaults, loaded.startup_vault);
    return {
        .config_directory = loaded.directory,
        .vaults = loaded.vaults,
        .vault = *vault,
        .log_file = loaded.log_file,
        .log_level = loaded.log_level,
        .warnings = loaded.warnings,
    };
}

void add_member(
    const test::TestWorkspace& workspace,
    std::string_view id,
    std::string_view name) {
    workspace.add_character(id, name);
    const auto directory =
        workspace.root() / "forums/lobby/members" / std::string(id);
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "character.toml") << "# member\n";
}

void disable_naming(const test::TestWorkspace& workspace) {
    const auto directory = workspace.root() / "system/session";
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "config.toml")
        << "naming_provider = \"absent\"\n";
}

void use_net_provider(
    const test::TestWorkspace& workspace, int port, bool stream) {
    // Keep the Assistant on the in-process test provider so session naming
    // does not take a response from the character's HTTP server.
    workspace.write_provider(
        "remote",
        "host = \"127.0.0.1\"\nport = " + std::to_string(port)
            + "\nhttps = false\nmode = \"net\"\nmodel = \"fake\"\n"
              "api = \"chat_completions\"\nstream = "
            + (stream ? "true" : "false") + "\ntimeout_s = 20\n");
    workspace.write_character_config(
        "display_name = \"Guide\"\nprovider = \"remote\"\n");
}

nlohmann::json chat_body(
    std::string_view model,
    nlohmann::json messages,
    bool stream = false) {
    return {
        {"model", model},
        {"messages", std::move(messages)},
        {"stream", stream},
    };
}

nlohmann::json user_message(std::string_view text) {
    return {{"role", "user"}, {"content", text}};
}

struct RawResponse {
    int status{};
    std::string raw;
    nlohmann::json json;
};

RawResponse exchange(
    Application& application,
    const ScgiRequest& request,
    DaemonShutdown& shutdown,
    const TurnClock& clock = {}) {
    int fds[2]{};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        throw std::runtime_error("socketpair failed");
    }
    UniqueFd server(fds[0]);
    UniqueFd client(fds[1]);
    handle_request(application, request, server.get(), shutdown, clock);
    server.close();
    RawResponse response;
    char buffer[4096];
    while (true) {
        const ssize_t count = ::recv(client.get(), buffer, sizeof(buffer), 0);
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            break;
        }
        response.raw.append(buffer, static_cast<std::size_t>(count));
    }
    if (response.raw.starts_with("Status: ")) {
        response.status = std::stoi(response.raw.substr(8));
    }
    const auto body_at = response.raw.find("\r\n\r\n");
    if (body_at != std::string::npos
        && (response.status != 200
            || response.raw.find("application/json") != std::string::npos)) {
        const std::string body = response.raw.substr(body_at + 4);
        if (!body.empty() && body.front() == '{') {
            response.json = nlohmann::json::parse(body);
        }
    }
    return response;
}

std::string message_content(const nlohmann::json& response) {
    return response.at("choices").at(0).at("message").at("content")
        .get<std::string>();
}

std::vector<nlohmann::json> sse_events(std::string_view raw) {
    std::vector<nlohmann::json> events;
    std::size_t cursor = 0;
    constexpr std::string_view prefix = "data: ";
    while (cursor < raw.size()) {
        const auto data = raw.find(prefix, cursor);
        if (data == std::string_view::npos) break;
        const auto end = raw.find('\n', data);
        const auto line = raw.substr(
            data, end == std::string_view::npos ? std::string_view::npos
                                                : end - data);
        cursor = end == std::string_view::npos ? raw.size() : end + 1;
        if (line == "data: [DONE]") continue;
        events.push_back(nlohmann::json::parse(
            std::string(line.substr(prefix.size()))));
    }
    return events;
}

std::string streamed_text(std::string_view raw) {
    std::string text;
    for (const auto& payload : sse_events(raw)) {
        if (payload.contains("error")) continue;
        const auto& delta = payload.at("choices").at(0).at("delta");
        if (delta.contains("content") && delta.at("content").is_string()) {
            text += delta.at("content").get<std::string>();
        }
    }
    return text;
}

std::string body_after_tag(std::string_view content) {
    const auto split = content.find("\n\n");
    if (split == std::string_view::npos) return std::string(content);
    return std::string(content.substr(split + 2));
}

SessionSnapshot transcript_of(
    Application& application, std::string_view session_id) {
    const auto epoch = application.context_epoch();
    const auto opened = application.open_session("lobby", session_id, epoch);
    if (!std::holds_alternative<OpenSessionSuccess>(opened)) {
        throw std::runtime_error("Session is not available");
    }
    const auto result = application.snapshot("lobby", session_id, epoch);
    if (!std::holds_alternative<SessionSnapshot>(result)) {
        throw std::runtime_error("Session snapshot failed");
    }
    return std::get<SessionSnapshot>(result);
}

bool transcript_contains(
    const SessionSnapshot& snapshot, std::string_view text) {
    for (const auto& entry : snapshot.transcript) {
        if (entry.text.find(text) != std::string::npos) return true;
    }
    return false;
}

class HoldingHttp {
public:
    HoldingHttp() {
        listen_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_ < 0) throw std::runtime_error("socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listen_, reinterpret_cast<sockaddr*>(&address),
                sizeof(address))
                != 0
            || ::listen(listen_, 4) != 0) {
            throw std::runtime_error("listen failed");
        }
        socklen_t size = sizeof(address);
        if (::getsockname(
                listen_, reinterpret_cast<sockaddr*>(&address), &size)
            != 0) {
            throw std::runtime_error("getsockname failed");
        }
        port_ = ntohs(address.sin_port);
        worker_ = std::thread([this] { run(); });
    }

    ~HoldingHttp() {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
            release_ = true;
        }
        cv_.notify_all();
        if (listen_ >= 0) {
            ::shutdown(listen_, SHUT_RDWR);
            ::close(listen_);
            listen_ = -1;
        }
        if (worker_.joinable()) worker_.join();
    }

    [[nodiscard]] int port() const { return port_; }

    bool wait_for_request(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        const int seen = requests_;
        return cv_.wait_for(lock, timeout, [&] {
            return requests_ > seen || stop_;
        }) && requests_ > seen;
    }

    void send(std::string bytes) {
        {
            std::lock_guard lock(mutex_);
            outgoing_ += std::move(bytes);
            release_ = true;
        }
        cv_.notify_all();
    }

    void finish() {
        {
            std::lock_guard lock(mutex_);
            close_after_ = true;
            release_ = true;
        }
        cv_.notify_all();
    }

    bool respond_when_ready(
        std::string bytes, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        if (!cv_.wait_for(lock, timeout, [&] {
                return awaiting_ || stop_.load();
            })) {
            return false;
        }
        if (stop_.load()) return false;
        outgoing_ += std::move(bytes);
        close_after_ = true;
        release_ = true;
        awaiting_ = false;
        cv_.notify_all();
        return true;
    }

    bool wait_until_awaiting(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] {
            return awaiting_ || stop_.load();
        }) && awaiting_;
    }

private:
    void run() {
        while (!stop_) {
            const int accepted = ::accept(listen_, nullptr, nullptr);
            if (accepted < 0) return;
            const int no_delay = 1;
            (void)::setsockopt(
                accepted, IPPROTO_TCP, TCP_NODELAY, &no_delay, sizeof(no_delay));
            const std::string request = read_request(accepted);
            (void)request;
            {
                std::lock_guard lock(mutex_);
                ++requests_;
                release_ = false;
                outgoing_.clear();
                close_after_ = false;
                awaiting_ = true;
            }
            cv_.notify_all();
            while (!stop_) {
                std::string outgoing;
                bool close_after = false;
                {
                    std::unique_lock lock(mutex_);
                    cv_.wait(lock, [&] { return release_ || stop_; });
                    outgoing = std::move(outgoing_);
                    outgoing_.clear();
                    close_after = close_after_;
                    close_after_ = false;
                    release_ = false;
                    awaiting_ = false;
                }
                if (!outgoing.empty()) send_all(accepted, outgoing);
                if (close_after || stop_) break;
                {
                    std::lock_guard lock(mutex_);
                    awaiting_ = true;
                }
                cv_.notify_all();
            }
            ::shutdown(accepted, SHUT_RDWR);
            ::close(accepted);
        }
    }

    static std::string read_request(int fd) {
        std::string request;
        char buffer[4096];
        while (request.find("\r\n\r\n") == std::string::npos) {
            const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
            if (count <= 0) return request;
            request.append(buffer, static_cast<std::size_t>(count));
        }
        const auto header_end = request.find("\r\n\r\n");
        const auto length_at = request.find("Content-Length:");
        if (length_at == std::string::npos) return request;
        const auto value_at = length_at + std::string("Content-Length:").size();
        const std::size_t length = static_cast<std::size_t>(
            std::stoul(request.substr(value_at)));
        const std::size_t total = header_end + 4 + length;
        while (request.size() < total) {
            const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
            if (count <= 0) break;
            request.append(buffer, static_cast<std::size_t>(count));
        }
        return request;
    }

    static void send_all(int fd, std::string_view bytes) {
        while (!bytes.empty()) {
            const ssize_t count = ::send(fd, bytes.data(), bytes.size(), 0);
            if (count <= 0) return;
            bytes.remove_prefix(static_cast<std::size_t>(count));
        }
    }

    int listen_{-1};
    int port_{};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    int requests_{};
    std::atomic<bool> stop_{false};
    bool release_{};
    bool close_after_{};
    bool awaiting_{};
    std::string outgoing_;
};

std::string http_message(
    std::string_view content_type, std::string_view body, bool length) {
    std::string result = "HTTP/1.1 200 OK\r\nContent-Type: ";
    result += content_type;
    result += "\r\n";
    if (length) {
        result += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    }
    result += "Connection: close\r\n\r\n";
    result += body;
    return result;
}

class TurnTest : public testing::Test {
protected:
    void open_application(RuntimeSettings settings = {}) {
        database_ = test::import_test_database(workspace_.root());
        application_ = Application::open(
            make_command(workspace_, database_), {}, settings);
    }

    RawResponse post(
        nlohmann::json messages,
        bool stream = false,
        DaemonShutdown* shutdown = nullptr,
        const TurnClock& clock = {}) {
        std::atomic<bool> local_stop{false};
        DaemonShutdown local_shutdown{local_stop};
        return exchange(
            *application_,
            {.method = "POST",
             .document_uri = "/v1/chat/completions",
             .body = chat_body("lobby", std::move(messages), stream).dump()},
            shutdown == nullptr ? local_shutdown : *shutdown,
            clock);
    }

    test::TestWorkspace workspace_;
    std::filesystem::path database_;
    std::unique_ptr<Application> application_;
};

TEST_F(TurnTest, NewChatStoresTheTurnWithoutTheTagOrClientHistory) {
    open_application();
    const auto response = post(nlohmann::json::array({
        user_message("earlier history that CHA must ignore"),
        user_message("Hello"),
    }));
    ASSERT_EQ(response.status, 200) << response.raw;
    EXPECT_EQ(response.json.at("object"), "chat.completion");
    EXPECT_EQ(response.json.at("model"), "lobby");
    EXPECT_FALSE(response.json.contains("usage"));
    const auto& choice = response.json.at("choices").at(0);
    EXPECT_EQ(choice.at("index"), 0);
    EXPECT_EQ(choice.at("finish_reason"), "stop");
    EXPECT_EQ(choice.at("message").at("role"), "assistant");
    const std::string content = message_content(response.json);
    const auto tag = parse_session_tag_text(content);
    ASSERT_TRUE(tag);
    EXPECT_EQ(tag->forum_id, "lobby");
    EXPECT_EQ(body_after_tag(content), "**Guide:** Hello");

    const auto epoch = application_->context_epoch();
    const auto sessions = application_->list_sessions("lobby", epoch);
    ASSERT_EQ(sessions.size(), 1u);
    EXPECT_EQ(sessions.front().id, tag->session_id);
    const auto snapshot = transcript_of(*application_, tag->session_id);
    EXPECT_FALSE(transcript_contains(snapshot, "[//]: # (cha"));
    EXPECT_FALSE(transcript_contains(
        snapshot, "earlier history that CHA must ignore"));
    EXPECT_TRUE(transcript_contains(snapshot, "Hello"));
    EXPECT_EQ(
        application_->get_forum("lobby", epoch).summary.default_character_id,
        "guide");
}

TEST_F(TurnTest, ContinuesATaggedSessionAndRejectsAMissingTag) {
    open_application();
    const auto first = post(nlohmann::json::array({user_message("Hello")}));
    ASSERT_EQ(first.status, 200) << first.raw;
    const std::string first_content = message_content(first.json);
    const auto tag = parse_session_tag_text(first_content);
    ASSERT_TRUE(tag);

    const auto second = post(nlohmann::json::array({
        user_message("Hello"),
        {{"role", "assistant"}, {"content", first_content}},
        user_message("Again"),
    }));
    ASSERT_EQ(second.status, 200) << second.raw;
    const auto continued = parse_session_tag_text(message_content(second.json));
    ASSERT_TRUE(continued);
    EXPECT_EQ(continued->session_id, tag->session_id);
    EXPECT_EQ(body_after_tag(message_content(second.json)), "**Guide:** Again");
    const auto snapshot = transcript_of(*application_, tag->session_id);
    EXPECT_TRUE(transcript_contains(snapshot, "Hello"));
    EXPECT_TRUE(transcript_contains(snapshot, "Again"));
    EXPECT_EQ(application_->list_sessions("lobby", application_->context_epoch())
                  .size(),
        1u);

    const auto missing = post(nlohmann::json::array({
        {{"role", "assistant"},
         {"content", "[//]: # (cha lobby/missing-session)\n\n**Guide:** no"}},
        user_message("Hello"),
    }));
    EXPECT_EQ(missing.status, 404);
    EXPECT_EQ(application_->list_sessions("lobby", application_->context_epoch())
                  .size(),
        1u);
}

TEST_F(TurnTest, PastedUserTagCreatesASeparateSession) {
    open_application();
    const auto first = post(nlohmann::json::array({user_message("Hello")}));
    ASSERT_EQ(first.status, 200);
    const auto tag = parse_session_tag_text(message_content(first.json));
    ASSERT_TRUE(tag);
    const auto pasted = post(nlohmann::json::array({
        user_message(format_session_tag(tag->forum_id, tag->session_id)),
    }));
    ASSERT_EQ(pasted.status, 200) << pasted.raw;
    const auto other =
        parse_session_tag_text(message_content(pasted.json));
    ASSERT_TRUE(other);
    EXPECT_NE(other->session_id, tag->session_id);
    EXPECT_EQ(application_->list_sessions("lobby", application_->context_epoch())
                  .size(),
        2u);
    EXPECT_FALSE(transcript_contains(
        transcript_of(*application_, tag->session_id),
        format_session_tag(tag->forum_id, tag->session_id)));
}

TEST_F(TurnTest, RendersPlainMentionMulticastRecordedAndRejections) {
    add_member(workspace_, "sage", "Sage");
    open_application();
    const auto epoch = application_->context_epoch();
    EXPECT_EQ(
        application_->get_forum("lobby", epoch).summary.default_character_id,
        "guide");

    const auto plain = post(nlohmann::json::array({user_message("Hello")}));
    ASSERT_EQ(plain.status, 200) << plain.raw;
    EXPECT_EQ(body_after_tag(message_content(plain.json)), "**Guide:** Hello");
    const auto tag = parse_session_tag_text(message_content(plain.json));
    ASSERT_TRUE(tag);

    const auto mentioned = post(nlohmann::json::array({
        user_message("Hello"),
        {{"role", "assistant"}, {"content", message_content(plain.json)}},
        user_message("@Sage hello"),
    }));
    ASSERT_EQ(mentioned.status, 200) << mentioned.raw;
    EXPECT_EQ(
        body_after_tag(message_content(mentioned.json)), "**Sage:** hello");
    EXPECT_EQ(
        application_->get_forum("lobby", epoch).summary.default_character_id,
        "guide");

    const auto recorded = post(nlohmann::json::array({
        user_message("Hello"),
        {{"role", "assistant"}, {"content", message_content(plain.json)}},
        user_message("@- keep this note"),
    }));
    ASSERT_EQ(recorded.status, 200) << recorded.raw;
    EXPECT_EQ(body_after_tag(message_content(recorded.json)), "(recorded)");
    const auto after_note = transcript_of(*application_, tag->session_id);
    EXPECT_TRUE(transcript_contains(after_note, "keep this note"));

    const auto multicast = post(nlohmann::json::array(
        {user_message("/mcast @Guide, @Sage. Question")}));
    ASSERT_EQ(multicast.status, 200) << multicast.raw;
    EXPECT_EQ(
        body_after_tag(message_content(multicast.json)),
        "**Guide:** Question\n\n**Sage:** Question");

    const auto before = transcript_of(*application_, tag->session_id).transcript.size();
    const auto rejected = post(nlohmann::json::array({
        user_message("Hello"),
        {{"role", "assistant"}, {"content", message_content(plain.json)}},
        user_message("/etc/hosts"),
    }));
    EXPECT_EQ(rejected.status, 400);
    EXPECT_NE(rejected.json.at("error").at("message").get<std::string>().find(
                  "Unknown command"),
        std::string::npos);
    EXPECT_EQ(
        transcript_of(*application_, tag->session_id).transcript.size(), before);

    const auto empty_mention =
        post(nlohmann::json::array({user_message("@Guide")}));
    EXPECT_EQ(empty_mention.status, 400);
    EXPECT_EQ(application_->list_sessions("lobby", epoch).size(), 2u);

    const auto failed_multicast =
        post(nlohmann::json::array({user_message("/mcast @Nobody hello")}));
    EXPECT_EQ(failed_multicast.status, 400);
    EXPECT_EQ(application_->list_sessions("lobby", epoch).size(), 2u);

    const auto fresh_reject =
        post(nlohmann::json::array({user_message("/not-a-command")}));
    EXPECT_EQ(fresh_reject.status, 400);
    EXPECT_EQ(application_->list_sessions("lobby", epoch).size(), 2u);
}

TEST_F(TurnTest, JevAcceptanceKeepsTheForumDefaultAndRejectionUsesTheFallback) {
    add_member(workspace_, "sage", "Sage");
    MockHttpServer success({http_response(
        "application/json",
        R"({"answers":{"recipient":{"type":"choice","choice":"all_characters"}}})")});
    const auto directory = workspace_.root() / "system/jev";
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "config.toml")
        << "url = \"http://127.0.0.1:" << success.port()
        << "/decisions\"\nmodel = \"jev\"\napi_key = \"api_key_1\"\n";
    open_application();
    const auto epoch = application_->context_epoch();
    ASSERT_EQ(
        application_->create_api_key(
            {.display_name = "Jev", .value = "jev-secret"}, epoch)
            .id,
        "api_key_1");
    success.start();
    const auto accepted = post(nlohmann::json::array({user_message("Hello")}));
    ASSERT_EQ(accepted.status, 200) << accepted.raw;
    EXPECT_EQ(
        body_after_tag(message_content(accepted.json)),
        "**Guide:** Hello\n\n**Sage:** Hello");
    EXPECT_EQ(
        application_->get_forum("lobby", epoch).summary.default_character_id,
        "guide");
    const auto tag = parse_session_tag_text(message_content(accepted.json));
    ASSERT_TRUE(tag);
    const auto follow = post(nlohmann::json::array({
        user_message("Hello"),
        {{"role", "assistant"}, {"content", message_content(accepted.json)}},
        user_message("Next"),
    }));
    ASSERT_EQ(follow.status, 200) << follow.raw;
    EXPECT_EQ(
        body_after_tag(message_content(follow.json)),
        "**Guide:** Next\n\n**Sage:** Next");
    success.join();

    test::TestWorkspace failed_workspace;
    add_member(failed_workspace, "sage", "Sage");
    MockHttpServer failure({
        "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n"
        "Connection: close\r\n\r\n"});
    const auto failed_dir = failed_workspace.root() / "system/jev";
    std::filesystem::create_directories(failed_dir);
    std::ofstream(failed_dir / "config.toml")
        << "url = \"http://127.0.0.1:" << failure.port()
        << "/decisions\"\nmodel = \"jev\"\napi_key = \"api_key_1\"\n";
    const auto failed_database =
        test::import_test_database(failed_workspace.root());
    auto failed_app = Application::open(
        make_command(failed_workspace, failed_database));
    ASSERT_EQ(
        failed_app->create_api_key(
            {.display_name = "Jev", .value = "jev-secret"},
            failed_app->context_epoch())
            .id,
        "api_key_1");
    failure.start();
    std::atomic<bool> stop{false};
    DaemonShutdown shutdown{stop};
    const auto rejected = exchange(
        *failed_app,
        {.method = "POST",
         .document_uri = "/v1/chat/completions",
         .body = chat_body(
                     "lobby",
                     nlohmann::json::array({user_message("Hello")}))
                     .dump()},
        shutdown);
    ASSERT_EQ(rejected.status, 200) << rejected.raw;
    EXPECT_EQ(body_after_tag(message_content(rejected.json)), "**Guide:** Hello");
    EXPECT_FALSE(rejected.json.contains("error"));
    failure.join();
}

TEST_F(TurnTest, StreamAgreesWithTheFinalTextAndOmitsReasoning) {
    open_application();
    const auto complete = post(nlohmann::json::array({user_message("Hello")}));
    ASSERT_EQ(complete.status, 200);
    const auto streamed = post(
        nlohmann::json::array({user_message("Hello")}), true);
    EXPECT_EQ(streamed.status, 200) << streamed.raw;
    EXPECT_NE(streamed.raw.find("text/event-stream"), std::string::npos);
    EXPECT_EQ(
        body_after_tag(message_content(complete.json)),
        body_after_tag(streamed_text(streamed.raw)));
    EXPECT_EQ(streamed_text(streamed.raw).find("HelloHello"), std::string::npos);
    EXPECT_NE(streamed.raw.find("data: [DONE]"), std::string::npos);
    const auto events = sse_events(streamed.raw);
    ASSERT_GE(events.size(), 2u);
    const std::string id = events.front().at("id").get<std::string>();
    const auto created = events.front().at("created");
    for (const auto& event : events) {
        if (event.contains("error")) continue;
        EXPECT_EQ(event.at("id"), id);
        EXPECT_EQ(event.at("created"), created);
        EXPECT_EQ(event.at("model"), "lobby");
        EXPECT_EQ(event.at("object"), "chat.completion.chunk");
        EXPECT_EQ(event.at("choices").at(0).at("index"), 0);
    }
    EXPECT_EQ(
        events.back().at("choices").at(0).at("finish_reason"), "stop");
    EXPECT_TRUE(events.back().at("choices").at(0).at("delta").empty());

    test::TestWorkspace reasoning_workspace;
    disable_naming(reasoning_workspace);
    const std::string sse_body =
        "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"REASONINGSECRET\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"Visible answer\"}}]}\n\n"
        "data: [DONE]\n\n";
    MockHttpServer server({http_response("text/event-stream", sse_body)});
    use_net_provider(reasoning_workspace, server.port(), true);
    const auto database = test::import_test_database(reasoning_workspace.root());
    auto application = Application::open(
        make_command(reasoning_workspace, database));
    server.start();
    std::atomic<bool> stop{false};
    DaemonShutdown shutdown{stop};
    const auto response = exchange(
        *application,
        {.method = "POST",
         .document_uri = "/v1/chat/completions",
         .body = chat_body(
                     "lobby",
                     nlohmann::json::array({user_message("Hello")}),
                     true)
                     .dump()},
        shutdown);
    server.join();
    EXPECT_EQ(response.status, 200) << response.raw;
    EXPECT_NE(streamed_text(response.raw).find("Visible answer"), std::string::npos)
        << response.raw << "\nrequests=" << server.requests().size();
    EXPECT_EQ(response.raw.find("REASONINGSECRET"), std::string::npos);
}

TEST_F(TurnTest, PrefixMismatchStopsThatReply) {
    add_member(workspace_, "sage", "Sage");
    disable_naming(workspace_);
    HoldingHttp http;
    use_net_provider(workspace_, http.port(), true);
    const auto log_file = workspace_.root() / "prefix.log";
    shutdown_diagnostic_logging();
    initialize_diagnostic_logging(log_file, "warn");
    open_application();

    int fds[2]{};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    UniqueFd server(fds[0]);
    UniqueFd client(fds[1]);
    std::atomic<bool> stop{false};
    DaemonShutdown shutdown{stop, 2s};
    const ScgiRequest request{
        .method = "POST",
        .document_uri = "/v1/chat/completions",
        .body = chat_body(
                    "lobby",
                    nlohmann::json::array({user_message("Hello")}),
                    true)
                    .dump(),
    };
    std::thread worker([&] {
        handle_request(
            *application_, request, server.get(), shutdown);
    });
    struct JoinWorker {
        HoldingHttp& http;
        std::thread& worker;
        ~JoinWorker() {
            http.finish();
            if (worker.joinable()) worker.join();
        }
    } join_worker{http, worker};

    ASSERT_TRUE(http.wait_until_awaiting(5s));
    const std::string partial_event =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Partial\"}}]}\n\n";
    std::ostringstream partial_size;
    partial_size << std::hex << partial_event.size();
    http.send(
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        "Transfer-Encoding: chunked\r\n\r\n"
        + partial_size.str() + "\r\n" + partial_event + "\r\n");
    std::this_thread::sleep_for(300ms);
    http.finish();
    worker.join();
    std::string raw;
    server.close();
    char buffer[1024];
    while (true) {
        const ssize_t count = ::recv(client.get(), buffer, sizeof(buffer), 0);
        if (count <= 0) break;
        raw.append(buffer, static_cast<std::size_t>(count));
    }
    shutdown_diagnostic_logging();
    EXPECT_NE(raw.find("Partial"), std::string::npos) << raw;
    EXPECT_NE(raw.find("\"error\""), std::string::npos) << raw;
    EXPECT_EQ(raw.find("data: [DONE]"), std::string::npos) << raw;
    std::ifstream log(log_file);
    const std::string warnings{
        std::istreambuf_iterator<char>(log), {}};
    EXPECT_NE(
        warnings.find("Streaming reply prefix changed"), std::string::npos)
        << warnings;
}

TEST_F(TurnTest, ProviderFailureUses502AndAnSseError) {
    disable_naming(workspace_);
    MockHttpServer server({
        "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n"
        "Connection: close\r\n\r\n",
        "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 0\r\n"
        "Connection: close\r\n\r\n"});
    use_net_provider(workspace_, server.port(), false);
    open_application();
    server.start();
    const auto complete = post(nlohmann::json::array({user_message("Hello")}));
    EXPECT_EQ(complete.status, 502) << complete.raw;
    EXPECT_EQ(complete.json.at("error").at("code"), "provider_error");
    EXPECT_FALSE(complete.json.contains("choices"));

    const auto streamed = post(
        nlohmann::json::array({user_message("Hello")}), true);
    EXPECT_EQ(streamed.status, 200) << streamed.raw;
    EXPECT_NE(streamed.raw.find("\"error\""), std::string::npos);
    EXPECT_EQ(streamed.raw.find("data: [DONE]"), std::string::npos);
    EXPECT_EQ(streamed.raw.find("\"finish_reason\":\"stop\""), std::string::npos);
    server.join();
}

TEST_F(TurnTest, DisconnectAndShutdownLeaveTheNextRequestAbleToRun) {
    disable_naming(workspace_);
    HoldingHttp http;
    use_net_provider(workspace_, http.port(), false);
    open_application();

    int fds[2]{};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    UniqueFd server_fd(fds[0]);
    UniqueFd client(fds[1]);
    std::atomic<bool> stop{false};
    DaemonShutdown shutdown{stop, 2s};
    std::thread worker([&] {
        handle_request(
            *application_,
            {.method = "POST",
             .document_uri = "/v1/chat/completions",
             .body = chat_body(
                         "lobby",
                         nlohmann::json::array({user_message("Hello")}),
                         true)
                         .dump()},
            server_fd.get(),
            shutdown);
    });
    struct JoinWorker {
        HoldingHttp& http;
        UniqueFd& client;
        std::thread& worker;
        ~JoinWorker() {
            client.close();
            http.finish();
            if (worker.joinable()) worker.join();
        }
    } join_worker{http, client, worker};
    ASSERT_TRUE(http.wait_for_request(5s));
    std::string raw;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (raw.find("text/event-stream") == std::string::npos
        && std::chrono::steady_clock::now() < deadline) {
        char buffer[1024];
        const ssize_t count =
            ::recv(client.get(), buffer, sizeof(buffer), MSG_DONTWAIT);
        if (count > 0) raw.append(buffer, static_cast<std::size_t>(count));
        else std::this_thread::sleep_for(10ms);
    }
    ASSERT_NE(raw.find("text/event-stream"), std::string::npos) << raw;
    const auto tag = parse_session_tag_text(streamed_text(raw));
    ASSERT_TRUE(tag) << raw;
    client.close();
    http.finish();
    worker.join();
    EXPECT_FALSE(stop.load());

    std::atomic<bool> serve{true};
    std::thread responder([&] {
        const std::string body =
            R"({"choices":[{"message":{"content":"Finished"}}]})";
        const std::string message =
            http_message("application/json", body, true);
        while (serve.load()) {
            (void)http.respond_when_ready(message, 200ms);
        }
    });
    const auto next = post(nlohmann::json::array({
        {{"role", "assistant"},
         {"content", format_session_tag(tag->forum_id, tag->session_id)
              + "\n\n"}},
        user_message("Again"),
    }));
    serve.store(false);
    responder.join();
    ASSERT_EQ(next.status, 200) << next.raw;
    EXPECT_EQ(
        parse_session_tag_text(message_content(next.json))->session_id,
        tag->session_id);
    EXPECT_EQ(body_after_tag(message_content(next.json)), "**Guide:** Finished");
    EXPECT_TRUE(transcript_contains(
        transcript_of(*application_, tag->session_id), "Hello"));
    EXPECT_TRUE(transcript_contains(
        transcript_of(*application_, tag->session_id), "Again"));
}

TEST_F(TurnTest, ClosedSessionDoesNotWaitForever) {
    disable_naming(workspace_);
    const std::string body =
        R"({"choices":[{"message":{"content":"Finished"}}]})";
    MockHttpServer server({http_response("application/json", body)});
    server.pause_before_response(1);
    use_net_provider(workspace_, server.port(), false);
    open_application();
    server.start();

    int fds[2]{};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    UniqueFd server_fd(fds[0]);
    UniqueFd client(fds[1]);
    std::atomic<bool> stop{false};
    DaemonShutdown shutdown{stop, 2s};
    std::thread worker([&] {
        handle_request(
            *application_,
            {.method = "POST",
            .document_uri = "/v1/chat/completions",
            .body = chat_body(
                        "lobby",
                        nlohmann::json::array({user_message("Hello")}))
                        .dump()},
            server_fd.get(),
            shutdown);
    });
    ASSERT_TRUE(server.wait_for_requests(1, 5s));
    application_->request_shutdown();
    worker.join();
    server.resume_responses();
    EXPECT_TRUE(application_->join_shutdown(2s));
    (void)client;
}

TEST_F(TurnTest, KeepaliveUsesTheTestClock) {
    disable_naming(workspace_);
    const std::string body =
        R"({"choices":[{"message":{"content":"Finished"}}]})";
    MockHttpServer server({http_response("application/json", body)});
    server.pause_before_response(1);
    use_net_provider(workspace_, server.port(), false);
    open_application();
    server.start();

    std::atomic<std::int64_t> offset{0};
    TurnClock clock = [&] {
        return std::chrono::steady_clock::now()
            + std::chrono::milliseconds(offset.load());
    };
    int fds[2]{};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
    UniqueFd server_fd(fds[0]);
    UniqueFd client(fds[1]);
    std::atomic<bool> stop{false};
    DaemonShutdown shutdown{stop, 2s};
    std::thread worker([&] {
        handle_request(
            *application_,
            {.method = "POST",
             .document_uri = "/v1/chat/completions",
             .body = chat_body(
                         "lobby",
                         nlohmann::json::array({user_message("Hello")}),
                         true)
                         .dump()},
            server_fd.get(),
            shutdown,
            clock);
    });
    std::string raw;
    auto read_until = [&](std::string_view needle) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (raw.find(needle) == std::string::npos
            && std::chrono::steady_clock::now() < deadline) {
            char buffer[1024];
            const ssize_t count =
                ::recv(client.get(), buffer, sizeof(buffer), MSG_DONTWAIT);
            if (count > 0) raw.append(buffer, static_cast<std::size_t>(count));
            else std::this_thread::sleep_for(10ms);
        }
    };
    read_until("text/event-stream");
    ASSERT_NE(raw.find("text/event-stream"), std::string::npos) << raw;
    offset = 15000;
    read_until(": keepalive");
    const auto first = raw.find(": keepalive");
    ASSERT_NE(first, std::string::npos) << raw;
    offset = 30000;
    read_until(": keepalive\n\n: keepalive");
    EXPECT_NE(raw.find(": keepalive\n\n: keepalive"), std::string::npos)
        << raw;
    server.resume_responses();
    worker.join();
    server.join();
}

TEST_F(TurnTest, SubmissionTimeoutStopsTheDaemon) {
    MockHttpServer server({http_response(
        "application/json",
        R"({"answers":{"recipient":{"type":"choice","choice":"undefined"}}})")});
    server.pause_before_response(1);
    const auto directory = workspace_.root() / "system/jev";
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "config.toml")
        << "url = \"http://127.0.0.1:" << server.port()
        << "/decisions\"\nmodel = \"jev\"\napi_key = \"api_key_1\"\n";
    RuntimeSettings settings;
    settings.command_deadline = 200ms;
    open_application(settings);
    ASSERT_EQ(
        application_->create_api_key(
            {.display_name = "Jev", .value = "jev-secret"},
            application_->context_epoch())
            .id,
        "api_key_1");
    server.start();
    std::atomic<bool> stop{false};
    DaemonShutdown shutdown{stop, 1s};
    const auto response = post(
        nlohmann::json::array({user_message("Hello")}), false, &shutdown);
    EXPECT_EQ(response.status, 500) << response.raw;
    EXPECT_EQ(response.json.at("error").at("code"), "command_timeout");
    EXPECT_TRUE(stop.load());
    server.resume_responses();
    server.join();
}

TEST_F(TurnTest, AlternatesSessionsAndAppendsEdits) {
    open_application();
    std::vector<std::string> contents;
    for (int index = 0; index < 9; ++index) {
        const auto response = post(nlohmann::json::array(
            {user_message("Session " + std::to_string(index))}));
        ASSERT_EQ(response.status, 200) << response.raw;
        contents.push_back(message_content(response.json));
    }
    EXPECT_EQ(
        application_->list_sessions("lobby", application_->context_epoch())
            .size(),
        9u);
    const auto first_tag = parse_session_tag_text(contents.front());
    ASSERT_TRUE(first_tag);
    const auto continued = post(nlohmann::json::array({
        user_message("Session 0"),
        {{"role", "assistant"}, {"content", contents.front()}},
        user_message("continued"),
    }));
    ASSERT_EQ(continued.status, 200) << continued.raw;
    EXPECT_EQ(
        parse_session_tag_text(message_content(continued.json))->session_id,
        first_tag->session_id);
    EXPECT_TRUE(transcript_contains(
        transcript_of(*application_, first_tag->session_id), "Session 0"));
    EXPECT_TRUE(transcript_contains(
        transcript_of(*application_, first_tag->session_id), "continued"));

    const auto edited = post(nlohmann::json::array({
        user_message("Session 0"),
        {{"role", "assistant"}, {"content", contents.front()}},
        user_message("edited follow-up"),
    }));
    ASSERT_EQ(edited.status, 200) << edited.raw;
    const auto after = transcript_of(*application_, first_tag->session_id);
    EXPECT_TRUE(transcript_contains(after, "Session 0"));
    EXPECT_TRUE(transcript_contains(after, "continued"));
    EXPECT_TRUE(transcript_contains(after, "edited follow-up"));

    const auto solo = post(nlohmann::json::array({user_message("only once")}));
    ASSERT_EQ(solo.status, 200);
    const auto solo_tag = parse_session_tag_text(message_content(solo.json));
    ASSERT_TRUE(solo_tag);
    const auto replaced =
        post(nlohmann::json::array({user_message("replacement")}));
    ASSERT_EQ(replaced.status, 200);
    const auto replaced_tag =
        parse_session_tag_text(message_content(replaced.json));
    ASSERT_TRUE(replaced_tag);
    EXPECT_NE(replaced_tag->session_id, solo_tag->session_id);
    EXPECT_TRUE(transcript_contains(
        transcript_of(*application_, solo_tag->session_id), "only once"));
    EXPECT_FALSE(transcript_contains(
        transcript_of(*application_, solo_tag->session_id), "replacement"));
}

} // namespace
} // namespace cha::daemon
