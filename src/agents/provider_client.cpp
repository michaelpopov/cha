#include "agents/provider_client.h"

#include "agents/character.h"
#include "agents/provider_response.h"
#include "util/logging.h"
#include "util/json_serialization.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
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

constexpr std::size_t max_streaming_error_body_size = 64 * 1024;

// Accumulates one HTTP response and hands streaming bytes to its decoder.
struct ResponseContext {
    // Set for streaming generation only; discovery and single-response calls
    // keep the whole body instead.
    ProviderStreamDecoder* decoder{};
    std::size_t received_bytes{};
    std::string body;
    std::exception_ptr error;
};

int transfer_progress(void* persona_data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto& cancellation = *static_cast<std::atomic_bool*>(persona_data);
    return cancellation.load(std::memory_order_acquire) ? 1 : 0;
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
        if (bytes > std::numeric_limits<std::size_t>::max() - context.received_bytes) {
            context.received_bytes = std::numeric_limits<std::size_t>::max();
        } else {
            context.received_bytes += bytes;
        }
        if (!context.decoder) {
            context.body.append(data, bytes);
        } else {
            if (context.body.size() < max_streaming_error_body_size) {
                const std::size_t retained = std::min(
                    bytes,
                    max_streaming_error_body_size - context.body.size());
                context.body.append(data, retained);
            }
            context.decoder->consume({data, bytes});
        }
    } catch (...) {
        context.error = std::current_exception();
        return 0;
    }

    return bytes;
}

std::string sanitize_content_type(std::string_view content_type) {
    constexpr std::size_t maximum_size = 128;
    std::string result;
    result.reserve(std::min(content_type.size(), maximum_size));
    for (const unsigned char character : content_type) {
        if (result.size() == maximum_size) {
            break;
        }
        if (character >= 0x20 && character != 0x7f) {
            result.push_back(static_cast<char>(character));
        }
    }
    return result.empty() ? "unknown" : result;
}

double elapsed_milliseconds(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

std::string http_event(
    std::string_view event,
    std::string_view endpoint,
    long status,
    std::string_view content_type,
    std::size_t request_bytes,
    std::size_t response_bytes,
    double duration_ms) {
    return "HTTP " + std::string(event)
        + ": endpoint=" + std::string(endpoint)
        + " status=" + std::to_string(status)
        + " content_type=" + sanitize_content_type(content_type)
        + " request_bytes=" + std::to_string(request_bytes)
        + " response_bytes=" + std::to_string(response_bytes)
        + " duration_ms=" + std::to_string(duration_ms);
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
            return value.at(message_pointer).get<std::string>();
        }
    } catch (const Json::exception&) {
    }

    return body.empty() ? "unknown server error" : body;
}

std::string_view role_name(ModelRole role) {
    switch (role) {
    case ModelRole::system: return "system";
    case ModelRole::persona: return "user";
    case ModelRole::assistant: return "assistant";
    }
    throw std::logic_error("Unknown model context role");
}

std::string build_request_body(
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
    body["temperature"] = config.temperature;
    if (!config.reasoning_effort.empty()) {
        body["reasoning_effort"] = config.reasoning_effort;
    }

    return dump_json(body, "Model request");
}

} // namespace

ProviderClient::ProviderClient(CharacterDefinition definition)
    : character_(std::move(definition.character)),
      config_(std::move(definition.backend)),
      system_prompt_(std::move(definition.system_prompt)) {
    if (character_.id.empty() || character_.display_name.empty()) {
        throw std::runtime_error(
            "Provider client character ID and display name cannot be empty");
    }
    api_key_ = config_.api_key;

    if (!config_.api_key_env.empty()) {
        const char* api_key = std::getenv(config_.api_key_env.c_str());
        if (!api_key || *api_key == '\0') {
            throw std::runtime_error(
                "Environment variable '" + config_.api_key_env
                + "' configured as api_key_env is not set");
        }
        api_key_ = api_key;
    }

    if (config_.mode == Mode::net) {
        (void)curl_global();
        curl_ = std::make_unique<CurlEasyHandle>();
        if (config_.model.empty()) {
            discover_model();
        }
    }
}

ProviderClient::~ProviderClient() = default;

void ProviderClient::discover_model() {
    ResponseContext response;
    const std::string url = models_endpoint();
    const auto started_at = std::chrono::steady_clock::now();
    log_info("Model discovery started: endpoint=" + url);
    try {
        curl_->reset();
        curl_->set(CURLOPT_URL, url.c_str(), "Failed to configure models request URL");
        curl_->set(CURLOPT_HTTPGET, 1L, "Failed to configure models request");
        curl_->set(CURLOPT_WRITEFUNCTION, receive_response, "Failed to configure models response callback");
        curl_->set(CURLOPT_WRITEDATA, &response, "Failed to configure models response destination");
        curl_->set(CURLOPT_CONNECTTIMEOUT, 10L, "Failed to configure models connection timeout");
        // Model discovery is a small metadata request and must not block startup indefinitely.
        curl_->set(CURLOPT_TIMEOUT, 10L, "Failed to configure models request timeout");
        curl_->set(CURLOPT_NOSIGNAL, 1L, "Failed to configure libcurl signals");

        curl_slist* raw_headers = nullptr;
        raw_headers = curl_slist_append(raw_headers, "Accept: application/json");
        if (!api_key_.empty()) {
            raw_headers = curl_slist_append(
                raw_headers,
                ("Authorization: Bearer " + api_key_).c_str());
        }
        if (!raw_headers) {
            throw std::runtime_error("Failed to create models request headers");
        }
        CurlHeaders headers(raw_headers);
        curl_->set(CURLOPT_HTTPHEADER, headers.get(), "Failed to configure models request headers");

        const CURLcode perform_result = curl_->perform();
        if (response.error) {
            std::rethrow_exception(response.error);
        }
        if (perform_result != CURLE_OK) {
            throw std::runtime_error(
                "Models request failed: "
                + std::string(curl_easy_strerror(perform_result)));
        }

        const long status = curl_->response_code(
            "Failed to read models response status");
        const std::string content_type = curl_->content_type(
            "Failed to read models response content type");
        if (status < 200 || status >= 300) {
            throw std::runtime_error(
                "Models request returned HTTP " + std::to_string(status)
                + ": " + response_error(response.body));
        }

        try {
            const Json value = Json::parse(response.body);
            const Json::json_pointer model_pointer("/data/0/id");
            if (!value.contains(model_pointer)
                || !value.at(model_pointer).is_string()) {
                throw std::runtime_error(
                    "Models response did not contain data[0].id");
            }
            config_.model = value.at(model_pointer).get<std::string>();
        } catch (const Json::exception& error) {
            throw std::runtime_error(
                "Failed to parse models response: " + std::string(error.what()));
        }
        log_info(
            "Model discovery completed: endpoint=" + url
            + " model=" + config_.model
            + " status=" + std::to_string(status)
            + " content_type=" + sanitize_content_type(content_type)
            + " response_bytes=" + std::to_string(response.received_bytes)
            + " duration_ms=" + std::to_string(elapsed_milliseconds(started_at)));
    } catch (const std::exception&) {
        log_error(
            "Model discovery failed: endpoint=" + url
            + " response_bytes=" + std::to_string(response.received_bytes)
            + " duration_ms=" + std::to_string(elapsed_milliseconds(started_at)));
        throw;
    }
}

RequestPayload ProviderClient::prepare(const GenerationRequest& input) {
    if (config_.mode == Mode::test) {
        return {.bytes = input.run.prompt_text};
    }
    return {
        .bytes = build_request_body(
            input,
            config_,
            system_prompt_),
    };
}

GenerationResult ProviderClient::perform(
    RequestPayload payload,
    const GenerationDeltaSink& on_delta,
    const std::atomic_bool& cancellation) {
    if (cancellation.load(std::memory_order_acquire)) {
        log_info("HTTP generation skipped because cancellation was already requested");
        return {GenerationOutcome::cancelled, {}};
    }
    if (config_.mode == Mode::test) {
        on_delta({
            GenerationDeltaKind::answer,
            std::move(payload.bytes),
        });
        return {GenerationOutcome::completed, {}};
    }

    const std::string& request_body = payload.bytes;
    std::optional<ProviderStreamDecoder> decoder;
    if (config_.stream) {
        decoder.emplace(config_.reasoning_format, on_delta);
    }
    ResponseContext response{
        .decoder = decoder ? &*decoder : nullptr,
    };

    curl_->reset();
    const std::string url = endpoint();
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
            elapsed_milliseconds(started_at));
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
    curl_->set(CURLOPT_CONNECTTIMEOUT, 10L, "Failed to configure connection timeout");
    curl_->set(CURLOPT_NOSIGNAL, 1L, "Failed to configure libcurl signals");
    curl_->set(CURLOPT_TCP_KEEPALIVE, 1L, "Failed to configure TCP keepalive");
    // Generation duration is intentionally unbounded; callers cancel it through this flag.
    curl_->set(CURLOPT_NOPROGRESS, 0L, "Failed to enable transfer progress");
    curl_->set(CURLOPT_XFERINFOFUNCTION, transfer_progress, "Failed to configure cancellation callback");
    curl_->set(CURLOPT_XFERINFODATA, &cancellation, "Failed to configure cancellation state");

    curl_slist* raw_headers = nullptr;
    raw_headers = curl_slist_append(
        raw_headers,
        "Content-Type: application/json");
    raw_headers = curl_slist_append(
        raw_headers,
        config_.stream
            ? "Accept: text/event-stream"
            : "Accept: application/json");
    if (!api_key_.empty()) {
        raw_headers = curl_slist_append(
            raw_headers,
            ("Authorization: Bearer " + api_key_).c_str());
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
            elapsed_milliseconds(started_at)));
        std::rethrow_exception(response.error);
    }
    if (perform_result == CURLE_ABORTED_BY_CALLBACK
        && cancellation.load(std::memory_order_acquire)) {
        return complete({GenerationOutcome::cancelled, {}}, 0, "unknown");
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
        return complete({
            GenerationOutcome::protocol_error,
            "Inference server returned HTTP " + std::to_string(status)
                + ": " + response_error(response.body),
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
        return complete(std::move(decoded.result), status, content_type);
    }

    return complete(
        decode_provider_response(
            response.body,
            config_.reasoning_format,
            on_delta),
        status,
        content_type);
}

ModelBackendInfo ProviderClient::info() const {
    return {
        .character = character_,
        .model = config_.model,
        .api = endpoint(),
        .streaming = config_.stream,
    };
}

std::string ProviderClient::base_url() const {
    std::string host = config_.host;
    if (host.find(':') != std::string::npos && !host.starts_with('[')) {
        host = '[' + host + ']';
    }
    return std::string(config_.https ? "https://" : "http://")
        + host + ':' + std::to_string(config_.port);
}

std::string ProviderClient::endpoint() const {
    return base_url() + "/v1/chat/completions";
}

std::string ProviderClient::models_endpoint() const {
    return base_url() + "/v1/models";
}

} // namespace cha
