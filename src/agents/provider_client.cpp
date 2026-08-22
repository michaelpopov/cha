#include "agents/provider_client.h"

#include "agents/character.h"
#include "agents/provider_response.h"
#include "agents/responses_api.h"
#include "util/logging.h"
#include "util/json_serialization.h"
#include "util/text.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace cha {

// Owns one reusable easy handle and keeps libcurl's variadic API behind typed calls.
class ProviderClient::CurlEasyHandle {
public:
    CurlEasyHandle()
        : handle_(curl_easy_init(), &curl_easy_cleanup) {
        if (!handle_) {
            throw std::runtime_error("Failed to create libcurl handle");
        }
    }

    void reset() noexcept { curl_easy_reset(handle_.get()); }

    void set(CURLoption option, long value, std::string_view operation) {
        require(curl_easy_setopt(handle_.get(), option, value), operation);
    }

    void set_offset(CURLoption option, curl_off_t value, std::string_view operation) {
        require(curl_easy_setopt(handle_.get(), option, value), operation);
    }

    void set(CURLoption option, const char* value, std::string_view operation) {
        require(curl_easy_setopt(handle_.get(), option, value), operation);
    }

    template<typename T>
    void set(CURLoption option, T* value, std::string_view operation) {
        void* data = const_cast<void*>(static_cast<const void*>(value));
        require(curl_easy_setopt(handle_.get(), option, data), operation);
    }

    void set(CURLoption option, curl_slist* value, std::string_view operation) {
        require(curl_easy_setopt(handle_.get(), option, value), operation);
    }

    void set(CURLoption option, curl_write_callback callback, std::string_view operation) {
        require(curl_easy_setopt(handle_.get(), option, callback), operation);
    }

    void set(CURLoption option, curl_xferinfo_callback callback, std::string_view operation) {
        require(curl_easy_setopt(handle_.get(), option, callback), operation);
    }

    CURLcode perform() { return curl_easy_perform(handle_.get()); }

    long response_code(std::string_view operation) {
        long status = 0;
        require(curl_easy_getinfo(handle_.get(), CURLINFO_RESPONSE_CODE, &status), operation);
        return status;
    }

    std::string content_type(std::string_view operation) {
        char* value = nullptr;
        require(curl_easy_getinfo(handle_.get(), CURLINFO_CONTENT_TYPE, &value), operation);
        return value ? value : "";
    }

private:
    static void require(CURLcode result, std::string_view operation) {
        if (result != CURLE_OK) {
            throw std::runtime_error(
                std::string(operation) + ": " + curl_easy_strerror(result));
        }
    }

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle_;
};

namespace {

using Json = nlohmann::json;

// Initializes libcurl once per process while any provider client may use it.
class CurlGlobal {
public:
    CurlGlobal() {
        const CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (result != CURLE_OK) {
            throw std::runtime_error(
                "Failed to initialize libcurl: "
                + std::string(curl_easy_strerror(result)));
        }
    }

    ~CurlGlobal() {
        curl_global_cleanup();
    }
};

// Releases curl header lists when their owning smart pointer leaves scope.
struct CurlHeadersDeleter {
    void operator()(curl_slist* headers) const {
        curl_slist_free_all(headers);
    }
};

using CurlHeaders = std::unique_ptr<curl_slist, CurlHeadersDeleter>;

constexpr std::size_t max_provider_error_body_size = 4 * 1024;
constexpr std::size_t max_public_provider_error_size = 512;
constexpr std::size_t max_request_id_size = 128;

// Accumulates one HTTP response and hands streaming bytes to its decoder.
struct ResponseContext {
    // Set for streaming generation only; single-response calls keep the whole
    // body instead.
    StreamingResponseDecoder* decoder{};
    std::size_t received_bytes{};
    std::string body;
    long status{};
    std::string request_id;
    std::size_t request_id_priority{std::numeric_limits<std::size_t>::max()};
    // Set by the first received byte. Until then the provider is still
    // thinking, which only the overall timeout bounds: a reasoning model or a
    // non-streaming request legitimately sends nothing for minutes.
    std::optional<std::chrono::steady_clock::time_point> last_activity;
    std::exception_ptr error;
};

struct TransferProgressContext {
    const std::atomic_bool* cancellation;
    ResponseContext* response;
    std::chrono::seconds idle_timeout;
    bool idle_timed_out{};
};

std::string sanitize_log_text(std::string_view value, std::size_t maximum_size) {
    std::string result;
    result.reserve(std::min(value.size(), maximum_size));
    for (const unsigned char character : value) {
        if (result.size() == maximum_size) break;
        if (character >= 0x20 && character != 0x7f) {
            result.push_back(static_cast<char>(character));
        } else if (!result.empty() && result.back() != ' ') {
            result.push_back(' ');
        }
    }
    while (!result.empty() && result.back() == ' ') result.pop_back();
    return result;
}

void begin_http_response(ResponseContext& context, std::string_view line) {
    const std::size_t status_start = line.find(' ');
    if (status_start == std::string_view::npos) return;
    long status{};
    const char* const begin = line.data() + status_start + 1;
    const char* const end = line.data() + line.size();
    const auto parsed = std::from_chars(begin, end, status);
    if (parsed.ec != std::errc{}) return;
    context.status = status;
    if (status >= 200) {
        context.request_id.clear();
        context.request_id_priority = std::numeric_limits<std::size_t>::max();
    }
}

std::size_t receive_header(
    char* data,
    std::size_t size,
    std::size_t count,
    void* persona_data) {
    const std::size_t bytes =
        size != 0 && count > std::numeric_limits<std::size_t>::max() / size
        ? std::numeric_limits<std::size_t>::max()
        : size * count;
    auto& context = *static_cast<ResponseContext*>(persona_data);
    try {
        // Headers deliberately leave last_activity unset: a streaming provider
        // sends them before it starts thinking, and a proxy's 100-continue
        // arrives before the request body is even on the wire.
        const std::string_view line(data, bytes);
        if (line.starts_with("HTTP/")) {
            begin_http_response(context, line);
            return bytes;
        }
        const std::size_t separator = line.find(':');
        if (separator == std::string_view::npos) return bytes;
        const std::string_view name = trim_view(line.substr(0, separator));
        static constexpr std::array<std::string_view, 7> request_id_headers{
            "x-request-id",
            "openai-request-id",
            "request-id",
            "x-goog-request-id",
            "x-amzn-requestid",
            "x-amz-request-id",
            "cf-ray",
        };
        for (std::size_t priority{}; priority < request_id_headers.size(); ++priority) {
            if (priority >= context.request_id_priority
                || !ascii_iequals(name, request_id_headers[priority])) {
                continue;
            }
            const std::string value = sanitize_log_text(
                trim_view(line.substr(separator + 1)),
                max_request_id_size);
            if (!value.empty()) {
                context.request_id = value;
                context.request_id_priority = priority;
            }
            break;
        }
    } catch (...) {
        context.error = std::current_exception();
        return 0;
    }
    return bytes;
}

int transfer_progress(void* persona_data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    auto& progress = *static_cast<TransferProgressContext*>(persona_data);
    if (progress.cancellation->load(std::memory_order_acquire)) return 1;
    const auto& last_activity = progress.response->last_activity;
    if (last_activity
        && std::chrono::steady_clock::now() - *last_activity
            >= progress.idle_timeout) {
        progress.idle_timed_out = true;
        return 1;
    }
    return 0;
}

CurlGlobal& curl_global() {
    static CurlGlobal global;
    return global;
}

std::size_t receive_response(
    char* data,
    std::size_t size,
    std::size_t count,
    void* persona_data) {
    const std::size_t bytes =
        size != 0 && count > std::numeric_limits<std::size_t>::max() / size
        ? std::numeric_limits<std::size_t>::max()
        : size * count;
    auto& context = *static_cast<ResponseContext*>(persona_data);

    try {
        context.last_activity = std::chrono::steady_clock::now();
        if (bytes > std::numeric_limits<std::size_t>::max() - context.received_bytes) {
            context.received_bytes = std::numeric_limits<std::size_t>::max();
        } else {
            context.received_bytes += bytes;
        }
        // An unread status is treated as success so a body still reaches its
        // reader; a real failure status always arrives before its body.
        const bool successful = context.status == 0
            || (context.status >= 200 && context.status < 300);
        if (successful && context.decoder) {
            // A successful stream is model output, so nothing is retained.
            context.decoder->consume({data, bytes});
        } else if (successful) {
            context.body.append(data, bytes);
        } else if (context.body.size() < max_provider_error_body_size) {
            const std::size_t retained = std::min(
                bytes,
                max_provider_error_body_size - context.body.size());
            context.body.append(data, retained);
        }
    } catch (...) {
        context.error = std::current_exception();
        return 0;
    }

    return bytes;
}

std::string sanitize_content_type(std::string_view content_type) {
    std::string result = sanitize_log_text(content_type, 128);
    return result.empty() ? "unknown" : result;
}

double elapsed_milliseconds(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

// Every log line carries the field, so an absent identifier is spelled out.
std::string request_id_field(std::string_view request_id) {
    return " provider_request_id="
        + (request_id.empty() ? "unreported" : std::string(request_id));
}

std::string http_event(
    std::string_view event,
    std::string_view endpoint,
    long status,
    std::string_view content_type,
    std::size_t request_bytes,
    std::size_t response_bytes,
    double duration_ms,
    const GenerationTokenUsage& usage = {},
    std::string_view provider_request_id = {}) {
    const auto token_count = [](const std::optional<std::size_t>& count) {
        return count ? std::to_string(*count) : "unreported";
    };
    return "HTTP " + std::string(event)
        + ": endpoint=" + std::string(endpoint)
        + " status=" + std::to_string(status)
        + " content_type=" + sanitize_content_type(content_type)
        + " request_bytes=" + std::to_string(request_bytes)
        + " response_bytes=" + std::to_string(response_bytes)
        + " duration_ms=" + std::to_string(duration_ms)
        + request_id_field(provider_request_id)
        + " input_tokens=" + token_count(usage.input_tokens)
        + " output_tokens=" + token_count(usage.output_tokens)
        + " cache_read_tokens=" + token_count(usage.cache_read_tokens);
}

std::string streaming_metadata(
    long status,
    std::string_view content_type,
    std::size_t received_bytes) {
    return " (HTTP " + std::to_string(status)
        + ", Content-Type: " + sanitize_content_type(content_type)
        + ", " + std::to_string(received_bytes) + " bytes)";
}

std::string response_error(const std::string& body) {
    try {
        const Json value = Json::parse(body);
        const Json::json_pointer message_pointer("/error/message");
        if (value.contains(message_pointer)
            && value.at(message_pointer).is_string()) {
            return sanitize_log_text(
                value.at(message_pointer).get<std::string>(),
                max_public_provider_error_size);
        }
    } catch (const Json::exception&) {
    }

    if (body.empty()) return "unknown server error";
    return sanitize_log_text(body, max_public_provider_error_size);
}

bool contains_any(
    std::string_view value,
    std::span<const std::string_view> patterns) {
    return std::ranges::any_of(patterns, [value](std::string_view pattern) {
        return value.find(pattern) != std::string_view::npos;
    });
}

std::optional<std::string> recognized_provider_error(
    long status,
    std::string_view text) {
    constexpr std::string_view quota_message =
        "Provider quota or billing limit exceeded.";
    const std::string folded_text = fold_ascii(text);
    static constexpr std::array<std::string_view, 4> quota_patterns{
        "insufficient_quota", "quota exceeded", "quota_exceeded",
        "out of budget",
    };
    if (contains_any(folded_text, quota_patterns)) {
        return std::string(quota_message);
    }

    static constexpr std::array<std::string_view, 6> authentication_patterns{
        "invalid_api_key", "invalid api key", "authentication", "unauthorized",
        "permission denied", "forbidden",
    };
    if (status == 401 || status == 403
        || contains_any(folded_text, authentication_patterns)) {
        return "Provider authentication or permission was rejected.";
    }

    static constexpr std::array<std::string_view, 2> rate_limit_patterns{
        "rate limit", "too many requests",
    };
    if (status == 429 || contains_any(folded_text, rate_limit_patterns)) {
        return "Provider rate limit exceeded.";
    }

    // Rate-limit and authentication errors often link to a billing page, so
    // the bare word only means quota once those stronger signals have missed.
    // Ambiguity resolves toward the retryable classification on purpose.
    if (folded_text.find("billing") != std::string::npos) {
        return std::string(quota_message);
    }

    static constexpr std::array<std::string_view, 6> overflow_patterns{
        "context window", "maximum context length", "prompt is too long",
        "context length exceeded", "exceeds the context window",
        "input token count",
    };
    // An empty 400 body is not evidence about length: a malformed request, an
    // unsupported parameter, or a gateway rejection looks exactly the same.
    if (status == 413
        || contains_any(folded_text, overflow_patterns)
        || (status == 400 && folded_text == "bad request")) {
        return "Prompt exceeds the model's context window.";
    }

    return std::nullopt;
}

std::string classified_http_error(long status, const std::string& body) {
    if (const auto recognized = recognized_provider_error(status, body)) {
        return *recognized;
    }
    const std::string detail = response_error(body);
    if (const auto recognized = recognized_provider_error(status, detail)) {
        return *recognized;
    }

    return "Inference server returned HTTP " + std::to_string(status)
        + ": " + detail;
}

// `body` must be a provider error document, never a decoded stream: matching
// the model's own words would replace a real decoder diagnosis with a
// confident and wrong provider verdict.
GenerationResult classify_success_response_error(
    GenerationResult result,
    long status,
    std::string_view body) {
    if (result.outcome != GenerationOutcome::protocol_error) return result;

    if (const auto recognized = recognized_provider_error(status, result.message)) {
        result.message = *recognized;
    } else if (const auto recognized = recognized_provider_error(status, body)) {
        result.message = *recognized;
    } else {
        result.message = sanitize_log_text(
            result.message,
            max_public_provider_error_size);
    }
    return result;
}

void log_provider_error_detail(
    std::string_view endpoint,
    long status,
    const ResponseContext& response) {
    if (response.body.empty()) return;
    const std::string detail = sanitize_log_text(
        response.body,
        max_provider_error_body_size);
    if (detail.empty()) return;
    log_error(
        "Provider HTTP error details: endpoint=" + std::string(endpoint)
        + " status=" + std::to_string(status)
        + request_id_field(response.request_id)
        + " detail=" + detail);
}

std::string_view role_name(ModelRole role) {
    switch (role) {
    case ModelRole::system: return "system";
    case ModelRole::persona: return "user";
    case ModelRole::assistant: return "assistant";
    }
    throw std::logic_error("Unknown model context role");
}

std::string build_chat_completions_request_body(
    const GenerationRequest& input,
    const ModelBackendConfig& config,
    std::string_view system_prompt) {
    Json messages = Json::array();
    for (const ModelMessage& message :
         project_model_context(input, system_prompt)) {
        messages.push_back({
            {"role", role_name(message.role)},
            {"content", message.content},
        });
    }

    Json body{
        {"model", config.model},
        {"stream", config.stream},
        {"messages", std::move(messages)},
    };
    if (config.temperature) {
        body["temperature"] = *config.temperature;
    }
    if (config.max_tokens) {
        body["max_tokens"] = *config.max_tokens;
    }
    if (config.stream) {
        body["stream_options"] = Json{{"include_usage", true}};
    }
    if (!config.reasoning_effort.empty()) {
        body["reasoning_effort"] = config.reasoning_effort;
    }
    if (!input.run.prompt_cache_key.empty()
        && config.cache_retention != CacheRetention::off
        && is_direct_openai_host(config.host)) {
        body["prompt_cache_key"] = input.run.prompt_cache_key;
    }

    return dump_json(body, "Model request");
}

} // namespace

ModelBackendInfo model_backend_info(const CharacterDefinition& definition) {
    return {
        .character = definition.character,
        .model = definition.provider.config.model,
        .api = provider_endpoint(definition.provider.config),
        .streaming = definition.provider.config.stream,
    };
}

std::string provider_endpoint(const ModelBackendConfig& config) {
    std::string host = config.host;
    if (host.find(':') != std::string::npos && !host.starts_with('[')) {
        host = '[' + host + ']';
    }
    const std::string base_url = std::string(config.https ? "https://" : "http://")
        + host + ':' + std::to_string(config.port) + config.base_path;
    switch (config.api) {
    case ProviderApi::chat_completions:
        return base_url + "/v1/chat/completions";
    case ProviderApi::responses:
        return base_url + "/v1/responses";
    }
    throw std::logic_error("Unknown provider API");
}

ProviderClient::ProviderClient(SharedCharacterDefinition definition)
    : definition_(std::move(definition)) {
    if (!definition_) {
        throw std::invalid_argument("Provider client requires a character definition");
    }
    const CharacterMetadata& character = definition_->character;
    const ModelBackendConfig& config = definition_->provider.config;
    if (character.id.empty() || character.display_name.empty()) {
        throw std::runtime_error(
            "Provider client character ID and display name cannot be empty");
    }
    if (config.model.empty()) {
        throw std::runtime_error("Provider client requires a non-empty configured model");
    }
    api_key_ = config.api_key;

    if (!config.api_key_env.empty()) {
        const char* api_key = std::getenv(config.api_key_env.c_str());
        if (!api_key || *api_key == '\0') {
            throw std::runtime_error(
                "Environment variable '" + config.api_key_env
                + "' configured as api_key_env is not set");
        }
        api_key_ = api_key;
    }

    if (config.mode == Mode::net) {
        (void)curl_global();
        curl_ = std::make_unique<CurlEasyHandle>();
    }
}

ProviderClient::~ProviderClient() = default;

RequestPayload ProviderClient::prepare(const GenerationRequest& input) {
    const ModelBackendConfig& config = definition_->provider.config;
    if (config.mode == Mode::test) {
        return {.bytes = input.run.prompt_text};
    }
    switch (config.api) {
    case ProviderApi::chat_completions:
        return {
            .bytes = build_chat_completions_request_body(
                input,
                config,
                definition_->system_prompt),
        };
    case ProviderApi::responses:
        return {
            .bytes = build_responses_request_body(
                input,
                config,
                definition_->system_prompt),
            .session_id = !input.run.prompt_cache_key.empty()
                    && config.cache_retention != CacheRetention::off
                    && is_direct_openai_host(config.host)
                ? std::optional<std::string>(input.run.prompt_cache_key)
                : std::nullopt,
        };
    }
    throw std::logic_error("Unknown provider API");
}

GenerationResult ProviderClient::perform(
    RequestPayload payload,
    const GenerationDeltaSink& on_delta,
    const std::atomic_bool& cancellation) {
    const ModelBackendConfig& config = definition_->provider.config;
    if (cancellation.load(std::memory_order_acquire)) {
        log_info("HTTP generation skipped because cancellation was already requested");
        return {GenerationOutcome::cancelled, {}};
    }
    if (config.mode == Mode::test) {
        on_delta({
            GenerationDeltaKind::answer,
            std::move(payload.bytes),
        });
        return {GenerationOutcome::completed, {}};
    }

    const std::string& request_body = payload.bytes;
    std::unique_ptr<StreamingResponseDecoder> decoder;
    if (config.stream) {
        switch (config.api) {
        case ProviderApi::chat_completions:
            decoder = std::make_unique<ProviderStreamDecoder>(
                config.reasoning_format, on_delta);
            break;
        case ProviderApi::responses:
            decoder = std::make_unique<ResponsesStreamDecoder>(on_delta);
            break;
        default:
            throw std::logic_error("Unknown provider API");
        }
    }
    ResponseContext response{
        .decoder = decoder.get(),
    };
    TransferProgressContext progress{
        .cancellation = &cancellation,
        .response = &response,
        .idle_timeout = std::chrono::seconds(config.idle_timeout_s),
    };

    curl_->reset();
    const std::string url = provider_endpoint(config);
    const auto started_at = std::chrono::steady_clock::now();
    log_debug(
        "HTTP request started: endpoint=" + url
        + " request_bytes=" + std::to_string(request_body.size()));
    const auto complete = [&response, &url, &request_body, started_at](
                              GenerationResult result,
                              long status,
                              std::string_view content_type) {
        const std::string message = http_event(
            result.outcome == GenerationOutcome::completed
                ? "request completed"
                : result.outcome == GenerationOutcome::cancelled
                ? "request cancelled"
                : "request failed",
            url,
            status,
            content_type,
            request_body.size(),
            response.received_bytes,
            elapsed_milliseconds(started_at),
            result.usage,
            response.request_id);
        if (result.outcome == GenerationOutcome::completed) {
            log_info(message);
        } else if (result.outcome == GenerationOutcome::cancelled) {
            log_info(message);
        } else {
            log_error(message);
        }
        return result;
    };
    curl_->set(CURLOPT_URL, url.c_str(), "Failed to configure request URL");
    curl_->set(CURLOPT_POST, 1L, "Failed to configure POST request");
    curl_->set(CURLOPT_POSTFIELDS, request_body.data(), "Failed to configure request body");
    curl_->set_offset(
        CURLOPT_POSTFIELDSIZE_LARGE,
        static_cast<curl_off_t>(request_body.size()),
        "Failed to configure request body size");
    curl_->set(CURLOPT_WRITEFUNCTION, receive_response, "Failed to configure response callback");
    curl_->set(CURLOPT_WRITEDATA, &response, "Failed to configure response destination");
    curl_->set(CURLOPT_HEADERFUNCTION, receive_header, "Failed to configure response header callback");
    curl_->set(CURLOPT_HEADERDATA, &response, "Failed to configure response header destination");
    curl_->set(CURLOPT_CONNECTTIMEOUT, 10L, "Failed to configure connection timeout");
    curl_->set(
        CURLOPT_TIMEOUT,
        static_cast<long>(config.timeout_s),
        "Failed to configure generation timeout");
    curl_->set(CURLOPT_NOSIGNAL, 1L, "Failed to configure libcurl signals");
    curl_->set(CURLOPT_TCP_KEEPALIVE, 1L, "Failed to configure TCP keepalive");
    // The progress callback owns both cancellation and the idle timeout, so
    // cancellation keeps precedence and idleness means "no bytes at all"
    // rather than libcurl's averaged low-speed window.
    curl_->set(CURLOPT_NOPROGRESS, 0L, "Failed to enable transfer progress");
    curl_->set(CURLOPT_XFERINFOFUNCTION, transfer_progress, "Failed to configure cancellation callback");
    curl_->set(CURLOPT_XFERINFODATA, &progress, "Failed to configure transfer progress state");

    curl_slist* raw_headers = nullptr;
    raw_headers = curl_slist_append(
        raw_headers,
        "Content-Type: application/json");
    raw_headers = curl_slist_append(
        raw_headers,
        config.stream
            ? "Accept: text/event-stream"
            : "Accept: application/json");
    if (!api_key_.empty()) {
        raw_headers = curl_slist_append(
            raw_headers,
            ("Authorization: Bearer " + api_key_).c_str());
    }
    if (payload.session_id) {
        raw_headers = curl_slist_append(
            raw_headers,
            ("session_id: " + *payload.session_id).c_str());
    }
    if (!raw_headers) {
        throw std::runtime_error("Failed to create HTTP headers");
    }
    CurlHeaders headers(raw_headers);
    curl_->set(CURLOPT_HTTPHEADER, headers.get(), "Failed to configure HTTP headers");

    const CURLcode perform_result = curl_->perform();
    if (response.error) {
        log_error(http_event(
            "response processing failed",
            url,
            0,
            "unknown",
            request_body.size(),
            response.received_bytes,
            elapsed_milliseconds(started_at),
            {},
            response.request_id));
        std::rethrow_exception(response.error);
    }
    if (perform_result == CURLE_ABORTED_BY_CALLBACK
        && cancellation.load(std::memory_order_acquire)) {
        return complete({GenerationOutcome::cancelled, {}}, 0, "unknown");
    }
    if (perform_result == CURLE_ABORTED_BY_CALLBACK
        && progress.idle_timed_out) {
        return complete({
            GenerationOutcome::transport_error,
            "HTTP request failed: idle timeout reached",
        }, 0, "unknown");
    }
    if (perform_result != CURLE_OK) {
        return complete({
            GenerationOutcome::transport_error,
            "HTTP request failed: "
                + std::string(curl_easy_strerror(perform_result)),
        }, 0, "unknown");
    }

    const long status = curl_->response_code(
        "Failed to read HTTP status");
    const std::string content_type = curl_->content_type(
        "Failed to read HTTP content type");
    if (status < 200 || status >= 300) {
        log_provider_error_detail(url, status, response);
        return complete({
            GenerationOutcome::protocol_error,
            classified_http_error(status, response.body),
        }, status, content_type);
    }

    if (decoder) {
        StreamDecodeResult decoded = decoder->finish();
        if (decoded.describe_response) {
            decoded.result.message += streaming_metadata(
                status,
                content_type,
                response.received_bytes);
        }
        // The decoder's own message is the only provider text here; a
        // successful stream body is model output and is not retained.
        return complete(
            classify_success_response_error(
                std::move(decoded.result),
                status,
                {}),
            status,
            content_type);
    }

    GenerationResult result;
    switch (config.api) {
    case ProviderApi::chat_completions:
        result = decode_provider_response(
            response.body,
            config.reasoning_format,
            on_delta);
        break;
    case ProviderApi::responses:
        result = decode_responses_response(response.body, on_delta);
        break;
    default:
        throw std::logic_error("Unknown provider API");
    }
    return complete(
        classify_success_response_error(
            std::move(result),
            status,
            response.body),
        status,
        content_type);
}

ModelBackendInfo ProviderClient::info() const {
    return model_backend_info(*definition_);
}

} // namespace cha
