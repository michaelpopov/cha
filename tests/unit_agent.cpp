#include "config.h"
#include "agent_protocol.h"
#include "agent.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <cstdlib>
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
#include <variant>
#include <vector>

namespace cha {
namespace {

using Json = nlohmann::json;

// Supplies scripted HTTP responses and records requests for isolated Agent tests.
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
                if (request.starts_with("GET ")) {
                    return request;
                }
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

CompletionRequest request(
    RequestId request_id,
    std::string agent_id,
    std::string prompt,
    std::vector<ConversationEntry> history = {}) {
    return {
        .request_id = request_id,
        .agent_id = std::move(agent_id),
        .history = std::move(history),
        .prompt = make_human_entry(1000 + request_id, std::move(prompt), request_id),
    };
}

AgentEvent next_event(AgentEventChannel& events) {
    std::optional<AgentEvent> event = events.get();
    if (!event) {
        throw std::runtime_error("Agent event channel closed unexpectedly");
    }
    return std::move(*event);
}

TEST(Agent, EchoesImmutableRequestPromptWithIdentifiedEvents) {
    Config config;
    config.id = "local-agent";
    config.name = "Local agent";
    CompletionRequestChannel requests;
    AgentEventChannel events;
    std::atomic_bool cancellation{false};
    Agent agent(cancellation);
    agent.init(config);

    agent.run(requests, events);
    ASSERT_TRUE(requests.push(request(
        41,
        "local-agent",
        "hello",
        {make_human_entry(1, "different history")})));

    EXPECT_EQ(std::get<AgentDelta>(next_event(events)).request_id, 41U);
    EXPECT_EQ(std::get<AgentDelta>(next_event(events)).text, "hello");
    EXPECT_EQ(std::get<AgentCompleted>(next_event(events)).request_id, 41U);
    agent.stop();
}

TEST(Agent, LoadsItsConfigurationFromPersonaAndRoomDirectories) {
    const auto directory = std::filesystem::temp_directory_path()
        / ("cha_server_directories_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto persona_directory = directory / "guide";
    const auto room_directory = directory / "room";
    std::filesystem::create_directories(persona_directory);
    std::filesystem::create_directories(room_directory);
    {
        std::ofstream config(persona_directory / "config.toml");
        config << "id = \"guide-id\"\nname = \"Guide\"\n"
               << "host = \"127.0.0.1\"\nport = 8080\nmode = \"test\"\n";
        std::ofstream system_prompt(persona_directory / "SYSTEM.md");
        system_prompt << "Persona instructions";
        std::ofstream user_prompt(room_directory / "USER.md");
        user_prompt << "Room instructions";
    }

    std::atomic_bool cancellation{false};
    Agent agent(cancellation);
    agent.init(persona_directory, room_directory);
    EXPECT_EQ(agent.info().id, "guide-id");
    EXPECT_EQ(agent.info().name, "Guide");

    std::filesystem::remove_all(directory);
}

TEST(Agent, ConstructionDoesNotRequireANetworkConnection) {
    Config config{
        .host = "127.0.0.1",
        .port = 1,
        .mode = Mode::net,
    };
    config.model = "configured-model";
    std::atomic_bool cancellation{false};
    EXPECT_NO_THROW({
        Agent agent(cancellation);
        agent.init(config);
    });
}

TEST(Agent, StoppingDoesNotCloseTheSharedOutputChannel) {
    Config config;
    CompletionRequestChannel requests;
    AgentEventChannel events;
    std::atomic_bool cancellation{false};
    Agent agent(cancellation);
    agent.init(config);

    agent.run(requests, events);
    agent.stop();

    AgentEvent event = AgentCompleted{};
    EXPECT_EQ(events.try_get(event), ChannelReadStatus::empty);
    EXPECT_TRUE(events.push(AgentCompleted{7}));
    EXPECT_EQ(std::get<AgentCompleted>(next_event(events)).request_id, 7U);
}

TEST(Agent, RejectsARequestForAnotherAgentAndContinues) {
    Config config;
    CompletionRequestChannel requests;
    AgentEventChannel events;
    std::atomic_bool cancellation{false};
    Agent agent(cancellation);
    agent.init(config);

    agent.run(requests, events);
    ASSERT_TRUE(requests.push(request(1, "other-id", "rejected")));
    const AgentFailed failed = std::get<AgentFailed>(next_event(events));
    EXPECT_EQ(failed.request_id, 1U);
    EXPECT_NE(failed.message.find("targets agent"), std::string::npos);

    ASSERT_TRUE(requests.push(request(2, "assistant", "accepted")));
    EXPECT_EQ(std::get<AgentDelta>(next_event(events)).text, "accepted");
    EXPECT_EQ(std::get<AgentDelta>(next_event(events)).text, "accepted");
    EXPECT_EQ(std::get<AgentCompleted>(next_event(events)).request_id, 2U);
    agent.stop();
}

TEST(Agent, RejectsAnInvalidTypedPromptAndContinues) {
    Config config;
    CompletionRequestChannel requests;
    AgentEventChannel events;
    std::atomic_bool cancellation{false};
    Agent agent(cancellation);
    agent.init(config);

    CompletionRequest invalid = request(1, "assistant", "rejected");
    invalid.prompt.status = CompletionStatus::cancelled;

    agent.run(requests, events);
    ASSERT_TRUE(requests.push(std::move(invalid)));
    const AgentFailed failed = std::get<AgentFailed>(next_event(events));
    EXPECT_EQ(failed.request_id, 1U);
    EXPECT_NE(failed.message.find("require complete status"), std::string::npos);

    ASSERT_TRUE(requests.push(request(2, "assistant", "accepted")));
    EXPECT_EQ(std::get<AgentDelta>(next_event(events)).text, "accepted");
    EXPECT_EQ(std::get<AgentDelta>(next_event(events)).text, "accepted");
    EXPECT_EQ(std::get<AgentCompleted>(next_event(events)).request_id, 2U);
    agent.stop();
}

TEST(Agent, CancelsAnIdentifiedRequestBeforeStartingIt) {
    Config config;
    CompletionRequestChannel requests;
    AgentEventChannel events;
    std::atomic_bool cancellation{true};
    Agent agent(cancellation);
    agent.init(config);
    agent.run(requests, events);

    ASSERT_TRUE(requests.push(request(9, "assistant", "Do not run")));
    EXPECT_EQ(std::get<AgentCancelled>(next_event(events)).request_id, 9U);
    agent.stop();
}

TEST(Agent, StreamsResponsesFromImmutableRequestHistory) {
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

    Config config;
    config.host = "127.0.0.1";
    config.port = mock.port();
    config.mode = Mode::net;
    config.model = "initial-model";
    config.stream = true;
    config.temperature = 0.5;
    config.reasoning_effort = "medium";
    config.api_key_env = "CHA_TEST_API_KEY";
    config.system_prompt = "Be concise.";
    ASSERT_EQ(::setenv("CHA_TEST_API_KEY", "test-key", 1), 0);

    CompletionRequestChannel requests;
    AgentEventChannel events;
    std::atomic_bool cancellation{false};
    Agent agent(cancellation);
    agent.init(config);
    agent.run(requests, events);

    ASSERT_TRUE(requests.push(request(10, "assistant", "First question")));
    EXPECT_EQ(std::get<AgentDelta>(next_event(events)).text, "Hello");
    EXPECT_EQ(std::get<AgentDelta>(next_event(events)).text, " world");
    EXPECT_EQ(std::get<AgentCompleted>(next_event(events)).request_id, 10U);

    ASSERT_TRUE(requests.push(request(
        11,
        "assistant",
        "Second question",
        {
            make_human_entry(1, "First question", 10),
            make_agent_entry(
                2, "assistant", "Assistant", "Hello world", CompletionStatus::complete, 10),
        })));
    EXPECT_EQ(std::get<AgentDelta>(next_event(events)).text, "Again");
    EXPECT_EQ(std::get<AgentCompleted>(next_event(events)).request_id, 11U);

    agent.stop();
    mock.join();
    ASSERT_EQ(::unsetenv("CHA_TEST_API_KEY"), 0);
    ASSERT_EQ(mock.requests().size(), 2U);
    EXPECT_NE(mock.requests()[0].find("Authorization: Bearer test-key"), std::string::npos);

    const Json first_request = Json::parse(request_body(mock.requests()[0]));
    EXPECT_EQ(first_request["model"], "initial-model");
    EXPECT_TRUE(first_request["stream"]);
    EXPECT_DOUBLE_EQ(first_request["temperature"].get<double>(), 0.5);
    EXPECT_EQ(first_request["reasoning_effort"], "medium");
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

TEST(Agent, SkipsMalformedStreamingEvents) {
    const std::string stream =
        "data: not-json\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"Valid response\"}}]}\n\n"
        "data: [DONE]\n\n";
    MockHttpServer mock({http_response("text/event-stream", stream)});
    mock.start();

    Config config;
    config.host = "127.0.0.1";
    config.port = mock.port();
    config.mode = Mode::net;
    config.model = "configured-model";
    config.stream = true;

    CompletionRequestChannel requests;
    AgentEventChannel events;
    std::atomic_bool cancellation{false};
    Agent agent(cancellation);
    agent.init(config);
    agent.run(requests, events);

    ASSERT_TRUE(requests.push(request(3, "assistant", "Question")));
    EXPECT_EQ(std::get<AgentDelta>(next_event(events)).text, "Valid response");
    EXPECT_EQ(std::get<AgentCompleted>(next_event(events)).request_id, 3U);
    agent.stop();
    mock.join();
}

TEST(Agent, DiscoversTheFirstEndpointModelWhenNoneIsConfigured) {
    const std::string models_response = R"({"data":[{"id":"discovered-model"},{"id":"other-model"}]})";
    const std::string chat_response = R"({"choices":[{"message":{"content":"Complete answer"}}]})";
    MockHttpServer mock({
        http_response("application/json", models_response),
        http_response("application/json", chat_response),
    });
    mock.start();

    Config config;
    config.host = "127.0.0.1";
    config.port = mock.port();
    config.mode = Mode::net;
    config.stream = false;

    CompletionRequestChannel requests;
    AgentEventChannel events;
    std::atomic_bool cancellation{false};
    Agent agent(cancellation);
    agent.init(config);
    agent.run(requests, events);

    ASSERT_TRUE(requests.push(request(4, "assistant", "Question")));
    EXPECT_EQ(std::get<AgentDelta>(next_event(events)).text, "Complete answer");
    EXPECT_EQ(std::get<AgentCompleted>(next_event(events)).request_id, 4U);
    agent.stop();
    mock.join();

    ASSERT_EQ(mock.requests().size(), 2U);
    EXPECT_TRUE(mock.requests()[0].starts_with("GET /v1/models HTTP/1.1"));
    const Json request = Json::parse(request_body(mock.requests()[1]));
    EXPECT_EQ(request["model"], "discovered-model");
}

TEST(Agent, ProvidesInfoAndHandlesNonStreamingResponse) {
    const std::string response_body = R"({"choices":[{"message":{"content":"Complete answer"}}]})";
    MockHttpServer mock({http_response("application/json", response_body)});
    mock.start();

    Config config;
    config.host = "127.0.0.1";
    config.port = mock.port();
    config.mode = Mode::net;
    config.model = "initial-model";
    config.stream = false;

    CompletionRequestChannel requests;
    AgentEventChannel events;
    std::atomic_bool cancellation{false};
    Agent agent(cancellation);
    agent.init(config);
    const AgentInfo info = agent.info();
    EXPECT_EQ(info.model, "initial-model");
    EXPECT_FALSE(info.streaming);
    EXPECT_NE(info.api.find("/v1/chat/completions"), std::string::npos);
    agent.run(requests, events);

    ASSERT_TRUE(requests.push(request(5, "assistant", "Question")));
    EXPECT_EQ(std::get<AgentDelta>(next_event(events)).text, "Complete answer");
    EXPECT_EQ(std::get<AgentCompleted>(next_event(events)).request_id, 5U);

    agent.stop();
    mock.join();

    ASSERT_EQ(mock.requests().size(), 1U);
    const Json request = Json::parse(request_body(mock.requests()[0]));
    EXPECT_EQ(request["model"], "initial-model");
    EXPECT_FALSE(request["stream"]);
    ASSERT_EQ(request["messages"].size(), 1U);
    EXPECT_EQ(request["messages"][0]["content"], "Question");
}

TEST(Agent, NamedInstancesProjectTheProvidedHistoryForTheirOwnIdentity) {
    const std::string first_body = R"({"choices":[{"message":{"content":"Initial answer"}}]})";
    const std::string second_body = R"({"choices":[{"message":{"content":"Reviewed answer"}}]})";
    MockHttpServer first_mock({http_response("application/json", first_body)});
    MockHttpServer second_mock({http_response("application/json", second_body)});
    first_mock.start();
    second_mock.start();

    std::atomic_bool cancellation{false};

    Config first_config;
    first_config.host = "127.0.0.1";
    first_config.port = first_mock.port();
    first_config.mode = Mode::net;
    first_config.model = "writer-model";
    first_config.stream = false;

    CompletionRequestChannel first_input;
    AgentEventChannel first_output;
    Agent writer(cancellation, "writer-id", "Writer");
    writer.init(first_config);
    writer.run(first_input, first_output);
    ASSERT_TRUE(first_input.push(request(6, "writer-id", "Draft an answer")));
    EXPECT_EQ(std::get<AgentDelta>(next_event(first_output)).text, "Initial answer");
    EXPECT_TRUE(std::holds_alternative<AgentCompleted>(next_event(first_output)));
    writer.stop();
    first_mock.join();

    Config second_config = first_config;
    second_config.port = second_mock.port();
    second_config.model = "reviewer-model";

    CompletionRequestChannel second_input;
    AgentEventChannel second_output;
    Agent reviewer(cancellation, "reviewer-id", "Reviewer");
    reviewer.init(second_config);
    reviewer.run(second_input, second_output);
    ASSERT_TRUE(second_input.push(request(
        7,
        "reviewer-id",
        "Review the draft",
        {
            make_human_entry(1, "Draft an answer", 6),
            make_agent_entry(
                2, "writer-id", "Writer", "Initial answer", CompletionStatus::complete, 6),
        })));
    EXPECT_EQ(std::get<AgentDelta>(next_event(second_output)).text, "Reviewed answer");
    EXPECT_TRUE(std::holds_alternative<AgentCompleted>(next_event(second_output)));
    reviewer.stop();
    second_mock.join();

    ASSERT_EQ(second_mock.requests().size(), 1U);
    const Json request = Json::parse(request_body(second_mock.requests()[0]));
    ASSERT_EQ(request["messages"].size(), 3U);
    EXPECT_EQ(request["messages"][0]["content"], "Draft an answer");
    EXPECT_EQ(request["messages"][1]["role"], "assistant");
    EXPECT_FALSE(request["messages"][1].contains("name"));
    EXPECT_EQ(request["messages"][1]["content"], "writer-id: Initial answer");
    EXPECT_EQ(request["messages"][2]["content"], "Review the draft");
}

} // namespace
} // namespace cha
