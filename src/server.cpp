#include "server.h"

#include "command.h"
#include "conversation.h"
#include "pipe.h"
#include "server_context.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <functional>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace cha {
namespace {

using Json = nlohmann::json;

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

struct CurlHeadersDeleter {
    void operator()(curl_slist* headers) const {
        curl_slist_free_all(headers);
    }
};

using CurlHeaders = std::unique_ptr<curl_slist, CurlHeadersDeleter>;

constexpr std::size_t max_streaming_error_body_size = 64 * 1024;

struct ResponseContext {
    Pipe* pipe{};
    Conversation* conversation{};
    bool streaming{};
    bool done{};
    std::string body;
    std::string pending;
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
            } else if (!data.empty()) {
                try {
                    const Json value = Json::parse(data);
                    const Json::json_pointer content_pointer("/choices/0/delta/content");

                    if (value.contains(content_pointer) && value.at(content_pointer).is_string()) {
                        const std::string content = value.at(content_pointer).get<std::string>();
                        context.conversation->append_to_message(content);
                        context.pipe->conversation_updated();
                    }
                } catch (const Json::parse_error&) {
                    // A malformed event should not discard valid chunks elsewhere in the stream.
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
        if (context.streaming) {
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

Server::Server(
    std::atomic_bool& cancellation,
    Conversation& conversation,
    std::string name)
    : _cancellation(cancellation),
      _conversation(conversation),
      name_(std::move(name)) {

}

Server::~Server() {
    stop();
}

void Server::init() {
    init(cha::Config::load());
}

void Server::init(const Config& config) {
    _config = config;
    initialize();
}

void Server::initialize() {

    if (name_.empty()) {
        name_ = _config.name;
    }
    system_prompt_ = _config.system_prompt;

    if (!_config.api_key_env.empty()) {
        const char* api_key = std::getenv(_config.api_key_env.c_str());
        if (!api_key || *api_key == '\0') {
            throw std::runtime_error(
                "Environment variable '" + _config.api_key_env + "' configured as api_key_env is not set"
            );
        }
        _config.api_key = api_key;
    }

    if (_config.mode == Mode::net) {
        (void)curl_global();
        curl_ = curl_easy_init();
        if (!curl_) {
            throw std::runtime_error("Failed to create libcurl handle");
        }
        if (_config.model.empty()) {
            discover_model();
        }
    }
}

void Server::discover_model() {
    ResponseContext response{.streaming = false};

    curl_easy_reset(curl_);
    const std::string url = models_endpoint();
    require_curl(curl_easy_setopt(curl_, CURLOPT_URL, url.c_str()), "Failed to configure models request URL");
    require_curl(curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L), "Failed to configure models request");
    require_curl(curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, receive_response), "Failed to configure models response callback");
    require_curl(curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response), "Failed to configure models response destination");
    require_curl(curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 10L), "Failed to configure models connection timeout");
    require_curl(curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L), "Failed to configure libcurl signals");

    curl_slist* raw_headers = nullptr;
    raw_headers = curl_slist_append(raw_headers, "Accept: application/json");
    if (!_config.api_key.empty()) {
        raw_headers = curl_slist_append(raw_headers, ("Authorization: Bearer " + _config.api_key).c_str());
    }
    if (!raw_headers) {
        throw std::runtime_error("Failed to create models request headers");
    }
    CurlHeaders headers(raw_headers);
    require_curl(curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers.get()), "Failed to configure models request headers");

    const CURLcode perform_result = curl_easy_perform(curl_);
    if (response.error) {
        std::rethrow_exception(response.error);
    }
    if (perform_result != CURLE_OK) {
        throw std::runtime_error("Models request failed: " + std::string(curl_easy_strerror(perform_result)));
    }

    long status = 0;
    require_curl(curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &status), "Failed to read models response status");
    if (status < 200 || status >= 300) {
        throw std::runtime_error("Models request returned HTTP " + std::to_string(status) + ": " + response_error(response.body));
    }

    try {
        const Json value = Json::parse(response.body);
        const Json::json_pointer model_pointer("/data/0/id");
        if (!value.contains(model_pointer) || !value.at(model_pointer).is_string()) {
            throw std::runtime_error("Models response did not contain data[0].id");
        }
        _config.model = value.at(model_pointer).get<std::string>();
    } catch (const Json::exception& error) {
        throw std::runtime_error("Failed to parse models response: " + std::string(error.what()));
    }
}

void Server::run(Pipe& pipe_in, Pipe& pipe_out) {
    if (thread_.joinable()) {
        throw std::logic_error("Server is already running");
    }

    input_ = &pipe_in;
    thread_ = std::thread(&Server::dialog, this, std::ref(pipe_in), std::ref(pipe_out));
}

void Server::stop() {
    if (thread_.joinable()) {
        const bool was_cancelled = _cancellation.exchange(true, std::memory_order_acq_rel);
        input_->close();
        thread_.join();
        _cancellation.store(was_cancelled, std::memory_order_release);
    }
    input_ = nullptr;

    if (!curl_) {
        return;
    }

    curl_easy_cleanup(curl_);
    curl_ = nullptr;
}

void Server::dialog(Pipe& pipe_in, Pipe& pipe_out) {
    while (true) {
        try {
            const PipeEvent input = pipe_in.get();

            if (input.kind == PipeEventKind::closed) {
                break;
            }
            if (input.kind == PipeEventKind::eom) {
                continue;
            }
            if (input.kind != PipeEventKind::data) {
                continue;
            }

            if (_config.mode == Mode::test) {
                _conversation.begin_message(name_);
                _conversation.append_to_message(input.data);
                pipe_out.conversation_updated();
                _conversation.append_to_message(input.data);
                pipe_out.conversation_updated();
                _conversation.finish_message();
                pipe_out.eom();
                continue;
            }

            if (handle_command(input.data, pipe_out)) {
                continue;
            }

            _conversation.begin_message(name_);
            try {
                complete(pipe_out);
                _conversation.finish_message();
            } catch (const std::exception& error) {
                _conversation.discard_message();
                const std::string message = "Error: " + std::string(error.what());
                _conversation.add_message(std::string(system_author), message);
                pipe_out.conversation_updated();
            }
            pipe_out.eom();
        } catch (const std::exception& error) {
            _conversation.discard_message();
            _conversation.add_message(std::string(system_author), "Error: " + std::string(error.what()));
            pipe_out.conversation_updated();
            pipe_out.eom();
        }
    }
}

bool Server::handle_command(const std::string& input, Pipe& pipe_out) {
    const Command command = parse_command(input);

    const auto reply = [&](std::string text) {
        _conversation.begin_message(name_);
        _conversation.append_to_message(text);
        _conversation.finish_message();
        pipe_out.conversation_updated();
        pipe_out.eom();
    };

    if (command.kind == CommandKind::clear && command.argument.empty()) {
        reply("Conversation cleared.");
        return true;
    }

    if (command.kind == CommandKind::info && command.argument.empty()) {
        std::ostringstream info;
        info << "Model: " << _config.model << '\n'
             << "API: " << endpoint() << '\n'
             << "Streaming: " << (_config.stream ? "yes" : "no") << '\n'
             << "Messages in memory: "
             << build_server_context(_conversation.snapshot(), system_prompt_, name_).size();
        reply(info.str());
        return true;
    }

    if (command.kind != CommandKind::text) {
        reply("Unknown command. Server commands: /clear, /info. Local commands: /stop, /exit");
        return true;
    }

    return false;
}

void Server::complete(Pipe& pipe_out) {
    if (!curl_) {
        throw std::runtime_error("Server HTTP client is closed");
    }

    Json body;
    body["model"] = _config.model;
    body["stream"] = _config.stream;
    body["messages"] = Json::array();
    for (const ServerMessage& message : build_server_context(_conversation.snapshot(), system_prompt_, name_)) {
        body["messages"].push_back({
            {"role", message.role},
            {"content", message.content},
        });
    }
    if (_config.temperature) {
        body["temperature"] = *_config.temperature;
    }
    if (!_config.reasoning_effort.empty()) {
        body["reasoning_effort"] = _config.reasoning_effort;
    }

    const std::string request_body = body.dump();
    ResponseContext response{
        .pipe = &pipe_out,
        .conversation = &_conversation,
        .streaming = _config.stream,
    };

    curl_easy_reset(curl_);
    const std::string url = endpoint();
    require_curl(curl_easy_setopt(curl_, CURLOPT_URL, url.c_str()), "Failed to configure request URL");
    require_curl(curl_easy_setopt(curl_, CURLOPT_POST, 1L), "Failed to configure POST request");
    require_curl(curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, request_body.data()), "Failed to configure request body");
    require_curl(
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request_body.size())),
        "Failed to configure request body size"
    );
    require_curl(curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, receive_response), "Failed to configure response callback");
    require_curl(curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response), "Failed to configure response destination");
    require_curl(curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 10L), "Failed to configure connection timeout");
    require_curl(curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L), "Failed to configure libcurl signals");
    require_curl(curl_easy_setopt(curl_, CURLOPT_TCP_KEEPALIVE, 1L), "Failed to configure TCP keepalive");
    // Generation duration is intentionally unbounded; the TUI's /stop action
    // cancels the transfer through the progress callback below.
    require_curl(curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, 0L), "Failed to enable transfer progress");
    require_curl(
        curl_easy_setopt(curl_, CURLOPT_XFERINFOFUNCTION, transfer_progress),
        "Failed to configure cancellation callback"
    );
    require_curl(
        curl_easy_setopt(curl_, CURLOPT_XFERINFODATA, &_cancellation),
        "Failed to configure cancellation state"
    );

    curl_slist* raw_headers = nullptr;
    raw_headers = curl_slist_append(raw_headers, "Content-Type: application/json");
    raw_headers = curl_slist_append(raw_headers, _config.stream ? "Accept: text/event-stream" : "Accept: application/json");
    if (!_config.api_key.empty()) {
        raw_headers = curl_slist_append(raw_headers, ("Authorization: Bearer " + _config.api_key).c_str());
    }
    if (!raw_headers) {
        throw std::runtime_error("Failed to create HTTP headers");
    }
    CurlHeaders headers(raw_headers);
    require_curl(curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers.get()), "Failed to configure HTTP headers");

    const CURLcode perform_result = curl_easy_perform(curl_);
    if (response.error) {
        std::rethrow_exception(response.error);
    }
    if (_cancellation.load(std::memory_order_acquire)) {
        return;
    }
    if (perform_result != CURLE_OK) {
        throw std::runtime_error("HTTP request failed: " + std::string(curl_easy_strerror(perform_result)));
    }

    long status = 0;
    require_curl(curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &status), "Failed to read HTTP status");
    if (status < 200 || status >= 300) {
        throw std::runtime_error("Server returned HTTP " + std::to_string(status) + ": " + response_error(response.body));
    }

    if (_config.stream) {
        if (!response.pending.empty()) {
            process_stream_event(response.pending, response);
            response.pending.clear();
        }
        return;
    }

    const Json value = Json::parse(response.body);
    const Json::json_pointer content_pointer("/choices/0/message/content");
    if (!value.contains(content_pointer) || !value.at(content_pointer).is_string()) {
        throw std::runtime_error("Response did not contain choices[0].message.content");
    }

    const std::string output = value.at(content_pointer).get<std::string>();
    _conversation.append_to_message(output);
    pipe_out.conversation_updated();
}

std::string Server::base_url() const {
    std::string host = _config.host;
    if (host.find(':') != std::string::npos && !host.starts_with('[')) {
        host = '[' + host + ']';
    }
    return std::string(_config.https ? "https://" : "http://") + host + ':' + std::to_string(_config.port);
}

std::string Server::endpoint() const {
    return base_url() + "/v1/chat/completions";
}

std::string Server::models_endpoint() const {
    return base_url() + "/v1/models";
}

} // namespace cha
