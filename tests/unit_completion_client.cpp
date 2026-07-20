#include "completion_client.h"
#include "conversation.h"
#include "mock_http_server.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cha {
namespace {

using Json = nlohmann::json;

CompletionRequest client_request(
    RequestId request_id,
    std::string prompt,
    std::vector<ConversationEntry> history = {}) {
    return {
        .request_id = request_id,
        .agent_id = "assistant",
        .history = std::move(history),
        .prompt =
            make_human_entry(1000 + request_id, std::move(prompt), request_id),
    };
}

Config network_config(int port, bool stream = true) {
    Config config;
    config.host = "127.0.0.1";
    config.port = port;
    config.mode = Mode::net;
    config.model = "configured-model";
    config.stream = stream;
    return config;
}

std::string status_response(
    int status,
    std::string_view reason,
    std::string_view content_type,
    const std::string& body) {
    return "HTTP/1.1 " + std::to_string(status) + " "
        + std::string(reason) + "\r\nContent-Type: "
        + std::string(content_type)
        + "\r\nContent-Length: " + std::to_string(body.size())
        + "\r\nConnection: close\r\n\r\n" + body;
}

TEST(CompletionClient, EchoesOnePromptInTestMode) {
    Config config;
    config.id = "local-agent";
    config.name = "Local agent";
    std::atomic_bool cancellation{false};
    CompletionClient client({.config = config});
    std::vector<std::string> deltas;

    CompletionRequest completion_request =
        client_request(1, "hello");
    completion_request.agent_id = "local-agent";
    const CompletionResult result = client.complete(
        completion_request,
        [&deltas](std::string text) {
            deltas.push_back(std::move(text));
        },
        cancellation);

    EXPECT_EQ(result.outcome, CompletionOutcome::completed);
    EXPECT_EQ(deltas, (std::vector<std::string>{"hello"}));
}

TEST(CompletionClient, RejectsAnAlreadyCancelledRequestBeforeDispatch) {
    Config config;
    config.id = "local-agent";
    config.name = "Local agent";
    std::atomic_bool cancellation{true};
    CompletionClient client({.config = config});
    bool received_delta = false;

    CompletionRequest completion_request =
        client_request(2, "do not dispatch");
    completion_request.agent_id = "local-agent";
    const CompletionResult result = client.complete(
        completion_request,
        [&received_delta](std::string) {
            received_delta = true;
        },
        cancellation);

    EXPECT_EQ(result.outcome, CompletionOutcome::cancelled);
    EXPECT_FALSE(received_delta);
}

TEST(CompletionClient, StreamsDeltasAndBuildsTheProviderRequest) {
    const std::string stream =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\" world\"}}]}\n\n"
        "data: [DONE]\n\n";
    MockHttpServer mock({
        http_response("text/event-stream", stream),
    });
    mock.start();

    Config config = network_config(mock.port());
    config.temperature = 0.25;
    config.reasoning_effort = "medium";
    config.api_key = "test-key";
    std::atomic_bool cancellation{false};
    CompletionClient client(
        {.config = config, .system_prompt = "Be concise."});
    std::vector<std::string> deltas;

    const CompletionResult result = client.complete(
        client_request(
            7,
            "Question",
            {
                make_human_entry(1, "Earlier question", 6),
                make_agent_entry(
                    2,
                    "assistant",
                    "Assistant",
                    "Earlier answer",
                    CompletionStatus::complete,
                    6),
            }),
        [&deltas](std::string text) {
            deltas.push_back(std::move(text));
        },
        cancellation);

    EXPECT_EQ(result.outcome, CompletionOutcome::completed);
    EXPECT_EQ(
        deltas,
        (std::vector<std::string>{"Hello", " world"}));
    mock.join();
    ASSERT_EQ(mock.requests().size(), 1U);
    EXPECT_NE(
        mock.requests().front().find(
            "Authorization: Bearer test-key"),
        std::string::npos);
    const Json body =
        Json::parse(request_body(mock.requests().front()));
    EXPECT_EQ(body["model"], "configured-model");
    EXPECT_TRUE(body["stream"]);
    EXPECT_DOUBLE_EQ(
        body["temperature"].get<double>(),
        0.25);
    EXPECT_EQ(body["reasoning_effort"], "medium");
    ASSERT_EQ(body["messages"].size(), 4U);
    EXPECT_EQ(body["messages"][0]["role"], "system");
    EXPECT_EQ(body["messages"][0]["content"], "Be concise.");
    EXPECT_EQ(body["messages"][1]["content"], "Earlier question");
    EXPECT_EQ(body["messages"][2]["role"], "assistant");
    EXPECT_EQ(body["messages"][2]["content"], "Earlier answer");
    EXPECT_EQ(body["messages"][3]["content"], "Question");
}

TEST(CompletionClient, ReportsMalformedStreamingProtocolDirectly) {
    const std::string stream =
        "data: not-json\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"Partial\"}}]}\n\n"
        "data: [DONE]\n\n";
    MockHttpServer mock({
        http_response("text/event-stream", stream),
    });
    mock.start();

    std::atomic_bool cancellation{false};
    CompletionClient client(
        {.config = network_config(mock.port())});
    std::string output;

    const CompletionResult result = client.complete(
        client_request(8, "Question"),
        [&output](std::string text) {
            output += text;
        },
        cancellation);

    EXPECT_EQ(
        result.outcome,
        CompletionOutcome::protocol_error);
    EXPECT_NE(
        result.message.find("malformed JSON"),
        std::string::npos);
    EXPECT_EQ(output, "Partial");
    mock.join();
}

TEST(CompletionClient, RejectsAStreamWithoutTheCompletionMarker) {
    const std::string stream =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Partial\"}}]}\n\n";
    MockHttpServer mock({
        http_response("text/event-stream", stream),
    });
    mock.start();

    std::atomic_bool cancellation{false};
    CompletionClient client(
        {.config = network_config(mock.port())});
    std::string output;

    const CompletionResult result = client.complete(
        client_request(9, "Question"),
        [&output](std::string text) {
            output += text;
        },
        cancellation);

    EXPECT_EQ(
        result.outcome,
        CompletionOutcome::protocol_error);
    EXPECT_NE(
        result.message.find("[DONE]"),
        std::string::npos);
    EXPECT_EQ(output, "Partial");
    mock.join();
}

TEST(CompletionClient, ReportsATruncatedResponseAsATransportError) {
    const std::string body =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Partial\"}}]}\n\n";
    const std::string response =
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        "Content-Length: " + std::to_string(body.size() + 20)
        + "\r\nConnection: close\r\n\r\n" + body;
    MockHttpServer mock({response});
    mock.start();

    std::atomic_bool cancellation{false};
    CompletionClient client(
        {.config = network_config(mock.port())});
    std::string output;

    const CompletionResult result = client.complete(
        client_request(10, "Question"),
        [&output](std::string text) {
            output += text;
        },
        cancellation);

    EXPECT_EQ(
        result.outcome,
        CompletionOutcome::transport_error);
    EXPECT_NE(
        result.message.find("HTTP request failed"),
        std::string::npos);
    EXPECT_EQ(output, "Partial");
    mock.join();
}

TEST(CompletionClient, CancelsAnActiveStreamingTransfer) {
    const std::string body =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Partial\"}}]}\n\n";
    const std::string partial_response =
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        "Content-Length: " + std::to_string(body.size() + 20)
        + "\r\nConnection: close\r\n\r\n" + body;
    MockHttpServer mock({partial_response}, true);
    mock.start();

    std::atomic_bool cancellation{false};
    CompletionClient client(
        {.config = network_config(mock.port())});
    std::string output;

    const CompletionResult result = client.complete(
        client_request(18, "Question"),
        [&output, &cancellation](std::string text) {
            output += text;
            cancellation.store(true, std::memory_order_release);
        },
        cancellation);

    EXPECT_EQ(result.outcome, CompletionOutcome::cancelled);
    EXPECT_EQ(output, "Partial");
    mock.join();
}

TEST(CompletionClient, ReportsAJsonErrorReturnedInsteadOfAStream) {
    const std::string body =
        R"({"error":{"message":"model unavailable"}})";
    MockHttpServer mock({
        http_response("application/json", body),
    });
    mock.start();

    std::atomic_bool cancellation{false};
    CompletionClient client(
        {.config = network_config(mock.port())});

    const CompletionResult result = client.complete(
        client_request(11, "Question"),
        [](std::string) {},
        cancellation);

    EXPECT_EQ(
        result.outcome,
        CompletionOutcome::protocol_error);
    EXPECT_NE(
        result.message.find("model unavailable"),
        std::string::npos);
    mock.join();
}

TEST(CompletionClient, RejectsACompletedStreamWithoutText) {
    const std::string stream =
        "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}\n\n"
        "data: [DONE]\n\n";
    MockHttpServer mock({
        http_response("text/event-stream", stream),
    });
    mock.start();

    std::atomic_bool cancellation{false};
    CompletionClient client(
        {.config = network_config(mock.port())});

    const CompletionResult result = client.complete(
        client_request(12, "Question"),
        [](std::string) {},
        cancellation);

    EXPECT_EQ(
        result.outcome,
        CompletionOutcome::protocol_error);
    EXPECT_NE(
        result.message.find("without text content"),
        std::string::npos);
    mock.join();
}

TEST(CompletionClient, IgnoresDataAfterTheCompletionMarker) {
    const std::string stream =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Complete\"}}]}\n\n"
        "data: [DONE]\n"
        "data: not-json\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\" ignored\"}}]}\n\n";
    MockHttpServer mock({
        http_response("text/event-stream", stream),
    });
    mock.start();

    std::atomic_bool cancellation{false};
    CompletionClient client(
        {.config = network_config(mock.port())});
    std::string output;

    const CompletionResult result = client.complete(
        client_request(13, "Question"),
        [&output](std::string text) {
            output += text;
        },
        cancellation);

    EXPECT_EQ(result.outcome, CompletionOutcome::completed);
    EXPECT_EQ(output, "Complete");
    mock.join();
}

TEST(CompletionClient, ParsesNonStreamingResponses) {
    const std::string body =
        R"({"choices":[{"message":{"content":"Complete answer"}}]})";
    MockHttpServer mock({
        http_response("application/json", body),
    });
    mock.start();

    std::atomic_bool cancellation{false};
    CompletionClient client(
        {.config = network_config(mock.port(), false)});
    std::vector<std::string> deltas;

    const CompletionResult result = client.complete(
        client_request(14, "Question"),
        [&deltas](std::string text) {
            deltas.push_back(std::move(text));
        },
        cancellation);

    EXPECT_EQ(result.outcome, CompletionOutcome::completed);
    EXPECT_EQ(
        deltas,
        (std::vector<std::string>{"Complete answer"}));
    EXPECT_FALSE(client.info().streaming);
    EXPECT_NE(
        client.info().api.find("/v1/chat/completions"),
        std::string::npos);
    mock.join();
}

TEST(CompletionClient, RejectsANonStreamingResponseWithoutText) {
    const std::string body =
        R"({"choices":[{"message":{"content":""}}]})";
    MockHttpServer mock({
        http_response("application/json", body),
    });
    mock.start();

    std::atomic_bool cancellation{false};
    CompletionClient client(
        {.config = network_config(mock.port(), false)});

    const CompletionResult result = client.complete(
        client_request(15, "Question"),
        [](std::string) {},
        cancellation);

    EXPECT_EQ(
        result.outcome,
        CompletionOutcome::protocol_error);
    EXPECT_NE(
        result.message.find("without text content"),
        std::string::npos);
    mock.join();
}

TEST(CompletionClient, ReportsNonSuccessfulHttpStatus) {
    const std::string body =
        R"({"error":{"message":"request rejected"}})";
    MockHttpServer mock({
        status_response(
            503,
            "Service Unavailable",
            "application/json",
            body),
    });
    mock.start();

    std::atomic_bool cancellation{false};
    CompletionClient client(
        {.config = network_config(mock.port())});

    const CompletionResult result = client.complete(
        client_request(16, "Question"),
        [](std::string) {},
        cancellation);

    EXPECT_EQ(
        result.outcome,
        CompletionOutcome::protocol_error);
    EXPECT_NE(result.message.find("HTTP 503"), std::string::npos);
    EXPECT_NE(
        result.message.find("request rejected"),
        std::string::npos);
    mock.join();
}

TEST(CompletionClient, DiscoversItsModelBeforeTheFirstCompletion) {
    const std::string models =
        R"({"data":[{"id":"discovered-model"}]})";
    const std::string completion =
        R"({"choices":[{"message":{"content":"Answer"}}]})";
    MockHttpServer mock({
        http_response("application/json", models),
        http_response("application/json", completion),
    });
    mock.start();

    Config config = network_config(mock.port(), false);
    config.model.clear();
    std::atomic_bool cancellation{false};
    CompletionClient client({.config = config});
    EXPECT_EQ(client.info().model, "discovered-model");

    const CompletionResult result = client.complete(
        client_request(17, "Question"),
        [](std::string) {},
        cancellation);

    EXPECT_EQ(result.outcome, CompletionOutcome::completed);
    mock.join();
    ASSERT_EQ(mock.requests().size(), 2U);
    EXPECT_TRUE(
        mock.requests()[0].starts_with(
            "GET /v1/models HTTP/1.1"));
    const Json body =
        Json::parse(request_body(mock.requests()[1]));
    EXPECT_EQ(body["model"], "discovered-model");
}

} // namespace
} // namespace cha
