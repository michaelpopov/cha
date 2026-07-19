#include "server.h"

#include "command.h"
#include "pipe.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
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
    bool streaming{};
    bool done{};
    std::string body;
    std::string pending;
    std::string output;
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
                const Json value = Json::parse(data);
                const Json::json_pointer content_pointer("/choices/0/delta/content");

                if (value.contains(content_pointer) && value.at(content_pointer).is_string()) {
                    const std::string content = value.at(content_pointer).get<std::string>();
                    context.pipe->put(content);
                    context.output += content;
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

Server::Server(const Config& config, std::atomic_bool& cancellation)
    : _config(config), _cancellation(cancellation), model_(config.model) {

    if (!_config.system_prompt.empty()) {
        std::ifstream prompt_file(_config.system_prompt, std::ios::binary);
        if (!prompt_file) {
            throw std::runtime_error("Failed to read system prompt file '" + _config.system_prompt.string() + "'");
        }

        std::ostringstream contents;
        contents << prompt_file.rdbuf();
        system_prompt_ = contents.str();
    }

    reset_history();

    if (_config.mode == Mode::net) {
        (void)curl_global();
        curl_ = curl_easy_init();
        if (!curl_) {
            throw std::runtime_error("Failed to create libcurl handle");
        }
    }
}

Server::~Server() {
    close();
}

void Server::run(Pipe& pipe_in, Pipe& pipe_out) {
    if (thread_.joinable()) {
        throw std::logic_error("Server is already running");
    }

    thread_ = std::thread(&Server::dialog, this, std::ref(pipe_in), std::ref(pipe_out));
}

void Server::close() {
    if (thread_.joinable()) {
        thread_.join();
    }

    if (!curl_) {
        return;
    }

    curl_easy_cleanup(curl_);
    curl_ = nullptr;
}

void Server::dialog(Pipe& pipe_in, Pipe& pipe_out) {
    try {
        while (true) {
            const PipeEvent input = pipe_in.get();

            if (input.kind == PipeEventKind::closed) {
                break;
            }
            if (input.kind == PipeEventKind::eom) {
                continue;
            }

            if (_config.mode == Mode::test) {
                pipe_out.put(input.data);
                pipe_out.put(input.data);
                pipe_out.eom();
                continue;
            }

            if (handle_command(input.data, pipe_out)) {
                continue;
            }

            messages_.push_back(Message{"user", input.data});
            try {
                const std::string output = complete(pipe_out);
                if (!output.empty()) {
                    messages_.push_back(Message{"assistant", output});
                }
            } catch (const std::exception& error) {
                messages_.pop_back();
                pipe_out.put("Error: " + std::string(error.what()));
            }
            pipe_out.eom();
        }
    } catch (const std::exception& error) {
        pipe_out.put("Error: " + std::string(error.what()));
        pipe_out.eom();
    }

    pipe_out.close();
}

void Server::reset_history() {
    messages_.clear();
    if (!system_prompt_.empty()) {
        messages_.push_back(Message{"system", system_prompt_});
    }
}

bool Server::handle_command(const std::string& input, Pipe& pipe_out) {
    const Command command = parse_command(input);

    if (command.kind == CommandKind::clear && command.argument.empty()) {
        reset_history();
        pipe_out.put("Conversation cleared.");
        pipe_out.eom();
        return true;
    }

    if (command.kind == CommandKind::info && command.argument.empty()) {
        std::ostringstream info;
        info << "Model: " << model_ << '\n'
             << "API: " << endpoint() << '\n'
             << "Streaming: " << (_config.stream ? "yes" : "no") << '\n'
             << "Messages in memory: " << messages_.size();
        pipe_out.put(info.str());
        pipe_out.eom();
        return true;
    }

    if (command.kind == CommandKind::model) {
        if (command.argument.empty()) {
            pipe_out.put("Usage: .model MODEL");
        } else {
            model_ = command.argument;
            reset_history();
            pipe_out.put("Model: " + model_);
        }
        pipe_out.eom();
        return true;
    }

    if (command.kind != CommandKind::text) {
        pipe_out.put("Unknown command. Server commands: .clear, .info, .model MODEL. Local commands: .stop, .exit");
        pipe_out.eom();
        return true;
    }

    return false;
}

std::string Server::complete(Pipe& pipe_out) {
    if (!curl_) {
        throw std::runtime_error("Server HTTP client is closed");
    }

    Json body;
    body["model"] = model_;
    body["stream"] = _config.stream;
    body["messages"] = Json::array();
    for (const Message& message : messages_) {
        body["messages"].push_back({
            {"role", message.role},
            {"content", message.content},
        });
    }
    if (_config.temperature) {
        body["temperature"] = *_config.temperature;
    }

    const std::string request_body = body.dump();
    ResponseContext response{
        .pipe = &pipe_out,
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
    // Generation duration is intentionally unbounded; the TUI's .stop action
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
    if (_cancellation.load(std::memory_order_acquire)) {
        return response.output;
    }
    if (response.error) {
        std::rethrow_exception(response.error);
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
        return response.output;
    }

    const Json value = Json::parse(response.body);
    const Json::json_pointer content_pointer("/choices/0/message/content");
    if (!value.contains(content_pointer) || !value.at(content_pointer).is_string()) {
        throw std::runtime_error("Response did not contain choices[0].message.content");
    }

    const std::string output = value.at(content_pointer).get<std::string>();
    pipe_out.put(output);
    return output;
}

std::string Server::base_url() const {
    std::string host = _config.host;
    if (host.find(':') != std::string::npos && !host.starts_with('[')) {
        host = '[' + host + ']';
    }
    return "http://" + host + ':' + std::to_string(_config.port);
}

std::string Server::endpoint() const {
    return base_url() + "/v1/chat/completions";
}

} // namespace cha
