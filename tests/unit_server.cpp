#include "config.h"
#include "pipe.h"
#include "server.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <netinet/in.h>
#include <optional>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace cha {
namespace {

using Json = nlohmann::json;

class MockHttpServer {
public:
    explicit MockHttpServer(std::vector<std::string> responses) : responses_(std::move(responses)) {
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == -1) {
            throw std::runtime_error("Failed to create mock server socket");
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == -1
            || ::listen(listener_, 4) == -1) {
            ::close(listener_);
            throw std::runtime_error("Failed to bind mock server socket");
        }

        socklen_t address_length = sizeof(address);
        if (::getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &address_length) == -1) {
            ::close(listener_);
            throw std::runtime_error("Failed to read mock server port");
        }
        port_ = static_cast<int>(::ntohs(address.sin_port));
    }

    ~MockHttpServer() {
        if (thread_.joinable()) {
            thread_.join();
        }
        if (listener_ != -1) {
            ::close(listener_);
        }
    }

    MockHttpServer(const MockHttpServer&) = delete;
    MockHttpServer& operator=(const MockHttpServer&) = delete;

    [[nodiscard]] int port() const {
        return port_;
    }

    void start() {
        thread_ = std::thread([this] {
            try {
                for (const std::string& response : responses_) {
                    const int client = accept_connection();
                    requests_.push_back(read_request(client));
                    send_all(client, response);
                    ::shutdown(client, SHUT_RDWR);
                    ::close(client);
                }
            } catch (...) {
                error_ = std::current_exception();
            }
        });
    }

    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
        if (error_) {
            std::rethrow_exception(error_);
        }
    }

    [[nodiscard]] const std::vector<std::string>& requests() const {
        return requests_;
    }

private:
    [[nodiscard]] int accept_connection() const {
        pollfd descriptor{listener_, POLLIN, 0};
        if (::poll(&descriptor, 1, 5000) != 1) {
            throw std::runtime_error("Timed out waiting for mock client");
        }

        const int client = ::accept(listener_, nullptr, nullptr);
        if (client == -1) {
            throw std::runtime_error("Failed to accept mock client");
        }
        return client;
    }

    [[nodiscard]] static std::string read_request(int client) {
        std::string request;
        std::array<char, 4096> buffer{};
        std::size_t expected_size = std::string::npos;

        while (expected_size == std::string::npos || request.size() < expected_size) {
            const ssize_t bytes = ::recv(client, buffer.data(), buffer.size(), 0);
            if (bytes <= 0) {
                throw std::runtime_error("Failed to read mock HTTP request");
            }
            request.append(buffer.data(), static_cast<std::size_t>(bytes));

            const std::size_t body_start = request.find("\r\n\r\n");
            if (body_start == std::string::npos) {
                continue;
            }

            const std::size_t length_start = request.find("Content-Length:");
            if (length_start == std::string::npos) {
                throw std::runtime_error("Mock request has no Content-Length header");
            }
            const std::size_t value_start = length_start + std::string_view("Content-Length:").size();
            const std::size_t value_end = request.find("\r\n", value_start);
            const std::size_t content_length = std::stoul(request.substr(value_start, value_end - value_start));
            expected_size = body_start + 4 + content_length;
        }

        return request;
    }

    static void send_all(int client, std::string_view response) {
        while (!response.empty()) {
            const ssize_t bytes = ::send(client, response.data(), response.size(), 0);
            if (bytes <= 0) {
                throw std::runtime_error("Failed to send mock HTTP response");
            }
            response.remove_prefix(static_cast<std::size_t>(bytes));
        }
    }

    int listener_{-1};
    int port_{};
    std::vector<std::string> responses_;
    std::vector<std::string> requests_;
    std::exception_ptr error_;
    std::thread thread_;
};

std::string http_response(std::string_view content_type, const std::string& body) {
    return "HTTP/1.1 200 OK\r\nContent-Type: " + std::string(content_type)
        + "\r\nContent-Length: " + std::to_string(body.size())
        + "\r\nConnection: close\r\n\r\n" + body;
}

std::string request_body(const std::string& request) {
    const std::size_t body_start = request.find("\r\n\r\n");
    return request.substr(body_start + 4);
}

TEST(Server, EchoesEachInputLineTwiceThenEndsTheMessage) {
    Config config;
    Pipe pipe_in;
    Pipe pipe_out;
    std::atomic_bool cancellation{false};
    Server server(config, cancellation);

    server.run(pipe_in, pipe_out);
    pipe_in.put("hello");
    pipe_in.eom();

    EXPECT_EQ(pipe_out.get(), (PipeEvent{PipeEventKind::data, "hello"}));
    EXPECT_EQ(pipe_out.get(), (PipeEvent{PipeEventKind::data, "hello"}));
    EXPECT_EQ(pipe_out.get(), (PipeEvent{PipeEventKind::eom, {}}));

    pipe_in.put("again");
    pipe_in.eom();

    EXPECT_EQ(pipe_out.get(), (PipeEvent{PipeEventKind::data, "again"}));
    EXPECT_EQ(pipe_out.get(), (PipeEvent{PipeEventKind::data, "again"}));
    EXPECT_EQ(pipe_out.get(), (PipeEvent{PipeEventKind::eom, {}}));

    pipe_in.close();
    server.join();
}

TEST(Server, ConstructionDoesNotRequireANetworkConnection) {
    Config config{"127.0.0.1", 1, Mode::net};
    std::atomic_bool cancellation{false};
    EXPECT_NO_THROW({ Server server(config, cancellation); });
}

TEST(Server, StreamsResponsesAndMaintainsConversationHistory) {
    const std::string first_stream =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\" world\"}}]}\n\n"
        "data: [DONE]\n\n";
    const std::string second_stream =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Again\"}}]}\n\n"
        "data: [DONE]\n\n";
    MockHttpServer mock({
        http_response("text/event-stream", first_stream),
        http_response("text/event-stream", second_stream),
    });
    mock.start();

    const auto prompt_path = std::filesystem::temp_directory_path() / "cha_server_system_prompt.txt";
    {
        std::ofstream prompt_file(prompt_path);
        prompt_file << "Be concise.";
    }

    Config config;
    config.host = "127.0.0.1";
    config.port = mock.port();
    config.mode = Mode::net;
    config.model = "initial-model";
    config.stream = true;
    config.temperature = 0.5;
    config.api_key = "test-key";
    config.system_prompt = prompt_path;

    Pipe pipe_in;
    Pipe pipe_out;
    std::atomic_bool cancellation{false};
    Server server(config, cancellation);
    server.run(pipe_in, pipe_out);

    pipe_in.put("First question");
    pipe_in.eom();
    EXPECT_EQ(pipe_out.get(), (PipeEvent{PipeEventKind::data, "Hello"}));
    EXPECT_EQ(pipe_out.get(), (PipeEvent{PipeEventKind::data, " world"}));
    EXPECT_EQ(pipe_out.get(), (PipeEvent{PipeEventKind::eom, {}}));

    pipe_in.put("Second question");
    pipe_in.eom();
    EXPECT_EQ(pipe_out.get(), (PipeEvent{PipeEventKind::data, "Again"}));
    EXPECT_EQ(pipe_out.get(), (PipeEvent{PipeEventKind::eom, {}}));

    pipe_in.close();
    server.join();
    server.close();
    mock.join();
    std::filesystem::remove(prompt_path);

    ASSERT_EQ(mock.requests().size(), 2U);
    EXPECT_NE(mock.requests()[0].find("Authorization: Bearer test-key"), std::string::npos);

    const Json first_request = Json::parse(request_body(mock.requests()[0]));
    EXPECT_EQ(first_request["model"], "initial-model");
    EXPECT_TRUE(first_request["stream"]);
    EXPECT_DOUBLE_EQ(first_request["temperature"].get<double>(), 0.5);
    ASSERT_EQ(first_request["messages"].size(), 2U);
    EXPECT_EQ(first_request["messages"][0]["role"], "system");
    EXPECT_EQ(first_request["messages"][0]["content"], "Be concise.");
    EXPECT_EQ(first_request["messages"][1]["content"], "First question");

    const Json second_request = Json::parse(request_body(mock.requests()[1]));
    ASSERT_EQ(second_request["messages"].size(), 4U);
    EXPECT_EQ(second_request["messages"][2]["role"], "assistant");
    EXPECT_EQ(second_request["messages"][2]["content"], "Hello world");
    EXPECT_EQ(second_request["messages"][3]["content"], "Second question");
}

TEST(Server, HandlesCommandsAndNonStreamingResponse) {
    const std::string response_body = R"({"choices":[{"message":{"content":"Complete answer"}}]})";
    MockHttpServer mock({http_response("application/json", response_body)});
    mock.start();

    Config config;
    config.host = "127.0.0.1";
    config.port = mock.port();
    config.mode = Mode::net;
    config.model = "initial-model";
    config.stream = false;

    Pipe pipe_in;
    Pipe pipe_out;
    std::atomic_bool cancellation{false};
    Server server(config, cancellation);
    server.run(pipe_in, pipe_out);

    pipe_in.put(".model replacement-model");
    pipe_in.eom();
    EXPECT_EQ(pipe_out.get(), (PipeEvent{PipeEventKind::data, "Model: replacement-model"}));
    EXPECT_EQ(pipe_out.get(), (PipeEvent{PipeEventKind::eom, {}}));

    pipe_in.put(".info");
    pipe_in.eom();
    const PipeEvent info = pipe_out.get();
    ASSERT_EQ(info.kind, PipeEventKind::data);
    EXPECT_NE(info.data.find("Model: replacement-model"), std::string::npos);
    EXPECT_NE(info.data.find("Streaming: no"), std::string::npos);
    EXPECT_EQ(pipe_out.get(), (PipeEvent{PipeEventKind::eom, {}}));

    pipe_in.put("Question");
    pipe_in.eom();
    EXPECT_EQ(pipe_out.get(), (PipeEvent{PipeEventKind::data, "Complete answer"}));
    EXPECT_EQ(pipe_out.get(), (PipeEvent{PipeEventKind::eom, {}}));

    pipe_in.put(".clear");
    pipe_in.eom();
    EXPECT_EQ(pipe_out.get(), (PipeEvent{PipeEventKind::data, "Conversation cleared."}));
    EXPECT_EQ(pipe_out.get(), (PipeEvent{PipeEventKind::eom, {}}));

    pipe_in.put(".unknown");
    pipe_in.eom();
    EXPECT_EQ(
        pipe_out.get(),
        (PipeEvent{
            PipeEventKind::data,
            "Unknown command. Server commands: .clear, .info, .model MODEL. Local commands: .stop, .exit"
        })
    );
    EXPECT_EQ(pipe_out.get(), (PipeEvent{PipeEventKind::eom, {}}));

    pipe_in.close();
    server.join();
    server.close();
    mock.join();

    ASSERT_EQ(mock.requests().size(), 1U);
    const Json request = Json::parse(request_body(mock.requests()[0]));
    EXPECT_EQ(request["model"], "replacement-model");
    EXPECT_FALSE(request["stream"]);
    ASSERT_EQ(request["messages"].size(), 1U);
    EXPECT_EQ(request["messages"][0]["content"], "Question");
}

} // namespace
} // namespace cha
