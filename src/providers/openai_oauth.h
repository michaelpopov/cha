#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace cha {

enum class OpenAiOAuthState {
    signed_out,
    waiting,
    connected,
};

// Public connection state. Waiting is the only state that includes the user
// code, verification URL, attempt expiry, and next-poll delay. Tokens and
// other private login fields stay inside the owner.
struct OpenAiOAuthSnapshot {
    OpenAiOAuthState state = OpenAiOAuthState::signed_out;
    std::optional<std::string> user_code;
    std::optional<std::string> verification_url;
    std::optional<std::int64_t> attempt_expires_at;
    std::optional<std::int64_t> next_poll_delay_ms;
    std::optional<std::string> error;
};

// Access-token copy for one model request. Callers must release the owner
// before streaming.
struct OpenAiOAuthRequestCredentials {
    std::string access_token;
    std::string account_id;
};

// Test seam for one blocking POST. Production always uses the fixed OpenAI
// HTTPS destinations.
struct OpenAiOAuthHttpRequest {
    std::string url;
    std::string content_type;
    std::string body;
    std::chrono::milliseconds timeout{};
};

struct OpenAiOAuthHttpResponse {
    long status = 0;
    std::string body;
};

using OpenAiOAuthTransport =
    std::function<OpenAiOAuthHttpResponse(const OpenAiOAuthHttpRequest&)>;
using OpenAiOAuthClock =
    std::function<std::chrono::system_clock::time_point()>;

// Owns one workspace ChatGPT login: the credential file, current bundle,
// optional pending device attempt, and the mutex that serializes them.
class OpenAiOAuth {
public:
    explicit OpenAiOAuth(std::filesystem::path credential_path);
    OpenAiOAuth(
        std::filesystem::path credential_path,
        OpenAiOAuthTransport transport,
        OpenAiOAuthClock clock);

    OpenAiOAuth(const OpenAiOAuth&) = delete;
    OpenAiOAuth& operator=(const OpenAiOAuth&) = delete;

    [[nodiscard]] OpenAiOAuthSnapshot status() const;
    OpenAiOAuthSnapshot start();
    OpenAiOAuthSnapshot poll();
    OpenAiOAuthSnapshot disconnect();
    OpenAiOAuthRequestCredentials credentials();

private:
    struct Bundle {
        std::string access_token;
        std::string refresh_token;
        std::int64_t expires_at{};
        std::string account_id;
    };

    struct Attempt {
        std::string device_auth_id;
        std::string user_code;
        std::chrono::milliseconds interval{std::chrono::seconds{1}};
        std::chrono::system_clock::time_point deadline{};
        std::chrono::system_clock::time_point next_poll{};
    };

    [[nodiscard]] OpenAiOAuthSnapshot snapshot_unlocked() const;
    void load_unlocked();
    bool save_unlocked(const Bundle& bundle);
    bool remove_unlocked();
    void abandon_unlocked(std::string fallback_error);
    void discard_expired_attempt_unlocked() const;
    OpenAiOAuthHttpResponse post_unlocked(
        std::string_view path,
        std::string_view content_type,
        std::string body,
        std::chrono::system_clock::time_point deadline);
    bool finish_login_unlocked(
        std::string authorization_code,
        std::string code_verifier,
        std::chrono::system_clock::time_point deadline);
    bool refresh_unlocked();

    std::filesystem::path credential_path_;
    OpenAiOAuthTransport transport_;
    OpenAiOAuthClock clock_;
    mutable std::mutex mutex_;
    std::optional<Bundle> bundle_;
    mutable std::optional<Attempt> pending_;
    mutable std::optional<std::string> error_;
};

} // namespace cha
