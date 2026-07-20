#include "agent.h"

#include "conversation.h"
#include "agent_context.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace cha {
namespace {

using Json = nlohmann::json;

// Initializes libcurl once per process while any network-backed Agent may use it.
class CurlGlobal {
public:
    CurlGlobal() {
        const CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (result != CURLE_OK) {
            throw std::runtime_error("Failed to initialize libcurl: " + std::string(curl_easy_strerror(result)));
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

// Accumulates HTTP response state and routes streaming updates into the active conversation.
struct ResponseContext {
    AgentEventChannel* events{};
    RequestId request_id{};
    bool streaming{};
    bool done{};
    bool received_content{};
    std::string body;
    std::string pending;
    std::string protocol_error;
    std::exception_ptr error;
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

void process_stream_event(std::string_view event, ResponseContext& context) {
    if (context.done) {
        return;
    }

    std::size_t line_start = 0;

    while (line_start <= event.size()) {
        const std::size_t line_end = event.find('\n', line_start);
        const std::string_view line = event.substr(
            line_start,
            line_end == std::string_view::npos ? event.size() - line_start : line_end - line_start
        );

        if (line.starts_with("data:")) {
            std::string_view data = line.substr(5);
            while (!data.empty() && data.front() == ' ') {
                data.remove_prefix(1);
            }

            if (data == "[DONE]") {
                context.done = true;
                return;
            } else if (!data.empty()) {
                try {
                    const Json value = Json::parse(data);
                    const Json::json_pointer content_pointer("/choices/0/delta/content");
                    const Json::json_pointer choices_pointer("/choices");

                    if (value.contains(content_pointer) && value.at(content_pointer).is_string()) {
                        const std::string content = value.at(content_pointer).get<std::string>();
                        if (!content.empty()) {
                            context.received_content = true;
                            context.events->push(AgentDelta{context.request_id, content});
                        }
                    } else if (!value.contains(choices_pointer)
                        || !value.at(choices_pointer).is_array()) {
                        if (context.protocol_error.empty()) {
                            context.protocol_error = "Streaming event did not contain a choices array";
                        }
                    }
                } catch (const Json::parse_error&) {
                    if (context.protocol_error.empty()) {
                        context.protocol_error = "Streaming event contained malformed JSON";
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

std::size_t receive_response(char* data, std::size_t size, std::size_t count, void* user_data) {
    const std::size_t bytes = size * count;
    auto& context = *static_cast<ResponseContext*>(user_data);

    try {
        if (!context.streaming) {
            context.body.append(data, bytes);
        } else if (context.body.size() < max_streaming_error_body_size) {
            const std::size_t retained = std::min(bytes, max_streaming_error_body_size - context.body.size());
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

std::string response_error(const std::string& body) {
    try {
        const Json value = Json::parse(body);
        const Json::json_pointer message_pointer("/error/message");
        if (value.contains(message_pointer) && value.at(message_pointer).is_string()) {
            return value.at(message_pointer).get<std::string>();
        }
    } catch (const Json::exception&) {
    }

    return body.empty() ? "unknown server error" : body;
}

void require_curl(CURLcode result, std::string_view operation) {
    if (result != CURLE_OK) {
        throw std::runtime_error(std::string(operation) + ": " + curl_easy_strerror(result));
    }
}

} // namespace

Agent::Agent(
    AgentDefinition definition,
    std::atomic_bool& cancellation)
    : config_(std::move(definition.config)),
      cancellation_(cancellation),
      system_prompt_(std::move(definition.system_prompt)) {
    if (config_.id.empty() || config_.name.empty()) {
        throw std::runtime_error("Agent ID and display name cannot be empty");
    }
    api_key_ = config_.api_key;

    if (!config_.api_key_env.empty()) {
        const char* api_key = std::getenv(config_.api_key_env.c_str());
        if (!api_key || *api_key == '\0') {
            throw std::runtime_error(
                "Environment variable '" + config_.api_key_env + "' configured as api_key_env is not set"
            );
        }
        api_key_ = api_key;
    }

    if (config_.mode == Mode::net) {
        (void)curl_global();
        curl_.reset(curl_easy_init());
        if (!curl_) {
            throw std::runtime_error("Failed to create libcurl handle");
        }
        if (config_.model.empty()) {
            discover_model();
        }
    }
}

Agent::~Agent() {
    stop();
}

void Agent::discover_model() {
    ResponseContext response{.streaming = false};

    curl_easy_reset(curl_.get());
    const std::string url = models_endpoint();
    require_curl(curl_easy_setopt(curl_.get(), CURLOPT_URL, url.c_str()), "Failed to configure models request URL");
    require_curl(curl_easy_setopt(curl_.get(), CURLOPT_HTTPGET, 1L), "Failed to configure models request");
    require_curl(curl_easy_setopt(curl_.get(), CURLOPT_WRITEFUNCTION, receive_response), "Failed to configure models response callback");
    require_curl(curl_easy_setopt(curl_.get(), CURLOPT_WRITEDATA, &response), "Failed to configure models response destination");
    require_curl(curl_easy_setopt(curl_.get(), CURLOPT_CONNECTTIMEOUT, 10L), "Failed to configure models connection timeout");
    // Model discovery is a small metadata request and must not block startup indefinitely.
    require_curl(curl_easy_setopt(curl_.get(), CURLOPT_TIMEOUT, 10L), "Failed to configure models request timeout");
    require_curl(curl_easy_setopt(curl_.get(), CURLOPT_NOSIGNAL, 1L), "Failed to configure libcurl signals");

    curl_slist* raw_headers = nullptr;
    raw_headers = curl_slist_append(raw_headers, "Accept: application/json");
    if (!api_key_.empty()) {
        raw_headers = curl_slist_append(raw_headers, ("Authorization: Bearer " + api_key_).c_str());
    }
    if (!raw_headers) {
        throw std::runtime_error("Failed to create models request headers");
    }
    CurlHeaders headers(raw_headers);
    require_curl(curl_easy_setopt(curl_.get(), CURLOPT_HTTPHEADER, headers.get()), "Failed to configure models request headers");

    const CURLcode perform_result = curl_easy_perform(curl_.get());
    if (response.error) {
        std::rethrow_exception(response.error);
    }
    if (perform_result != CURLE_OK) {
        throw std::runtime_error("Models request failed: " + std::string(curl_easy_strerror(perform_result)));
    }

    long status = 0;
    require_curl(curl_easy_getinfo(curl_.get(), CURLINFO_RESPONSE_CODE, &status), "Failed to read models response status");
    if (status < 200 || status >= 300) {
        throw std::runtime_error("Models request returned HTTP " + std::to_string(status) + ": " + response_error(response.body));
    }

    try {
        const Json value = Json::parse(response.body);
        const Json::json_pointer model_pointer("/data/0/id");
        if (!value.contains(model_pointer) || !value.at(model_pointer).is_string()) {
            throw std::runtime_error("Models response did not contain data[0].id");
        }
        config_.model = value.at(model_pointer).get<std::string>();
    } catch (const Json::exception& error) {
        throw std::runtime_error("Failed to parse models response: " + std::string(error.what()));
    }
}

void Agent::start(CompletionRequestChannel& requests, AgentEventChannel& events) {
    const std::scoped_lock lock(lifecycle_mutex_);
    if (state_ == State::running) {
        throw std::logic_error("Agent is already running");
    }
    if (state_ == State::stopped) {
        throw std::logic_error("Agent cannot be restarted after it is stopped");
    }

    input_ = &requests;
    try {
        thread_ = std::thread(&Agent::dialog, this, std::ref(requests), std::ref(events));
        state_ = State::running;
    } catch (...) {
        input_ = nullptr;
        throw;
    }
}

void Agent::stop() {
    const std::scoped_lock lock(lifecycle_mutex_);
    if (state_ == State::stopped) {
        return;
    }
    if (state_ == State::running) {
        // Borrow the caller-owned flag to interrupt CURL, then restore it for
        // another agent or coordinator that may reuse the same cancellation state.
        const bool was_cancelled = cancellation_.exchange(true, std::memory_order_acq_rel);
        input_->close();
        thread_.join();
        cancellation_.store(was_cancelled, std::memory_order_release);
    }
    input_ = nullptr;
    curl_.reset();
    state_ = State::stopped;
}

void Agent::dialog(CompletionRequestChannel& requests, AgentEventChannel& events) {
    while (true) {
        const std::optional<CompletionRequest> request = requests.get();
        if (!request) {
            break;
        }
        try {
            validate_completion_request(*request, config_.id);
            if (cancellation_.load(std::memory_order_acquire)) {
                events.push(AgentCancelled{request->request_id});
                continue;
            }
            if (config_.mode == Mode::test) {
                events.push(AgentDelta{request->request_id, request->prompt.text});
                events.push(AgentDelta{request->request_id, request->prompt.text});
                events.push(AgentCompleted{request->request_id});
                continue;
            }

            const CompletionResult result = complete(*request, events);
            if (result.outcome == CompletionOutcome::cancelled) {
                events.push(AgentCancelled{request->request_id});
            } else if (result.outcome == CompletionOutcome::completed) {
                events.push(AgentCompleted{request->request_id});
            } else {
                events.push(AgentFailed{request->request_id, result.message});
            }
        } catch (const std::exception& error) {
            events.push(AgentFailed{request->request_id, error.what()});
        }
    }
}

AgentInfo Agent::info() const {
    return {
        .id = config_.id,
        .name = config_.name,
        .model = config_.model,
        .api = endpoint(),
        .streaming = config_.stream,
    };
}

Agent::CompletionResult Agent::complete(const CompletionRequest& request, AgentEventChannel& events) {
    if (!curl_) {
        throw std::runtime_error("Agent HTTP client is closed");
    }

    Json body;
    body["model"] = config_.model;
    body["stream"] = config_.stream;
    body["messages"] = Json::array();
    ConversationSnapshot snapshot{
        .entries = request.history,
    };
    snapshot.entries.push_back(request.prompt);
    for (const AgentMessage& message : build_agent_context(snapshot, system_prompt_, config_.id)) {
        Json protocol_message{
            {"role", message.role},
            {"content", message.content},
        };
        body["messages"].push_back(std::move(protocol_message));
    }
    if (config_.temperature) {
        body["temperature"] = *config_.temperature;
    }
    if (!config_.reasoning_effort.empty()) {
        body["reasoning_effort"] = config_.reasoning_effort;
    }

    const std::string request_body = body.dump();
    ResponseContext response{
        .events = &events,
        .request_id = request.request_id,
        .streaming = config_.stream,
    };

    curl_easy_reset(curl_.get());
    const std::string url = endpoint();
    require_curl(curl_easy_setopt(curl_.get(), CURLOPT_URL, url.c_str()), "Failed to configure request URL");
    require_curl(curl_easy_setopt(curl_.get(), CURLOPT_POST, 1L), "Failed to configure POST request");
    require_curl(curl_easy_setopt(curl_.get(), CURLOPT_POSTFIELDS, request_body.data()), "Failed to configure request body");
    require_curl(
        curl_easy_setopt(curl_.get(), CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request_body.size())),
        "Failed to configure request body size"
    );
    require_curl(curl_easy_setopt(curl_.get(), CURLOPT_WRITEFUNCTION, receive_response), "Failed to configure response callback");
    require_curl(curl_easy_setopt(curl_.get(), CURLOPT_WRITEDATA, &response), "Failed to configure response destination");
    require_curl(curl_easy_setopt(curl_.get(), CURLOPT_CONNECTTIMEOUT, 10L), "Failed to configure connection timeout");
    require_curl(curl_easy_setopt(curl_.get(), CURLOPT_NOSIGNAL, 1L), "Failed to configure libcurl signals");
    require_curl(curl_easy_setopt(curl_.get(), CURLOPT_TCP_KEEPALIVE, 1L), "Failed to configure TCP keepalive");
    // Generation duration is intentionally unbounded; the TUI's /stop action
    // cancels the transfer through the progress callback below.
    require_curl(curl_easy_setopt(curl_.get(), CURLOPT_NOPROGRESS, 0L), "Failed to enable transfer progress");
    require_curl(
        curl_easy_setopt(curl_.get(), CURLOPT_XFERINFOFUNCTION, transfer_progress),
        "Failed to configure cancellation callback"
    );
    require_curl(
        curl_easy_setopt(curl_.get(), CURLOPT_XFERINFODATA, &cancellation_),
        "Failed to configure cancellation state"
    );

    curl_slist* raw_headers = nullptr;
    raw_headers = curl_slist_append(raw_headers, "Content-Type: application/json");
    raw_headers = curl_slist_append(raw_headers, config_.stream ? "Accept: text/event-stream" : "Accept: application/json");
    if (!api_key_.empty()) {
        raw_headers = curl_slist_append(raw_headers, ("Authorization: Bearer " + api_key_).c_str());
    }
    if (!raw_headers) {
        throw std::runtime_error("Failed to create HTTP headers");
    }
    CurlHeaders headers(raw_headers);
    require_curl(curl_easy_setopt(curl_.get(), CURLOPT_HTTPHEADER, headers.get()), "Failed to configure HTTP headers");

    const CURLcode perform_result = curl_easy_perform(curl_.get());
    if (response.error) {
        std::rethrow_exception(response.error);
    }
    if (perform_result == CURLE_ABORTED_BY_CALLBACK
        && cancellation_.load(std::memory_order_acquire)) {
        return {CompletionOutcome::cancelled, {}};
    }
    if (perform_result != CURLE_OK) {
        return {
            CompletionOutcome::transport_error,
            "HTTP request failed: " + std::string(curl_easy_strerror(perform_result)),
        };
    }

    long status = 0;
    require_curl(curl_easy_getinfo(curl_.get(), CURLINFO_RESPONSE_CODE, &status), "Failed to read HTTP status");
    if (status < 200 || status >= 300) {
        return {
            CompletionOutcome::protocol_error,
            "Inference server returned HTTP " + std::to_string(status) + ": " + response_error(response.body),
        };
    }

    if (config_.stream) {
        if (!response.pending.empty()) {
            process_stream_event(response.pending, response);
            response.pending.clear();
        }
        if (!response.protocol_error.empty()) {
            return {CompletionOutcome::protocol_error, response.protocol_error};
        }
        if (!response.done) {
            const std::string detail = response_error(response.body);
            return {
                CompletionOutcome::protocol_error,
                response.received_content
                    ? "Streaming response ended before [DONE]"
                    : "Streaming response was not valid SSE: " + detail,
            };
        }
        if (!response.received_content) {
            return {
                CompletionOutcome::protocol_error,
                "Streaming response completed without text content",
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
            "Inference server returned invalid JSON: " + std::string(error.what()),
        };
    }
    const Json::json_pointer content_pointer("/choices/0/message/content");
    if (!value.contains(content_pointer) || !value.at(content_pointer).is_string()) {
        return {
            CompletionOutcome::protocol_error,
            "Response did not contain choices[0].message.content",
        };
    }

    const std::string output = value.at(content_pointer).get<std::string>();
    if (output.empty()) {
        return {
            CompletionOutcome::protocol_error,
            "Response completed without text content",
        };
    }
    events.push(AgentDelta{request.request_id, output});
    return {CompletionOutcome::completed, {}};
}

std::string Agent::base_url() const {
    std::string host = config_.host;
    if (host.find(':') != std::string::npos && !host.starts_with('[')) {
        host = '[' + host + ']';
    }
    return std::string(config_.https ? "https://" : "http://") + host + ':' + std::to_string(config_.port);
}

std::string Agent::endpoint() const {
    return base_url() + "/v1/chat/completions";
}

std::string Agent::models_endpoint() const {
    return base_url() + "/v1/models";
}

} // namespace cha
