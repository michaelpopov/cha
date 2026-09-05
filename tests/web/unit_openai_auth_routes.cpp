#include "providers/openai_oauth.h"
#include "web/http_server.h"
#include "web/openai_auth_routes.h"
#include "web/web_settings.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace cha::web {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

constexpr std::string_view kVerifyUrl = "https://auth.openai.com/codex/device";

bool contains_secret(std::string_view text) {
    return text.find("fixture-device") != std::string_view::npos
        || text.find("fixture-code") != std::string_view::npos
        || text.find("fixture-verifier") != std::string_view::npos
        || text.find("fixture-access") != std::string_view::npos
        || text.find("fixture-refresh") != std::string_view::npos
        || text.find("acct_test") != std::string_view::npos
        || text.find("authorization_code") != std::string_view::npos
        || text.find("code_verifier") != std::string_view::npos;
}

OpenAiOAuthHttpResponse json_response(long status, const Json& body) {
    return {.status = status, .body = body.dump()};
}

class AuthServer {
public:
    AuthServer(OpenAiOAuth& owner, WebSettings settings = {}) {
        OpenAiAuthRoutes(owner, settings).install(server_);
        port_ = server_.bind_to_any_port("127.0.0.1");
        if (port_ < 0) throw std::runtime_error("Could not bind auth test server");
        configure_http_server(server_, settings);
        thread_ = std::thread([this] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }

    ~AuthServer() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    httplib::Client client() const {
        httplib::Client client("127.0.0.1", port_);
        client.set_keep_alive(false);
        return client;
    }

    int port() const noexcept { return port_; }

    void stop() { server_.stop(); }

private:
    httplib::Server server_;
    int port_{};
    std::thread thread_;
};

Json body(const httplib::Result& result) {
    if (!result) {
        ADD_FAILURE() << "HTTP request failed with error "
                      << static_cast<int>(result.error());
        return {};
    }
    return Json::parse(result->body);
}

void expect_error(
    const httplib::Result& result,
    int status,
    std::string_view code,
    std::string_view message = {}) {
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, status);
    EXPECT_EQ(result->get_header_value("Cache-Control"), "no-store");
    const Json json = body(result);
    ASSERT_TRUE(json.contains("error"));
    ASSERT_TRUE(json["error"].is_object());
    EXPECT_EQ(json["error"]["code"].get<std::string>(), code);
    EXPECT_TRUE(json["error"]["message"].is_string());
    EXPECT_FALSE(contains_secret(result->body));
    if (!message.empty()) {
        EXPECT_EQ(json["error"]["message"].get<std::string>(), message);
    }
}

void expect_snapshot(const httplib::Result& result, std::string_view status) {
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 200);
    EXPECT_EQ(result->get_header_value("Cache-Control"), "no-store");
    EXPECT_FALSE(contains_secret(result->body));
    const Json json = body(result);
    EXPECT_EQ(json.at("status").get<std::string>(), status);
    EXPECT_FALSE(json.contains("access_token"));
    EXPECT_FALSE(json.contains("refresh_token"));
    EXPECT_FALSE(json.contains("account_id"));
    EXPECT_FALSE(json.contains("device_auth_id"));
    EXPECT_FALSE(json.contains("authorization_code"));
    EXPECT_FALSE(json.contains("code_verifier"));
    if (status != "waiting") {
        EXPECT_FALSE(json.contains("user_code"));
        EXPECT_FALSE(json.contains("verification_url"));
        EXPECT_FALSE(json.contains("attempt_expires_at"));
        EXPECT_FALSE(json.contains("next_poll_delay_ms"));
    }
}

class OpenAiAuthRoutesTest : public testing::Test {
protected:
    struct State {
        std::mutex mutex;
        std::chrono::system_clock::time_point now{
            std::chrono::system_clock::time_point{std::chrono::seconds{1'700'000'000}}};
        std::vector<OpenAiOAuthHttpRequest> requests;
        std::vector<OpenAiOAuthHttpResponse> responses;
        std::size_t next{};
        std::function<void(const OpenAiOAuthHttpRequest&)> on_request;
    };

    void SetUp() override {
        directory_ = std::filesystem::temp_directory_path()
            / ("cha_openai_auth_routes_"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory_);
        path_ = directory_ / "cha.sqlite3.openai-auth.json";
        state_ = std::make_shared<State>();
    }

    void TearDown() override {
#ifndef _WIN32
        if (!directory_.empty()) {
            (void)::chmod(directory_.c_str(), 0700);
        }
#endif
        std::filesystem::remove_all(directory_);
    }

    OpenAiOAuth make_owner() {
        return OpenAiOAuth{path_, transport(), clock()};
    }

    OpenAiOAuthTransport transport() {
        return [state = state_](const OpenAiOAuthHttpRequest& request) {
            OpenAiOAuthHttpResponse response;
            {
                std::lock_guard lock(state->mutex);
                state->requests.push_back(request);
                if (state->next >= state->responses.size()) {
                    throw std::runtime_error("unexpected OpenAI request");
                }
                response = state->responses[state->next++];
            }
            if (state->on_request) state->on_request(request);
            return response;
        };
    }

    OpenAiOAuthClock clock() {
        return [state = state_] {
            std::lock_guard lock(state->mutex);
            return state->now;
        };
    }

    void push(OpenAiOAuthHttpResponse response) {
        std::lock_guard lock(state_->mutex);
        state_->responses.push_back(std::move(response));
    }

    void advance(std::chrono::milliseconds delay) {
        std::lock_guard lock(state_->mutex);
        state_->now += delay;
    }

    std::vector<OpenAiOAuthHttpRequest> requests() const {
        std::lock_guard lock(state_->mutex);
        return state_->requests;
    }

    OpenAiOAuthHttpResponse start_ok() {
        return json_response(
            200,
            {{"device_auth_id", "fixture-device"},
             {"user_code", "TEST-ONLY"},
             {"interval", 1}});
    }

    OpenAiOAuthHttpResponse poll_pending() {
        return json_response(
            403, {{"error", "deviceauth_authorization_pending"}});
    }

    httplib::Result post(
        AuthServer& server,
        std::string_view path,
        std::string body = "{}",
        std::string content_type = "application/json",
        httplib::Headers headers = {}) {
        return server.client().Post(
            std::string(path), std::move(headers), std::move(body),
            std::move(content_type));
    }

    std::filesystem::path directory_;
    std::filesystem::path path_;
    std::shared_ptr<State> state_;
};

TEST_F(OpenAiAuthRoutesTest, StatusStartsSignedOutWithoutUpstreamWork) {
    OpenAiOAuth owner = make_owner();
    AuthServer server(owner);
    expect_snapshot(server.client().Get("/api/v1/openai/auth"), "signed_out");
    EXPECT_TRUE(requests().empty());
}

TEST_F(OpenAiAuthRoutesTest, LoginPollAndDisconnectFollowTheSharedOwner) {
    push(start_ok());
    push(poll_pending());
    OpenAiOAuth owner = make_owner();
    AuthServer server(owner);

    const auto login = post(server, "/api/v1/openai/auth/login");
    expect_snapshot(login, "waiting");
    const Json waiting = body(login);
    EXPECT_EQ(waiting.at("user_code"), "TEST-ONLY");
    EXPECT_EQ(waiting.at("verification_url"), kVerifyUrl);
    EXPECT_EQ(waiting.at("attempt_expires_at"), 1'700'000'000 + 15 * 60);
    EXPECT_EQ(waiting.at("next_poll_delay_ms"), 1000);

    const auto early = post(server, "/api/v1/openai/auth/poll");
    expect_snapshot(early, "waiting");
    EXPECT_EQ(requests().size(), 1U);

    const auto other_browser = post(server, "/api/v1/openai/auth/login");
    expect_snapshot(other_browser, "waiting");
    EXPECT_EQ(body(other_browser).at("user_code"), "TEST-ONLY");
    EXPECT_EQ(requests().size(), 1U);

    advance(1s);
    const auto polled = post(server, "/api/v1/openai/auth/poll");
    expect_snapshot(polled, "waiting");
    EXPECT_EQ(requests().size(), 2U);

    const auto again = post(server, "/api/v1/openai/auth/poll");
    expect_snapshot(again, "waiting");
    EXPECT_EQ(requests().size(), 2U);

    const auto cancelled = post(server, "/api/v1/openai/auth/disconnect");
    expect_snapshot(cancelled, "signed_out");
    expect_snapshot(server.client().Get("/api/v1/openai/auth"), "signed_out");
}

TEST_F(OpenAiAuthRoutesTest, RejectsMalformedPostsAndKeepsNoStoreOnErrors) {
    OpenAiOAuth owner = make_owner();
    AuthServer server(owner);

    expect_error(
        post(server, "/api/v1/openai/auth/login", "{}", "text/plain"),
        400, "bad_request", "Expected a JSON request body.");
    expect_error(
        post(server, "/api/v1/openai/auth/poll", "[]"),
        400, "bad_request", "Invalid JSON request body.");
    expect_error(
        post(server, "/api/v1/openai/auth/disconnect", R"({"extra":true})"),
        400, "bad_request", "Invalid JSON request body.");

    httplib::Headers foreign{
        {"Content-Type", "application/json"},
        {"Origin", "http://other.example"},
    };
    expect_error(
        post(server, "/api/v1/openai/auth/login", "{}", "application/json",
             foreign),
        403, "forbidden_origin", "Request origin is not allowed.");
    EXPECT_TRUE(requests().empty());
}

TEST_F(OpenAiAuthRoutesTest, AcceptsMatchingOriginAndSurvivesShutdownDuringPoll) {
    push(start_ok());
    push(poll_pending());
    std::mutex gate;
    std::condition_variable started;
    bool entered = false;
    state_->on_request = [&](const OpenAiOAuthHttpRequest&) {
        if (requests().size() < 2) return;
        {
            std::lock_guard lock(gate);
            entered = true;
        }
        started.notify_all();
        std::this_thread::sleep_for(150ms);
    };

    OpenAiOAuth owner = make_owner();
    AuthServer server(owner);
    const httplib::Headers origin{
        {"Origin", "http://127.0.0.1:" + std::to_string(server.port())},
    };
    const auto login = post(
        server, "/api/v1/openai/auth/login", "{}", "application/json", origin);
    expect_snapshot(login, "waiting");

    advance(1s);
    httplib::Result polled;
    std::thread waiter([&] {
        polled = post(server, "/api/v1/openai/auth/poll");
    });
    {
        std::unique_lock lock(gate);
        started.wait_for(lock, 2s, [&] { return entered; });
    }
    server.stop();
    waiter.join();
    expect_snapshot(polled, "waiting");
}

} // namespace
} // namespace cha::web
