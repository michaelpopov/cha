#include "agents/completion_client.h"

#include "agents/agent.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace cha {

// Owns one reusable easy handle and keeps libcurl's variadic API behind typed calls.
class CompletionClient::CurlEasyHandle {
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

// Initializes libcurl once per process while any completion client may use it.
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

// Accumulates one HTTP response and forwards recognized text to a transport-neutral sink.
struct ResponseContext {
    // Required when streaming is true; model discovery leaves both fields inactive.
    const CompletionDeltaSink* on_delta{};
    bool streaming{};
    bool done{};
    bool received_reasoning{};
    bool received_answer{};
    std::size_t received_bytes{};
    ReasoningFormat reasoning_format{ReasoningFormat::automatic};
    std::string body;
    std::string pending;
    std::string protocol_error;
    std::exception_ptr error;

    bool received_output() const noexcept {
        return received_reasoning || received_answer;
    }
};

int transfer_progress(void* user_data, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    const auto& cancellation = *static_cast<std::atomic_bool*>(user_data);
    return cancellation.load(std::memory_order_acquire) ? 1 : 0;
}

CurlGlobal& curl_global() {
    static CurlGlobal global;
    return global;
}

void normalize_newlines(std::string& text) {
    std::size_t index = 0;
    while ((index = text.find("\r\n", index)) != std::string::npos) {
        text.erase(index, 1);
    }
}

void emit_delta(
    ResponseContext& context,
    CompletionDeltaKind kind,
    std::string text) {
    if (text.empty()) {
        return;
    }
    if (kind == CompletionDeltaKind::reasoning) {
        context.received_reasoning = true;
    } else {
        context.received_answer = true;
    }
    (*context.on_delta)(CompletionDelta{kind, std::move(text)});
}

void process_response_object(
    const Json& object,
    ReasoningFormat format,
    ResponseContext& context) {
    const auto emit_field = [&object, &context](
                                std::string_view field,
                                bool strict) -> bool {
        const auto iterator = object.find(field);
        if (iterator == object.end() || iterator->is_null()) {
            return false;
        }
        if (!iterator->is_string()) {
            if (strict && context.protocol_error.empty()) {
                context.protocol_error =
                    "Reasoning field '" + std::string(field)
                    + "' was not a string or null";
            }
            return false;
        }
        std::string text = iterator->get<std::string>();
        if (text.empty()) {
            return false;
        }
        emit_delta(context, CompletionDeltaKind::reasoning, std::move(text));
        return true;
    };

    switch (format) {
    case ReasoningFormat::automatic:
        if (!emit_field("reasoning_content", false)) {
            (void)emit_field("reasoning", false);
        }
        break;
    case ReasoningFormat::none:
        break;
    case ReasoningFormat::reasoning_content:
        (void)emit_field("reasoning_content", true);
        break;
    case ReasoningFormat::reasoning:
        (void)emit_field("reasoning", true);
        break;
    }

    const auto content = object.find("content");
    if (content != object.end() && content->is_string()) {
        emit_delta(
            context,
            CompletionDeltaKind::answer,
            content->get<std::string>());
    }
}

void process_stream_event(std::string_view event, ResponseContext& context) {
    if (context.done) {
        return;
    }

    std::size_t line_start = 0;

    while (line_start <= event.size()) {
        const std::size_t line_end = event.find('\n', line_start);
        const std::string_view line = event.substr(
            line_start,
            line_end == std::string_view::npos
                ? event.size() - line_start
                : line_end - line_start);

        if (line.starts_with("data:")) {
            std::string_view data = line.substr(5);
            while (!data.empty() && data.front() == ' ') {
                data.remove_prefix(1);
            }

            if (data == "[DONE]") {
                context.done = true;
                return;
            }
            if (!data.empty()) {
                try {
                    const Json value = Json::parse(data);
                    const Json::json_pointer choices_pointer("/choices");
                    const Json::json_pointer delta_pointer(
                        "/choices/0/delta");
                    if (!value.contains(choices_pointer)
                        || !value.at(choices_pointer).is_array()) {
                        if (context.protocol_error.empty()) {
                            context.protocol_error =
                                "Streaming event did not contain a choices array";
                        }
                    } else if (value.contains(delta_pointer)
                        && value.at(delta_pointer).is_object()) {
                        process_response_object(
                            value.at(delta_pointer),
                            context.reasoning_format,
                            context);
                    }
                } catch (const Json::parse_error&) {
                    if (context.protocol_error.empty()) {
                        context.protocol_error =
                            "Streaming event contained malformed JSON";
                    }
                }
            }
        }

        if (line_end == std::string_view::npos) {
            break;
        }
        line_start = line_end + 1;
    }
}

void process_pending_events(ResponseContext& context) {
    normalize_newlines(context.pending);

    std::size_t event_end = 0;
    while ((event_end = context.pending.find("\n\n")) != std::string::npos) {
        const std::string event = context.pending.substr(0, event_end);
        context.pending.erase(0, event_end + 2);
        process_stream_event(event, context);
        if (context.done) {
            context.pending.clear();
            return;
        }
    }
}

std::size_t receive_response(
    char* data,
    std::size_t size,
    std::size_t count,
    void* user_data) {
    const std::size_t bytes =
        size != 0 && count > std::numeric_limits<std::size_t>::max() / size
        ? std::numeric_limits<std::size_t>::max()
        : size * count;
    auto& context = *static_cast<ResponseContext*>(user_data);

    try {
        if (bytes > std::numeric_limits<std::size_t>::max() - context.received_bytes) {
            context.received_bytes = std::numeric_limits<std::size_t>::max();
        } else {
            context.received_bytes += bytes;
        }
        if (!context.streaming) {
            context.body.append(data, bytes);
        } else if (context.body.size() < max_streaming_error_body_size) {
            const std::size_t retained = std::min(
                bytes,
                max_streaming_error_body_size - context.body.size());
            context.body.append(data, retained);
        }
        if (context.streaming && !context.done) {
            context.pending.append(data, bytes);
            process_pending_events(context);
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

std::string_view role_name(AgentRole role) {
    switch (role) {
    case AgentRole::system: return "system";
    case AgentRole::user: return "user";
    case AgentRole::assistant: return "assistant";
    }
    throw std::logic_error("Unknown agent context role");
}

std::string build_request_body(
    const TranscriptReadView& transcript,
    const Config& config,
    std::string_view system_prompt) {
    Json messages = Json::array();
    for (const AgentMessage& message : project_agent_context(
             transcript.entries(),
             transcript.open_entry_id(),
             system_prompt,
             config.id)) {
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
    if (!config.reasoning_effort.empty()) {
        body["reasoning_effort"] = config.reasoning_effort;
    }

    try {
        return body.dump();
    } catch (const Json::type_error& error) {
        if (error.id == 316) {
            throw std::runtime_error(
                "Completion request contains invalid UTF-8");
        }
        throw;
    }
}

} // namespace

CompletionClient::CompletionClient(AgentDefinition definition)
    : config_(std::move(definition.config)),
      system_prompt_(std::move(definition.system_prompt)) {
    if (config_.id.empty() || config_.name.empty()) {
        throw std::runtime_error(
            "Completion client agent ID and display name cannot be empty");
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

CompletionClient::~CompletionClient() = default;

void CompletionClient::discover_model() {
    ResponseContext response{.streaming = false};

    curl_->reset();
    const std::string url = models_endpoint();
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
}

RequestPayload CompletionClient::prepare(
    const CompletionRequest& request,
    const TranscriptReadView& transcript) {
    if (config_.mode == Mode::test) {
        return {.bytes = request.prompt.text};
    }
    return {
        .bytes = build_request_body(
            transcript,
            config_,
            system_prompt_),
    };
}

CompletionResult CompletionClient::perform(
    RequestPayload payload,
    const CompletionDeltaSink& on_delta,
    const std::atomic_bool& cancellation) {
    if (cancellation.load(std::memory_order_acquire)) {
        return {CompletionOutcome::cancelled, {}};
    }
    if (config_.mode == Mode::test) {
        on_delta({
            CompletionDeltaKind::answer,
            std::move(payload.bytes),
        });
        return {CompletionOutcome::completed, {}};
    }

    const std::string& request_body = payload.bytes;
    ResponseContext response{
        .on_delta = &on_delta,
        .streaming = config_.stream,
        .reasoning_format = config_.reasoning_format,
    };

    curl_->reset();
    const std::string url = endpoint();
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
        std::rethrow_exception(response.error);
    }
    if (perform_result == CURLE_ABORTED_BY_CALLBACK
        && cancellation.load(std::memory_order_acquire)) {
        return {CompletionOutcome::cancelled, {}};
    }
    if (perform_result != CURLE_OK) {
        return {
            CompletionOutcome::transport_error,
            "HTTP request failed: "
                + std::string(curl_easy_strerror(perform_result)),
        };
    }

    const long status = curl_->response_code(
        "Failed to read HTTP status");
    const std::string content_type = curl_->content_type(
        "Failed to read HTTP content type");
    if (status < 200 || status >= 300) {
        return {
            CompletionOutcome::protocol_error,
            "Inference server returned HTTP " + std::to_string(status)
                + ": " + response_error(response.body),
        };
    }

    if (config_.stream) {
        if (!response.pending.empty()) {
            process_stream_event(response.pending, response);
            response.pending.clear();
        }
        if (!response.protocol_error.empty()) {
            return {
                CompletionOutcome::protocol_error,
                response.protocol_error
                    + streaming_metadata(
                        status,
                        content_type,
                        response.received_bytes),
            };
        }
        if (!response.done) {
            return {
                CompletionOutcome::protocol_error,
                (response.received_output()
                    ? "Streaming response ended before [DONE]"
                    : "Streaming response was not valid SSE")
                    + streaming_metadata(
                        status,
                        content_type,
                        response.received_bytes),
            };
        }
        if (!response.received_answer) {
            return {
                CompletionOutcome::protocol_error,
                "Streaming response completed without answer content",
            };
        }
        return {CompletionOutcome::completed, {}};
    }

    Json value;
    try {
        value = Json::parse(response.body);
    } catch (const Json::exception& error) {
        return {
            CompletionOutcome::protocol_error,
            "Inference server returned invalid JSON: "
                + std::string(error.what()),
        };
    }
    const Json::json_pointer message_pointer(
        "/choices/0/message");
    if (!value.contains(message_pointer)
        || !value.at(message_pointer).is_object()) {
        return {
            CompletionOutcome::protocol_error,
            "Response did not contain choices[0].message",
        };
    }
    process_response_object(
        value.at(message_pointer),
        config_.reasoning_format,
        response);
    if (!response.protocol_error.empty()) {
        return {
            CompletionOutcome::protocol_error,
            response.protocol_error,
        };
    }
    if (!response.received_answer) {
        return {
            CompletionOutcome::protocol_error,
            "Response completed without answer content",
        };
    }
    return {CompletionOutcome::completed, {}};
}

AgentRuntimeInfo CompletionClient::info() const {
    return {
        .persona = {
            .id = config_.id,
            .name = config_.name,
        },
        .model = config_.model,
        .api = endpoint(),
        .streaming = config_.stream,
    };
}

std::string CompletionClient::base_url() const {
    std::string host = config_.host;
    if (host.find(':') != std::string::npos && !host.starts_with('[')) {
        host = '[' + host + ']';
    }
    return std::string(config_.https ? "https://" : "http://")
        + host + ':' + std::to_string(config_.port);
}

std::string CompletionClient::endpoint() const {
    return base_url() + "/v1/chat/completions";
}

std::string CompletionClient::models_endpoint() const {
    return base_url() + "/v1/models";
}

} // namespace cha
