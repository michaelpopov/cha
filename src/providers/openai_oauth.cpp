#include "providers/openai_oauth.h"

#include "util/json_serialization.h"
#include "util/logging.h"
#include "util/path_name.h"
#include "util/private_filesystem.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace cha {
namespace {

using Json = nlohmann::json;

constexpr std::string_view auth_origin = "https://auth.openai.com";
constexpr std::string_view client_id = "app_EMoamEEZ73f0CkXaXp7hrann";
constexpr std::string_view verification_url =
    "https://auth.openai.com/codex/device";
constexpr std::string_view redirect_uri =
    "https://auth.openai.com/deviceauth/callback";
constexpr std::string_view start_path = "/api/accounts/deviceauth/usercode";
constexpr std::string_view poll_path = "/api/accounts/deviceauth/token";
constexpr std::string_view token_path = "/oauth/token";
constexpr std::string_view json_content_type = "application/json";
constexpr std::string_view form_content_type =
    "application/x-www-form-urlencoded";

constexpr auto network_budget = std::chrono::seconds{15};
constexpr auto login_limit = std::chrono::minutes{15};
constexpr auto refresh_margin = std::chrono::minutes{5};
constexpr auto min_interval = std::chrono::seconds{1};
constexpr auto slow_down_extra = std::chrono::seconds{5};
constexpr std::size_t max_auth_body_size = 256 * 1024;

constexpr const char* login_failed = "OpenAI login failed.";
constexpr const char* login_timed_out = "OpenAI login timed out.";
constexpr const char* login_expired = "OpenAI login expired.";
constexpr const char* already_connected =
    "Disconnect the current ChatGPT account before connecting another.";
constexpr const char* not_signed_in =
    "Sign in to ChatGPT before using this provider.";
constexpr const char* renew_failed = "OpenAI credentials could not be renewed.";
constexpr const char* save_failed = "OpenAI credentials could not be saved.";
constexpr const char* remove_failed = "OpenAI credentials could not be removed.";
constexpr const char* invalid_file = "OpenAI credential file is invalid.";

// Initializes libcurl once for process lifetime. Intentionally never calls
// curl_global_cleanup(): provider_client.cpp owns its own init/cleanup pair,
// and a second cleanup here could run while the other owner still needs
// libcurl. A leaked init is safe; a premature cleanup is not.
void ensure_curl_initialized() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            throw std::runtime_error("Failed to initialize libcurl");
        }
    });
}

class CurlHandle {
public:
    CurlHandle()
        : handle_(curl_easy_init(), &curl_easy_cleanup) {
        if (!handle_) {
            throw std::runtime_error("Failed to create libcurl handle");
        }
    }

    CURL* get() const noexcept { return handle_.get(); }

private:
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle_;
};

class CurlHeaders {
public:
    ~CurlHeaders() { curl_slist_free_all(headers_); }

    void append(const std::string& header) {
        curl_slist* const appended = curl_slist_append(headers_, header.c_str());
        if (appended == nullptr) {
            throw std::runtime_error("Failed to create HTTP headers");
        }
        headers_ = appended;
    }

    curl_slist* get() const noexcept { return headers_; }

private:
    curl_slist* headers_{};
};

void require_curl(CURLcode result) {
    if (result != CURLE_OK) {
        throw std::runtime_error("OpenAI request failed.");
    }
}

std::size_t receive_body(
    char* data,
    std::size_t size,
    std::size_t count,
    void* user) {
    const std::size_t bytes =
        size != 0 && count > std::numeric_limits<std::size_t>::max() / size
        ? std::numeric_limits<std::size_t>::max()
        : size * count;
    auto& body = *static_cast<std::string*>(user);
    if (bytes > max_auth_body_size || max_auth_body_size - body.size() < bytes) {
        return 0;
    }
    body.append(data, bytes);
    return bytes;
}

OpenAiOAuthHttpResponse production_post(const OpenAiOAuthHttpRequest& request) {
    ensure_curl_initialized();
    if (request.timeout <= std::chrono::milliseconds{0}) {
        throw std::runtime_error(login_timed_out);
    }

    CurlHandle curl;
    CurlHeaders headers;
    headers.append("Content-Type: " + request.content_type);
    headers.append("Accept: application/json");

    std::string body;
    const long timeout_ms = static_cast<long>(
        std::min<std::chrono::milliseconds::rep>(
            request.timeout.count(), std::numeric_limits<long>::max()));
    const long connect_ms = std::min(timeout_ms, 10'000L);

    require_curl(curl_easy_setopt(curl.get(), CURLOPT_URL, request.url.c_str()));
    require_curl(curl_easy_setopt(curl.get(), CURLOPT_POST, 1L));
    require_curl(curl_easy_setopt(
        curl.get(), CURLOPT_POSTFIELDS, request.body.c_str()));
    require_curl(curl_easy_setopt(
        curl.get(),
        CURLOPT_POSTFIELDSIZE_LARGE,
        static_cast<curl_off_t>(request.body.size())));
    require_curl(curl_easy_setopt(
        curl.get(), CURLOPT_WRITEFUNCTION, receive_body));
    require_curl(curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &body));
    require_curl(curl_easy_setopt(
        curl.get(), CURLOPT_HTTPHEADER, headers.get()));
    require_curl(curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, timeout_ms));
    require_curl(curl_easy_setopt(
        curl.get(), CURLOPT_CONNECTTIMEOUT_MS, connect_ms));
    require_curl(curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L));
    require_curl(curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L));
    require_curl(curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L));
    require_curl(curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L));

    const CURLcode result = curl_easy_perform(curl.get());
    if (result != CURLE_OK) {
        throw std::runtime_error("OpenAI request failed.");
    }

    long status = 0;
    require_curl(curl_easy_getinfo(
        curl.get(), CURLINFO_RESPONSE_CODE, &status));
    return {.status = status, .body = std::move(body)};
}

std::chrono::system_clock::time_point system_now() {
    return std::chrono::system_clock::now();
}

bool is_unreserved(unsigned char value) {
    return (value >= 'a' && value <= 'z')
        || (value >= 'A' && value <= 'Z')
        || (value >= '0' && value <= '9')
        || value == '-' || value == '_'
        || value == '.' || value == '~';
}

std::string form_encode_component(std::string_view value) {
    constexpr char hex[] = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (const unsigned char character : value) {
        if (is_unreserved(character)) {
            encoded.push_back(static_cast<char>(character));
        } else {
            encoded.push_back('%');
            encoded.push_back(hex[character >> 4]);
            encoded.push_back(hex[character & 0x0f]);
        }
    }
    return encoded;
}

std::string form_encode(
    const std::vector<std::pair<std::string_view, std::string_view>>& fields) {
    std::string body;
    for (const auto& [name, value] : fields) {
        if (!body.empty()) body.push_back('&');
        body += form_encode_component(name);
        body.push_back('=');
        body += form_encode_component(value);
    }
    return body;
}

std::int64_t unix_seconds(std::chrono::system_clock::time_point time) {
    return std::chrono::duration_cast<std::chrono::seconds>(
               time.time_since_epoch())
        .count();
}

bool add_seconds(std::int64_t now, double seconds, std::int64_t& result) {
    if (!std::isfinite(seconds) || seconds <= 0) return false;
    if (seconds > static_cast<double>(
            std::numeric_limits<std::int64_t>::max() - now)) {
        return false;
    }
    result = now + static_cast<std::int64_t>(seconds);
    return true;
}

Json parse_json_object(std::string_view text) {
    const Json value = Json::parse(text, nullptr, false);
    if (value.is_discarded() || !value.is_object()) return Json::object();
    return value;
}

std::optional<std::string> nonempty_string(
    const Json& object,
    std::string_view key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) return std::nullopt;
    const std::string& value = found->get_ref<const std::string&>();
    if (value.empty()) return std::nullopt;
    return value;
}

std::optional<double> nonnegative_number(const Json& value) {
    if (value.is_number()) {
        const double number = value.get<double>();
        if (std::isfinite(number) && number >= 0) return number;
        return std::nullopt;
    }
    if (!value.is_string()) return std::nullopt;
    const std::string& text = value.get_ref<const std::string&>();
    if (text.empty()
        || std::isspace(static_cast<unsigned char>(text.front()))) {
        return std::nullopt;
    }
    errno = 0;
    char* end = nullptr;
    const double number = std::strtod(text.c_str(), &end);
    if (end != text.c_str() + text.size()) return std::nullopt;
    if (!std::isfinite(number) || number < 0) return std::nullopt;
    return number;
}

std::optional<std::chrono::milliseconds> parse_interval(const Json& object) {
    const auto found = object.find("interval");
    if (found == object.end()) return std::nullopt;
    const auto seconds = nonnegative_number(*found);
    if (!seconds) return std::nullopt;
    double milliseconds = *seconds * 1000.0;
    if (!std::isfinite(milliseconds)
        || milliseconds > static_cast<double>(
               std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    auto interval = std::chrono::milliseconds{
        static_cast<std::int64_t>(milliseconds)};
    if (interval < min_interval) interval = min_interval;
    return interval;
}

std::string error_code(const Json& object) {
    const auto found = object.find("error");
    if (found == object.end()) return {};
    if (found->is_string()) return found->get<std::string>();
    if (!found->is_object()) return {};
    const auto code = found->find("code");
    if (code == found->end() || !code->is_string()) return {};
    return code->get<std::string>();
}

int base64url_value(unsigned char character) {
    if (character >= 'A' && character <= 'Z') return character - 'A';
    if (character >= 'a' && character <= 'z') return character - 'a' + 26;
    if (character >= '0' && character <= '9') return character - '0' + 52;
    if (character == '-' || character == '+') return 62;
    if (character == '_' || character == '/') return 63;
    return -1;
}

std::optional<std::string> decode_base64url(std::string_view input) {
    std::string decoded;
    decoded.reserve(input.size());
    int value = 0;
    int bits = -8;
    for (const unsigned char character : input) {
        if (character == '=') break;
        const int digit = base64url_value(character);
        if (digit < 0) return std::nullopt;
        value = (value << 6) + digit;
        bits += 6;
        if (bits >= 0) {
            decoded.push_back(static_cast<char>((value >> bits) & 0xff));
            bits -= 8;
        }
    }
    return decoded;
}

std::optional<std::string> account_id_from_access_token(std::string_view token) {
    const std::size_t first = token.find('.');
    if (first == std::string_view::npos) return std::nullopt;
    const std::size_t second = token.find('.', first + 1);
    if (second == std::string_view::npos) return std::nullopt;
    if (token.find('.', second + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    const auto payload = decode_base64url(
        token.substr(first + 1, second - first - 1));
    if (!payload) return std::nullopt;
    const Json json = parse_json_object(*payload);
    const auto auth = json.find("https://api.openai.com/auth");
    if (auth == json.end() || !auth->is_object()) return std::nullopt;
    return nonempty_string(*auth, "chatgpt_account_id");
}

struct ParsedTokens {
    std::string access_token;
    std::string refresh_token;
    std::int64_t expires_at{};
    std::string account_id;
};

std::optional<ParsedTokens> parse_token_response(
    const OpenAiOAuthHttpResponse& response,
    std::chrono::system_clock::time_point now) {
    if (response.status < 200 || response.status >= 300) return std::nullopt;
    const Json object = parse_json_object(response.body);
    const auto access_token = nonempty_string(object, "access_token");
    const auto refresh_token = nonempty_string(object, "refresh_token");
    const auto expires = object.find("expires_in");
    if (!access_token || !refresh_token || expires == object.end()
        || !expires->is_number()) {
        return std::nullopt;
    }
    const double expires_in = expires->get<double>();
    std::int64_t expires_at = 0;
    if (!add_seconds(unix_seconds(now), expires_in, expires_at)) {
        return std::nullopt;
    }
    const auto account_id = account_id_from_access_token(*access_token);
    if (!account_id) return std::nullopt;
    return ParsedTokens{
        .access_token = *access_token,
        .refresh_token = *refresh_token,
        .expires_at = expires_at,
        .account_id = *account_id,
    };
}

bool needs_refresh(
    std::int64_t expires_at_unix,
    std::chrono::system_clock::time_point now) {
    const auto expires_at = std::chrono::system_clock::time_point{
        std::chrono::seconds{expires_at_unix}};
    return expires_at <= now + refresh_margin;
}

std::optional<std::int64_t> integer_number(const Json& value) {
    if (value.is_number_integer()) {
        return value.get<std::int64_t>();
    }
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number
            > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
            return std::nullopt;
        }
        return static_cast<std::int64_t>(number);
    }
    return std::nullopt;
}

} // namespace

OpenAiOAuth::OpenAiOAuth(std::filesystem::path credential_path)
    : OpenAiOAuth(std::move(credential_path), production_post, system_now) {}

OpenAiOAuth::OpenAiOAuth(
    std::filesystem::path credential_path,
    OpenAiOAuthTransport transport,
    OpenAiOAuthClock clock)
    : credential_path_(std::move(credential_path)),
      transport_(std::move(transport)),
      clock_(std::move(clock)) {
    if (credential_path_.empty()) {
        throw std::invalid_argument("OpenAI credential path is empty");
    }
    if (!transport_) {
        throw std::invalid_argument("OpenAI transport is missing");
    }
    if (!clock_) {
        throw std::invalid_argument("OpenAI clock is missing");
    }
    load_unlocked();
}

OpenAiOAuthSnapshot OpenAiOAuth::status() const {
    std::lock_guard lock(mutex_);
    discard_expired_attempt_unlocked();
    return snapshot_unlocked();
}

OpenAiOAuthSnapshot OpenAiOAuth::start() {
    std::lock_guard lock(mutex_);
    discard_expired_attempt_unlocked();
    if (pending_) return snapshot_unlocked();
    if (bundle_) {
        error_ = already_connected;
        return snapshot_unlocked();
    }

    error_.reset();
    const auto deadline = clock_() + network_budget;
    Json body;
    body["client_id"] = std::string(client_id);
    OpenAiOAuthHttpResponse response;
    try {
        response = post_unlocked(
            start_path,
            json_content_type,
            dump_json(body, "OpenAI login request"),
            deadline);
    } catch (const std::runtime_error& error) {
        error_ = std::string(error.what()) == login_timed_out
            ? login_timed_out
            : login_failed;
        return snapshot_unlocked();
    } catch (...) {
        error_ = login_failed;
        return snapshot_unlocked();
    }

    const Json object = parse_json_object(response.body);
    const auto device_auth_id = nonempty_string(object, "device_auth_id");
    const auto user_code = nonempty_string(object, "user_code");
    const auto interval = parse_interval(object);
    if (response.status < 200 || response.status >= 300
        || !device_auth_id || !user_code || !interval) {
        error_ = login_failed;
        return snapshot_unlocked();
    }

    const auto now = clock_();
    pending_ = Attempt{
        .device_auth_id = *device_auth_id,
        .user_code = *user_code,
        .interval = *interval,
        .deadline = now + login_limit,
        .next_poll = now + *interval,
    };
    error_.reset();
    log_info("OpenAI device login started");
    return snapshot_unlocked();
}

OpenAiOAuthSnapshot OpenAiOAuth::poll() {
    std::lock_guard lock(mutex_);
    discard_expired_attempt_unlocked();
    if (!pending_) return snapshot_unlocked();

    const auto now = clock_();
    if (now < pending_->next_poll) return snapshot_unlocked();

    error_.reset();
    const auto deadline = now + network_budget;
    Json body;
    body["device_auth_id"] = pending_->device_auth_id;
    body["user_code"] = pending_->user_code;
    OpenAiOAuthHttpResponse response;
    try {
        response = post_unlocked(
            poll_path,
            json_content_type,
            dump_json(body, "OpenAI login poll"),
            deadline);
    } catch (const std::runtime_error& error) {
        pending_.reset();
        error_ = std::string(error.what()) == login_timed_out
            ? login_timed_out
            : login_failed;
        return snapshot_unlocked();
    } catch (...) {
        pending_.reset();
        error_ = login_failed;
        return snapshot_unlocked();
    }

    const Json object = parse_json_object(response.body);
    const auto authorization_code = nonempty_string(object, "authorization_code");
    const auto code_verifier = nonempty_string(object, "code_verifier");
    if (authorization_code && code_verifier) {
        if (!finish_login_unlocked(
                *authorization_code, *code_verifier, deadline)) {
            return snapshot_unlocked();
        }
        log_info("OpenAI device login succeeded");
        return snapshot_unlocked();
    }
    if (response.status == 403 || response.status == 404) {
        pending_->next_poll = clock_() + pending_->interval;
        return snapshot_unlocked();
    }

    const std::string code = error_code(object);
    if (code == "deviceauth_authorization_pending") {
        pending_->next_poll = clock_() + pending_->interval;
        return snapshot_unlocked();
    }
    if (code == "slow_down") {
        pending_->interval += slow_down_extra;
        pending_->next_poll = clock_() + pending_->interval;
        return snapshot_unlocked();
    }

    pending_.reset();
    error_ = login_failed;
    return snapshot_unlocked();
}

OpenAiOAuthSnapshot OpenAiOAuth::disconnect() {
    std::lock_guard lock(mutex_);
    pending_.reset();
    bundle_.reset();
    error_.reset();
    if (!remove_unlocked()) {
        error_ = remove_failed;
    }
    log_info("OpenAI credentials disconnected");
    return snapshot_unlocked();
}

OpenAiOAuthRequestCredentials OpenAiOAuth::credentials() {
    std::lock_guard lock(mutex_);
    if (!bundle_) {
        throw std::runtime_error(not_signed_in);
    }
    if (needs_refresh(bundle_->expires_at, clock_()) && !refresh_unlocked()) {
        throw std::runtime_error(error_.value_or(renew_failed));
    }
    if (!bundle_) {
        throw std::runtime_error(not_signed_in);
    }
    return {
        .access_token = bundle_->access_token,
        .account_id = bundle_->account_id,
    };
}

OpenAiOAuthSnapshot OpenAiOAuth::snapshot_unlocked() const {
    OpenAiOAuthSnapshot snapshot;
    snapshot.error = error_;
    if (pending_) {
        snapshot.state = OpenAiOAuthState::waiting;
        snapshot.user_code = pending_->user_code;
        snapshot.verification_url = std::string(verification_url);
        snapshot.attempt_expires_at = unix_seconds(pending_->deadline);
        const auto now = clock_();
        auto delay = pending_->next_poll - now;
        if (delay < std::chrono::milliseconds{0}) {
            delay = std::chrono::milliseconds{0};
        }
        snapshot.next_poll_delay_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(delay).count();
        return snapshot;
    }
    if (bundle_) {
        snapshot.state = OpenAiOAuthState::connected;
        return snapshot;
    }
    snapshot.state = OpenAiOAuthState::signed_out;
    return snapshot;
}

void OpenAiOAuth::load_unlocked() {
    bundle_.reset();
    pending_.reset();
    error_.reset();

    std::error_code status_error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(credential_path_, status_error);
    if (status.type() == std::filesystem::file_type::not_found) return;
    if (status_error || !std::filesystem::is_regular_file(status)) {
        error_ = invalid_file;
        log_warn(
            std::string(invalid_file) + " path=" + utf8_path(credential_path_));
        return;
    }

    std::ifstream input(credential_path_, std::ios::binary);
    if (!input) {
        error_ = invalid_file;
        log_warn(
            std::string(invalid_file) + " path=" + utf8_path(credential_path_));
        return;
    }
    const std::string contents{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    if (contents.size() > max_auth_body_size) {
        error_ = invalid_file;
        log_warn(
            std::string(invalid_file) + " path=" + utf8_path(credential_path_));
        return;
    }

    const Json object = parse_json_object(contents);
    const auto access_token = nonempty_string(object, "access_token");
    const auto refresh_token = nonempty_string(object, "refresh_token");
    const auto account_id = nonempty_string(object, "account_id");
    const auto expires = object.find("expires_at");
    if (!access_token || !refresh_token || !account_id
        || expires == object.end()) {
        error_ = invalid_file;
        log_warn(
            std::string(invalid_file) + " path=" + utf8_path(credential_path_));
        return;
    }
    const auto expires_at = integer_number(*expires);
    if (!expires_at) {
        error_ = invalid_file;
        log_warn(
            std::string(invalid_file) + " path=" + utf8_path(credential_path_));
        return;
    }

    bundle_ = Bundle{
        .access_token = *access_token,
        .refresh_token = *refresh_token,
        .expires_at = *expires_at,
        .account_id = *account_id,
    };
}

bool OpenAiOAuth::save_unlocked(const Bundle& bundle) {
    try {
        Json object;
        object["access_token"] = bundle.access_token;
        object["refresh_token"] = bundle.refresh_token;
        object["expires_at"] = bundle.expires_at;
        object["account_id"] = bundle.account_id;
        create_private_file(
            credential_path_,
            dump_json(object, "OpenAI credentials"));
        return true;
    } catch (...) {
        return false;
    }
}

bool OpenAiOAuth::remove_unlocked() {
    std::error_code error;
    std::filesystem::remove(credential_path_, error);
    return !error;
}

void OpenAiOAuth::abandon_unlocked(std::string fallback_error) {
    bundle_.reset();
    pending_.reset();
    if (!remove_unlocked()) {
        error_ = remove_failed;
        return;
    }
    error_ = std::move(fallback_error);
}

void OpenAiOAuth::discard_expired_attempt_unlocked() const {
    if (!pending_) return;
    if (clock_() >= pending_->deadline) {
        pending_.reset();
        error_ = login_expired;
    }
}

OpenAiOAuthHttpResponse OpenAiOAuth::post_unlocked(
    std::string_view path,
    std::string_view content_type,
    std::string body,
    std::chrono::system_clock::time_point deadline) {
    const auto now = clock_();
    if (now >= deadline) {
        throw std::runtime_error(login_timed_out);
    }
    auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now);
    if (remaining < std::chrono::milliseconds{1}) {
        remaining = std::chrono::milliseconds{1};
    }
    return transport_(OpenAiOAuthHttpRequest{
        .url = std::string(auth_origin) + std::string(path),
        .content_type = std::string(content_type),
        .body = std::move(body),
        .timeout = remaining,
    });
}

bool OpenAiOAuth::finish_login_unlocked(
    std::string authorization_code,
    std::string code_verifier,
    std::chrono::system_clock::time_point deadline) {
    const std::string body = form_encode({
        {"grant_type", "authorization_code"},
        {"client_id", client_id},
        {"code", authorization_code},
        {"code_verifier", code_verifier},
        {"redirect_uri", redirect_uri},
    });
    OpenAiOAuthHttpResponse response;
    try {
        response = post_unlocked(token_path, form_content_type, body, deadline);
    } catch (const std::runtime_error& error) {
        pending_.reset();
        error_ = std::string(error.what()) == login_timed_out
            ? login_timed_out
            : login_failed;
        return false;
    } catch (...) {
        pending_.reset();
        error_ = login_failed;
        return false;
    }

    auto parsed = parse_token_response(response, clock_());
    if (!parsed) {
        pending_.reset();
        error_ = login_failed;
        return false;
    }
    Bundle bundle{
        .access_token = std::move(parsed->access_token),
        .refresh_token = std::move(parsed->refresh_token),
        .expires_at = parsed->expires_at,
        .account_id = std::move(parsed->account_id),
    };
    if (!save_unlocked(bundle)) {
        abandon_unlocked(save_failed);
        return false;
    }
    bundle_ = std::move(bundle);
    pending_.reset();
    error_.reset();
    return true;
}

bool OpenAiOAuth::refresh_unlocked() {
    if (!bundle_) return false;
    const auto deadline = clock_() + network_budget;
    const std::string body = form_encode({
        {"grant_type", "refresh_token"},
        {"client_id", client_id},
        {"refresh_token", bundle_->refresh_token},
    });
    OpenAiOAuthHttpResponse response;
    try {
        response = post_unlocked(token_path, form_content_type, body, deadline);
    } catch (...) {
        abandon_unlocked(renew_failed);
        return false;
    }

    auto parsed = parse_token_response(response, clock_());
    if (!parsed) {
        abandon_unlocked(renew_failed);
        return false;
    }
    Bundle bundle{
        .access_token = std::move(parsed->access_token),
        .refresh_token = std::move(parsed->refresh_token),
        .expires_at = parsed->expires_at,
        .account_id = std::move(parsed->account_id),
    };
    if (!save_unlocked(bundle)) {
        abandon_unlocked(save_failed);
        return false;
    }
    bundle_ = std::move(bundle);
    error_.reset();
    log_info("OpenAI credentials refreshed");
    return true;
}

} // namespace cha
