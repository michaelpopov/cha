#include "providers/openai_oauth.h"

#include "util/private_filesystem.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
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

namespace cha {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

constexpr std::string_view kStartUrl =
    "https://auth.openai.com/api/accounts/deviceauth/usercode";
constexpr std::string_view kPollUrl =
    "https://auth.openai.com/api/accounts/deviceauth/token";
constexpr std::string_view kTokenUrl = "https://auth.openai.com/oauth/token";
constexpr std::string_view kVerifyUrl = "https://auth.openai.com/codex/device";
constexpr std::string_view kClientId = "app_EMoamEEZ73f0CkXaXp7hrann";

std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

std::string base64url_encode(std::string_view input) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string encoded;
    int value = 0;
    int bits = -6;
    for (const unsigned char character : input) {
        value = (value << 8) + character;
        bits += 8;
        while (bits >= 0) {
            encoded.push_back(table[(value >> bits) & 0x3f]);
            bits -= 6;
        }
    }
    if (bits > -6) {
        encoded.push_back(table[((value << 8) >> (bits + 8)) & 0x3f]);
    }
    return encoded;
}

std::string jwt_for_account(std::string_view account_id) {
    const Json header = {{"alg", "none"}, {"typ", "JWT"}};
    const Json payload = {
        {"https://api.openai.com/auth",
         {{"chatgpt_account_id", std::string(account_id)}}}};
    return base64url_encode(header.dump()) + "."
        + base64url_encode(payload.dump()) + ".sig";
}

Json token_payload(
    std::string_view access_token,
    std::string_view refresh_token,
    int expires_in = 3600) {
    return {
        {"access_token", access_token},
        {"refresh_token", refresh_token},
        {"expires_in", expires_in},
    };
}

OpenAiOAuthHttpResponse json_response(long status, const Json& body) {
    return {.status = status, .body = body.dump()};
}

bool contains_secret(std::string_view text) {
    return text.find("fixture-device") != std::string_view::npos
        || text.find("fixture-code") != std::string_view::npos
        || text.find("fixture-verifier") != std::string_view::npos
        || text.find("fixture-access") != std::string_view::npos
        || text.find("fixture-refresh") != std::string_view::npos
        || text.find("fixture-denial") != std::string_view::npos
        || text.find("authorization_code") != std::string_view::npos
        || text.find("code_verifier") != std::string_view::npos
        || text.find("chatgpt_account_id") != std::string_view::npos;
}

void expect_sanitized(const std::optional<std::string>& error) {
    ASSERT_TRUE(error.has_value());
    EXPECT_FALSE(error->empty());
    EXPECT_FALSE(contains_secret(*error));
}

class OpenAiOAuthTest : public testing::Test {
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
            / ("cha_openai_oauth_"
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

    std::int64_t unix_now() {
        std::lock_guard lock(state_->mutex);
        return std::chrono::duration_cast<std::chrono::seconds>(
                   state_->now.time_since_epoch())
            .count();
    }

    std::vector<OpenAiOAuthHttpRequest> requests() const {
        std::lock_guard lock(state_->mutex);
        return state_->requests;
    }

    void write_bundle(
        std::string access_token,
        std::string refresh_token,
        std::int64_t expires_at,
        std::string account_id) {
        Json object;
        object["access_token"] = std::move(access_token);
        object["refresh_token"] = std::move(refresh_token);
        object["expires_at"] = expires_at;
        object["account_id"] = std::move(account_id);
        create_private_file(path_, object.dump());
    }

    OpenAiOAuthHttpResponse start_ok(const Json& interval) {
        return json_response(
            200,
            {{"device_auth_id", "fixture-device"},
             {"user_code", "TEST-ONLY"},
             {"interval", interval}});
    }

    OpenAiOAuthHttpResponse poll_approved() {
        return json_response(
            200,
            {{"authorization_code", "fixture-code"},
             {"code_verifier", "fixture-verifier"}});
    }

    OpenAiOAuthHttpResponse tokens_ok(
        std::string_view refresh = "fixture-refresh",
        int expires_in = 3600) {
        return json_response(
            200,
            token_payload(jwt_for_account("acct_test"), refresh, expires_in));
    }

    OpenAiOAuthSnapshot complete_login(
        OpenAiOAuth& oauth,
        std::string_view refresh = "fixture-refresh") {
        push(start_ok(1));
        EXPECT_EQ(oauth.start().state, OpenAiOAuthState::waiting);
        advance(1s);
        push(poll_approved());
        push(tokens_ok(refresh));
        return oauth.poll();
    }

    std::shared_ptr<State> state_;
    std::filesystem::path directory_;
    std::filesystem::path path_;
};

TEST_F(OpenAiOAuthTest, MissingFileIsSignedOut) {
    OpenAiOAuth oauth = make_owner();
    const OpenAiOAuthSnapshot snapshot = oauth.status();
    EXPECT_EQ(snapshot.state, OpenAiOAuthState::signed_out);
    EXPECT_FALSE(snapshot.user_code);
    EXPECT_FALSE(snapshot.verification_url);
    EXPECT_FALSE(snapshot.attempt_expires_at);
    EXPECT_FALSE(snapshot.next_poll_delay_ms);
    EXPECT_FALSE(snapshot.error);
    EXPECT_FALSE(std::filesystem::exists(path_));
}

TEST_F(OpenAiOAuthTest, InvalidFileIsSignedOutWithSanitizedError) {
    std::ofstream(path_) << "{not-json";
    OpenAiOAuth oauth = make_owner();
    const OpenAiOAuthSnapshot snapshot = oauth.status();
    EXPECT_EQ(snapshot.state, OpenAiOAuthState::signed_out);
    expect_sanitized(snapshot.error);
}

TEST_F(OpenAiOAuthTest, ExpiredBundleLoadsConnectedWithoutRefresh) {
    write_bundle(
        jwt_for_account("acct_test"),
        "fixture-refresh",
        unix_now() - 10,
        "acct_test");
    OpenAiOAuth oauth = make_owner();
    EXPECT_EQ(oauth.status().state, OpenAiOAuthState::connected);
    EXPECT_TRUE(requests().empty());
}

TEST_F(OpenAiOAuthTest, StartUsesIntervalStringAndWaitsBeforeFirstPoll) {
    OpenAiOAuth oauth = make_owner();
    push(start_ok("5"));
    const OpenAiOAuthSnapshot started = oauth.start();
    EXPECT_EQ(started.state, OpenAiOAuthState::waiting);
    EXPECT_EQ(started.user_code, "TEST-ONLY");
    EXPECT_EQ(started.verification_url, kVerifyUrl);
    EXPECT_EQ(started.attempt_expires_at, unix_now() + 15 * 60);
    EXPECT_EQ(started.next_poll_delay_ms, 5000);
    ASSERT_EQ(requests().size(), 1U);
    EXPECT_EQ(requests()[0].url, kStartUrl);
    EXPECT_EQ(requests()[0].content_type, "application/json");
    EXPECT_EQ(requests()[0].timeout, 15s);
    const Json body = Json::parse(requests()[0].body);
    EXPECT_EQ(body.at("client_id").get<std::string>(), kClientId);

    const OpenAiOAuthSnapshot early = oauth.poll();
    EXPECT_EQ(early.state, OpenAiOAuthState::waiting);
    EXPECT_EQ(requests().size(), 1U);

    const OpenAiOAuthSnapshot again = oauth.start();
    EXPECT_EQ(again.user_code, "TEST-ONLY");
    EXPECT_EQ(requests().size(), 1U);
}

TEST_F(OpenAiOAuthTest, StartClampsTinyIntervalToOneSecond) {
    OpenAiOAuth oauth = make_owner();
    push(start_ok(0));
    const OpenAiOAuthSnapshot started = oauth.start();
    EXPECT_EQ(started.next_poll_delay_ms, 1000);
}

TEST_F(OpenAiOAuthTest, StartWhileConnectedRequiresDisconnect) {
    OpenAiOAuth oauth = make_owner();
    EXPECT_EQ(complete_login(oauth).state, OpenAiOAuthState::connected);
    const std::size_t before = requests().size();
    const OpenAiOAuthSnapshot snapshot = oauth.start();
    EXPECT_EQ(snapshot.state, OpenAiOAuthState::connected);
    expect_sanitized(snapshot.error);
    EXPECT_EQ(requests().size(), before);
}

TEST_F(OpenAiOAuthTest, PendingStatusesDoNotExposeSecrets) {
    OpenAiOAuth oauth = make_owner();
    push(start_ok(1));
    ASSERT_EQ(oauth.start().state, OpenAiOAuthState::waiting);
    advance(1s);

    push(json_response(403, {{"error", "fixture-denial"}}));
    OpenAiOAuthSnapshot snapshot = oauth.poll();
    EXPECT_EQ(snapshot.state, OpenAiOAuthState::waiting);
    EXPECT_FALSE(snapshot.error);
    EXPECT_FALSE(contains_secret(snapshot.user_code.value_or("")));

    advance(1s);
    push(json_response(404, {{"error", "fixture-denial"}}));
    snapshot = oauth.poll();
    EXPECT_EQ(snapshot.state, OpenAiOAuthState::waiting);

    advance(1s);
    push(json_response(400, {{"error", "deviceauth_authorization_pending"}}));
    snapshot = oauth.poll();
    EXPECT_EQ(snapshot.state, OpenAiOAuthState::waiting);
    EXPECT_EQ(snapshot.next_poll_delay_ms, 1000);
}

TEST_F(OpenAiOAuthTest, SlowDownAddsFiveSeconds) {
    OpenAiOAuth oauth = make_owner();
    push(start_ok(1));
    ASSERT_EQ(oauth.start().state, OpenAiOAuthState::waiting);
    advance(1s);
    push(json_response(400, {{"error", {{"code", "slow_down"}}}}));
    const OpenAiOAuthSnapshot snapshot = oauth.poll();
    EXPECT_EQ(snapshot.state, OpenAiOAuthState::waiting);
    EXPECT_EQ(snapshot.next_poll_delay_ms, 6000);
}

TEST_F(OpenAiOAuthTest, DenialEndsAttemptWithSanitizedError) {
    OpenAiOAuth oauth = make_owner();
    push(start_ok(1));
    ASSERT_EQ(oauth.start().state, OpenAiOAuthState::waiting);
    advance(1s);
    push(json_response(400, {{"error", "fixture-denial"}}));
    const OpenAiOAuthSnapshot snapshot = oauth.poll();
    EXPECT_EQ(snapshot.state, OpenAiOAuthState::signed_out);
    expect_sanitized(snapshot.error);
    EXPECT_FALSE(std::filesystem::exists(path_));
}

TEST_F(OpenAiOAuthTest, Start404IsFailureNotPending) {
    OpenAiOAuth oauth = make_owner();
    push(json_response(404, {{"error", "deviceauth_authorization_pending"}}));
    const OpenAiOAuthSnapshot snapshot = oauth.start();
    EXPECT_EQ(snapshot.state, OpenAiOAuthState::signed_out);
    expect_sanitized(snapshot.error);
}

TEST_F(OpenAiOAuthTest, ApprovalExchangeSavesUnixExpiryAndAccount) {
    OpenAiOAuth oauth = make_owner();
    push(start_ok(1));
    ASSERT_EQ(oauth.start().state, OpenAiOAuthState::waiting);
    advance(1s);
    push(poll_approved());
    push(tokens_ok());
    const OpenAiOAuthSnapshot snapshot = oauth.poll();
    EXPECT_EQ(snapshot.state, OpenAiOAuthState::connected);
    EXPECT_FALSE(snapshot.user_code);
    EXPECT_FALSE(snapshot.verification_url);
    EXPECT_FALSE(snapshot.error);

    const auto calls = requests();
    ASSERT_EQ(calls.size(), 3U);
    EXPECT_EQ(calls[1].url, kPollUrl);
    EXPECT_EQ(calls[2].url, kTokenUrl);
    EXPECT_EQ(calls[2].content_type, "application/x-www-form-urlencoded");
    EXPECT_NE(calls[2].body.find("grant_type=authorization_code"), std::string::npos);
    EXPECT_NE(
        calls[2].body.find(
            "redirect_uri=https%3A%2F%2Fauth.openai.com%2Fdeviceauth%2Fcallback"),
        std::string::npos);
    EXPECT_EQ(calls[1].timeout, 15s);
    EXPECT_EQ(calls[2].timeout, 15s);

    const Json stored = Json::parse(file_bytes(path_));
    EXPECT_EQ(stored.at("account_id"), "acct_test");
    EXPECT_EQ(stored.at("expires_at"), unix_now() + 3600);
    EXPECT_EQ(stored.at("refresh_token"), "fixture-refresh");
    EXPECT_FALSE(stored.contains("device_auth_id"));
#ifndef _WIN32
    struct stat info {};
    ASSERT_EQ(::lstat(path_.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, static_cast<mode_t>(0600));
#endif

    OpenAiOAuth restarted{path_, transport(), clock()};
    EXPECT_EQ(restarted.status().state, OpenAiOAuthState::connected);
    EXPECT_EQ(requests().size(), 3U);
}

TEST_F(OpenAiOAuthTest, ExchangeSharesTheFifteenSecondBudget) {
    OpenAiOAuth oauth = make_owner();
    push(start_ok(1));
    ASSERT_EQ(oauth.start().state, OpenAiOAuthState::waiting);
    advance(1s);
    state_->on_request = [this](const OpenAiOAuthHttpRequest& request) {
        if (request.url == kPollUrl) advance(2s);
    };
    push(poll_approved());
    push(tokens_ok());
    ASSERT_EQ(oauth.poll().state, OpenAiOAuthState::connected);
    const auto calls = requests();
    ASSERT_GE(calls.size(), 3U);
    EXPECT_EQ(calls[1].timeout, 15s);
    EXPECT_EQ(calls[2].timeout, 13s);
}

TEST_F(OpenAiOAuthTest, DeadlineExpiresAttemptWithoutNetwork) {
    OpenAiOAuth oauth = make_owner();
    push(start_ok(1));
    ASSERT_EQ(oauth.start().state, OpenAiOAuthState::waiting);
    advance(15min);
    const OpenAiOAuthSnapshot snapshot = oauth.poll();
    EXPECT_EQ(snapshot.state, OpenAiOAuthState::signed_out);
    expect_sanitized(snapshot.error);
    EXPECT_EQ(requests().size(), 1U);
}

TEST_F(OpenAiOAuthTest, NetworkDeadlineSkipsExchange) {
    OpenAiOAuth oauth = make_owner();
    push(start_ok(1));
    ASSERT_EQ(oauth.start().state, OpenAiOAuthState::waiting);
    advance(1s);
    state_->on_request = [this](const OpenAiOAuthHttpRequest& request) {
        if (request.url == kPollUrl) advance(15s);
    };
    push(poll_approved());
    const OpenAiOAuthSnapshot snapshot = oauth.poll();
    EXPECT_EQ(snapshot.state, OpenAiOAuthState::signed_out);
    expect_sanitized(snapshot.error);
    EXPECT_EQ(requests().size(), 2U);
}

TEST_F(OpenAiOAuthTest, DisconnectCancelsWaitingLogin) {
    OpenAiOAuth oauth = make_owner();
    push(start_ok(1));
    ASSERT_EQ(oauth.start().state, OpenAiOAuthState::waiting);
    const OpenAiOAuthSnapshot snapshot = oauth.disconnect();
    EXPECT_EQ(snapshot.state, OpenAiOAuthState::signed_out);
    EXPECT_FALSE(snapshot.user_code);
    EXPECT_EQ(oauth.poll().state, OpenAiOAuthState::signed_out);
    EXPECT_EQ(requests().size(), 1U);
}

TEST_F(OpenAiOAuthTest, TokenResponseRequiresAccountClaimAndRefreshToken) {
    OpenAiOAuth oauth = make_owner();
    push(start_ok(1));
    ASSERT_EQ(oauth.start().state, OpenAiOAuthState::waiting);
    advance(1s);
    push(poll_approved());
    push(json_response(
        200,
        {{"access_token", "fixture-access"},
         {"refresh_token", "fixture-refresh"},
         {"expires_in", 3600}}));
    EXPECT_EQ(oauth.poll().state, OpenAiOAuthState::signed_out);

    OpenAiOAuth again = make_owner();
    push(start_ok(1));
    ASSERT_EQ(again.start().state, OpenAiOAuthState::waiting);
    advance(1s);
    push(poll_approved());
    push(json_response(
        200,
        {{"access_token", jwt_for_account("acct_test")},
         {"expires_in", 3600}}));
    const OpenAiOAuthSnapshot snapshot = again.poll();
    EXPECT_EQ(snapshot.state, OpenAiOAuthState::signed_out);
    expect_sanitized(snapshot.error);
}

TEST_F(OpenAiOAuthTest, CredentialsRefreshNearExpiryAndPersistReplacement) {
    write_bundle(
        jwt_for_account("acct_old"),
        "old-refresh",
        unix_now() + 60,
        "acct_old");
    OpenAiOAuth oauth = make_owner();
    push(json_response(
        200,
        token_payload(jwt_for_account("acct_new"), "new-refresh", 3600)));
    const OpenAiOAuthRequestCredentials credentials = oauth.credentials();
    EXPECT_EQ(credentials.account_id, "acct_new");
    EXPECT_EQ(credentials.access_token, jwt_for_account("acct_new"));
    ASSERT_EQ(requests().size(), 1U);
    EXPECT_EQ(requests()[0].url, kTokenUrl);
    EXPECT_NE(
        requests()[0].body.find("grant_type=refresh_token"),
        std::string::npos);
    EXPECT_NE(requests()[0].body.find("refresh_token=old-refresh"), std::string::npos);
    const Json stored = Json::parse(file_bytes(path_));
    EXPECT_EQ(stored.at("refresh_token"), "new-refresh");
    EXPECT_EQ(stored.at("account_id"), "acct_new");
    EXPECT_EQ(stored.at("expires_at"), unix_now() + 3600);
}

TEST_F(OpenAiOAuthTest, FreshCredentialsSkipRefresh) {
    write_bundle(
        jwt_for_account("acct_test"),
        "fixture-refresh",
        unix_now() + 3600,
        "acct_test");
    OpenAiOAuth oauth = make_owner();
    const OpenAiOAuthRequestCredentials credentials = oauth.credentials();
    EXPECT_EQ(credentials.account_id, "acct_test");
    EXPECT_TRUE(requests().empty());
}

TEST_F(OpenAiOAuthTest, FailedRefreshSignsOutWaitersWithoutRetry) {
    write_bundle(
        jwt_for_account("acct_test"),
        "old-refresh",
        unix_now() + 60,
        "acct_test");
    OpenAiOAuth oauth = make_owner();
    push(json_response(400, {{"error", "fixture-denial"}}));

    std::atomic<int> failures{0};
    auto call = [&] {
        try {
            (void)oauth.credentials();
        } catch (const std::runtime_error& error) {
            EXPECT_FALSE(contains_secret(error.what()));
            ++failures;
        }
    };
    std::thread first(call);
    std::thread second(call);
    first.join();
    second.join();

    EXPECT_EQ(failures, 2);
    EXPECT_EQ(requests().size(), 1U);
    EXPECT_EQ(oauth.status().state, OpenAiOAuthState::signed_out);
    EXPECT_FALSE(std::filesystem::exists(path_));
}

TEST_F(OpenAiOAuthTest, ConcurrentNearExpiryRefreshHappensOnce) {
    write_bundle(
        jwt_for_account("acct_old"),
        "old-refresh",
        unix_now() + 60,
        "acct_old");
    OpenAiOAuth oauth = make_owner();
    push(json_response(
        200,
        token_payload(jwt_for_account("acct_new"), "new-refresh", 3600)));

    OpenAiOAuthRequestCredentials first_result;
    OpenAiOAuthRequestCredentials second_result;
    std::thread first([&] { first_result = oauth.credentials(); });
    std::thread second([&] { second_result = oauth.credentials(); });
    first.join();
    second.join();

    EXPECT_EQ(first_result.account_id, "acct_new");
    EXPECT_EQ(second_result.account_id, "acct_new");
    EXPECT_EQ(requests().size(), 1U);
    EXPECT_EQ(Json::parse(file_bytes(path_)).at("refresh_token"), "new-refresh");
}

TEST_F(OpenAiOAuthTest, DisconnectAfterInFlightExchangeRemainsEffective) {
    OpenAiOAuth oauth = make_owner();
    push(start_ok(1));
    ASSERT_EQ(oauth.start().state, OpenAiOAuthState::waiting);
    advance(1s);

    std::mutex gate_mutex;
    std::condition_variable gate;
    int phase = 0;
    state_->on_request = [&](const OpenAiOAuthHttpRequest& request) {
        if (request.url != kPollUrl) return;
        std::unique_lock lock(gate_mutex);
        phase = 1;
        gate.notify_all();
        gate.wait(lock, [&] { return phase >= 3; });
    };
    push(poll_approved());
    push(tokens_ok());

    OpenAiOAuthSnapshot polled;
    std::thread poller([&] { polled = oauth.poll(); });
    {
        std::unique_lock lock(gate_mutex);
        gate.wait(lock, [&] { return phase >= 1; });
    }

    OpenAiOAuthSnapshot disconnected;
    std::thread canceller([&] {
        {
            std::lock_guard lock(gate_mutex);
            phase = 2;
            gate.notify_all();
        }
        disconnected = oauth.disconnect();
    });
    {
        std::unique_lock lock(gate_mutex);
        gate.wait(lock, [&] { return phase >= 2; });
        std::this_thread::sleep_for(50ms);
        phase = 3;
        gate.notify_all();
    }
    poller.join();
    canceller.join();

    EXPECT_EQ(polled.state, OpenAiOAuthState::connected);
    EXPECT_EQ(disconnected.state, OpenAiOAuthState::signed_out);
    EXPECT_EQ(oauth.status().state, OpenAiOAuthState::signed_out);
    EXPECT_FALSE(std::filesystem::exists(path_));
}

TEST_F(OpenAiOAuthTest, SaveFailureClearsMemoryAndAttemptsRemoval) {
    path_ = directory_ / "missing-parent" / "cha.sqlite3.openai-auth.json";
    OpenAiOAuth oauth{path_, transport(), clock()};
    push(start_ok(1));
    ASSERT_EQ(oauth.start().state, OpenAiOAuthState::waiting);
    advance(1s);
    push(poll_approved());
    push(tokens_ok());
    const OpenAiOAuthSnapshot snapshot = oauth.poll();
    EXPECT_EQ(snapshot.state, OpenAiOAuthState::signed_out);
    expect_sanitized(snapshot.error);
    EXPECT_FALSE(std::filesystem::exists(path_));
}

TEST_F(OpenAiOAuthTest, CredentialsThrowWhenSignedOut) {
    OpenAiOAuth oauth = make_owner();
    EXPECT_THROW(
        {
            try {
                (void)oauth.credentials();
            } catch (const std::runtime_error& error) {
                EXPECT_FALSE(contains_secret(error.what()));
                throw;
            }
        },
        std::runtime_error);
}

TEST_F(OpenAiOAuthTest, RestartDoesNotKeepPendingLogin) {
    OpenAiOAuth oauth = make_owner();
    push(start_ok(1));
    ASSERT_EQ(oauth.start().state, OpenAiOAuthState::waiting);
    OpenAiOAuth restarted{path_, transport(), clock()};
    EXPECT_EQ(restarted.status().state, OpenAiOAuthState::signed_out);
}

#ifndef _WIN32
TEST_F(OpenAiOAuthTest, RemovalFailureIsReported) {
    OpenAiOAuth oauth = make_owner();
    ASSERT_EQ(complete_login(oauth).state, OpenAiOAuthState::connected);
    ASSERT_EQ(::chmod(directory_.c_str(), 0500), 0);
    const OpenAiOAuthSnapshot snapshot = oauth.disconnect();
    EXPECT_EQ(snapshot.state, OpenAiOAuthState::signed_out);
    expect_sanitized(snapshot.error);
    ASSERT_EQ(::chmod(directory_.c_str(), 0700), 0);
    EXPECT_TRUE(std::filesystem::exists(path_));
}
#endif

TEST(OpenAiOAuthLive, LoginAndRefresh) {
    if (std::getenv("CHA_OPENAI_OAUTH_LIVE") == nullptr) {
        GTEST_SKIP() << "set CHA_OPENAI_OAUTH_LIVE=1 to run the live login";
    }

    const std::filesystem::path directory =
        std::filesystem::temp_directory_path()
        / ("cha_openai_oauth_live_"
           + std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const std::filesystem::path path =
        directory / "cha.sqlite3.openai-auth.json";
    struct Cleanup {
        std::filesystem::path directory;
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove(path, error);
            std::filesystem::remove_all(directory, error);
        }
    } cleanup{directory, path};

    OpenAiOAuth oauth{path};
    const OpenAiOAuthSnapshot started = oauth.start();
    ASSERT_EQ(started.state, OpenAiOAuthState::waiting);
    ASSERT_TRUE(started.user_code);
    ASSERT_TRUE(started.verification_url);
    std::cerr
        << "\nApprove this CHA login in a browser:\n"
        << "  URL:  " << *started.verification_url << "\n"
        << "  Code: " << *started.user_code << "\n"
        << std::flush;

    OpenAiOAuthSnapshot snapshot = started;
    while (snapshot.state == OpenAiOAuthState::waiting) {
        const auto delay = snapshot.next_poll_delay_ms.value_or(1000);
        std::this_thread::sleep_for(std::chrono::milliseconds{delay});
        snapshot = oauth.poll();
        if (snapshot.error) {
            FAIL() << "live login failed with a sanitized error";
        }
    }
    ASSERT_EQ(snapshot.state, OpenAiOAuthState::connected);
    ASSERT_TRUE(std::filesystem::is_regular_file(path));

    Json stored = Json::parse(file_bytes(path));
    const std::string first_refresh = stored.at("refresh_token").get<std::string>();
    stored["expires_at"] = 1;
    create_private_file(path, stored.dump());

    OpenAiOAuth refreshed{path};
    const OpenAiOAuthRequestCredentials credentials = refreshed.credentials();
    EXPECT_FALSE(credentials.access_token.empty());
    EXPECT_FALSE(credentials.account_id.empty());
    const Json after = Json::parse(file_bytes(path));
    const bool rotated =
        after.at("refresh_token").get<std::string>() != first_refresh;
    std::cerr
        << "Live refresh succeeded. Refresh token rotated: "
        << (rotated ? "yes" : "no") << "\n"
        << std::flush;
    EXPECT_EQ(after.at("account_id"), credentials.account_id);

    (void)refreshed.disconnect();
}

} // namespace
} // namespace cha
