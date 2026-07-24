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
    Conversation& conversation,
    RequestId request_id,
    std::string prompt,
    std::vector<ConversationEntry> history = {}) {
    for (ConversationEntry& entry : history) {
        conversation.add_entry(std::move(entry));
    }
    CompletionRequest request{
        .request_id = request_id,
        .prompt = make_human_entry(
            1000 + request_id,
            "assistant",
            "Assistant",
            std::move(prompt),
            request_id),
    };
    conversation.add_entry(request.prompt);
    return request;
}

CompletionResult complete(
    CompletionClient& client,
    const CompletionRequest& request,
    const Conversation& conversation,
    const CompletionDeltaSink& on_delta,
    const std::atomic_bool& cancellation) {
    RequestPayload payload;
    {
        ConversationReadView view = conversation.read();
        payload = client.prepare(request, view);
    }
    return client.perform(std::move(payload), on_delta, cancellation);
}

Config network_config(int port, bool stream = true) {
    Config config;
    config.id = "assistant";
    config.name = "Assistant";
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
    return "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason)
        + "\r\nContent-Type: " + std::string(content_type)
        + "\r\nContent-Length: " + std::to_string(body.size())
        + "\r\nConnection: close\r\n\r\n" + body;
}

TEST(CompletionClient, EchoesOnePromptInTestMode) {
    Config config;
    config.id = "assistant";
    config.name = "Assistant";
    std::atomic_bool cancellation{false};
    CompletionClient client({.config = config});
    Conversation conversation;
    CompletionRequest request = client_request(conversation, 1, "hello");
    std::vector<std::string> deltas;

    const CompletionResult result = complete(
        client, request, conversation,
        [&deltas](CompletionDelta delta) {
            EXPECT_EQ(delta.kind, CompletionDeltaKind::answer);
            deltas.push_back(std::move(delta.text));
        },
        cancellation);

    EXPECT_EQ(result.outcome, CompletionOutcome::completed);
    EXPECT_EQ(deltas, (std::vector<std::string>{"hello"}));
}

TEST(CompletionClient, RejectsAnAlreadyCancelledRequestBeforeDispatch) {
    Config config;
    config.id = "assistant";
    config.name = "Assistant";
    std::atomic_bool cancellation{true};
    CompletionClient client({.config = config});
    Conversation conversation;
    CompletionRequest request = client_request(conversation, 2, "do not dispatch");
    bool received_delta = false;

    const CompletionResult result = complete(
        client, request, conversation,
        [&received_delta](CompletionDelta) { received_delta = true; },
        cancellation);

    EXPECT_EQ(result.outcome, CompletionOutcome::cancelled);
    EXPECT_FALSE(received_delta);
}

TEST(CompletionClient, StreamsDeltasAndBuildsTheProviderRequest) {
    const std::string stream =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\" world\"}}]}\n\n"
        "data: [DONE]\n\n";
    MockHttpServer mock({http_response("text/event-stream", stream)});
    mock.start();

    Config config = network_config(mock.port());
    config.temperature = 0.25;
    config.reasoning_effort = "medium";
    config.api_key = "test-key";
    std::atomic_bool cancellation{false};
    CompletionClient client({.config = config, .system_prompt = "Be concise."});
    Conversation conversation;
    const CompletionRequest request = client_request(
        conversation, 7, "Question", {
            make_human_entry(1, "assistant", "Assistant", "Earlier question", 6),
            make_agent_entry(2, "assistant", "Assistant", "Earlier answer", CompletionStatus::complete, 6),
            make_notice_entry(3, "hidden"),
            make_agent_entry(4, "other", "Other", "Other answer", CompletionStatus::complete, 6),
        });
    std::vector<std::string> deltas;

    const CompletionResult result = complete(
        client, request, conversation,
        [&deltas](CompletionDelta delta) {
            deltas.push_back(std::move(delta.text));
        },
        cancellation);

    EXPECT_EQ(result.outcome, CompletionOutcome::completed);
    EXPECT_EQ(deltas, (std::vector<std::string>{"Hello", " world"}));
    mock.join();
    ASSERT_EQ(mock.requests().size(), 1U);
    EXPECT_NE(mock.requests().front().find("Authorization: Bearer test-key"), std::string::npos);
    const Json body = Json::parse(request_body(mock.requests().front()));
    EXPECT_EQ(body["model"], "configured-model");
    EXPECT_TRUE(body["stream"]);
    EXPECT_DOUBLE_EQ(body["temperature"], 0.25);
    EXPECT_EQ(body["reasoning_effort"], "medium");
    EXPECT_EQ(body["messages"], Json::array({
        {{"role", "system"}, {"content", "Be concise."}},
        {{"role", "user"}, {"content", "User: Earlier question"}},
        {{"role", "assistant"}, {"content", "Earlier answer"}},
        {{"role", "user"}, {"content", "Other: Other answer\n\nUser: Question"}},
    }));
}

TEST(CompletionClient, OmitsEmptySystemPromptAndEscapesTranscriptContent) {
    MockHttpServer mock({http_response(
        "application/json", R"({"choices":[{"message":{"content":"Answer"}}]})")});
    mock.start();
    std::atomic_bool cancellation{false};
    CompletionClient client({.config = network_config(mock.port(), false)});
    Conversation conversation;
    const std::string prompt = "quote \" and newline\n and backslash \\";
    const CompletionRequest request = client_request(conversation, 19, prompt);

    const CompletionResult result = complete(
        client, request, conversation, [](CompletionDelta) {}, cancellation);

    EXPECT_EQ(result.outcome, CompletionOutcome::completed);
    mock.join();
    const Json body = Json::parse(request_body(mock.requests().front()));
    ASSERT_EQ(body["messages"].size(), 1U);
    EXPECT_EQ(body["messages"][0]["role"], "user");
    EXPECT_EQ(body["messages"][0]["content"], prompt);
}

TEST(CompletionClient, RejectsInvalidUtf8WhenPreparingRequest) {
    CompletionClient client({.config = network_config(1, false)});
    Conversation conversation;
    const CompletionRequest request = client_request(
        conversation,
        20,
        std::string("\xc0\x80", 2));

    std::string message;
    try {
        const ConversationReadView view = conversation.read();
        (void)client.prepare(request, view);
    } catch (const std::runtime_error& error) {
        message = error.what();
    }

    EXPECT_EQ(message, "Completion request contains invalid UTF-8");
}

TEST(CompletionClient, HandlesNonStreamingProviderResponse) {
    MockHttpServer mock({http_response(
        "application/json", R"({"choices":[{"message":{"content":"Answer"}}]})")});
    mock.start();
    std::atomic_bool cancellation{false};
    CompletionClient client({.config = network_config(mock.port(), false)});
    Conversation conversation;
    const CompletionRequest request = client_request(conversation, 8, "Question");
    std::string output;

    const CompletionResult result = complete(
        client, request, conversation,
        [&output](CompletionDelta delta) { output += delta.text; }, cancellation);

    EXPECT_EQ(result.outcome, CompletionOutcome::completed);
    EXPECT_EQ(output, "Answer");
    mock.join();
}

TEST(CompletionClient, ReportsProviderHttpFailure) {
    MockHttpServer mock({status_response(
        503, "Service Unavailable", "application/json",
        R"({"error":{"message":"request rejected"}})")});
    mock.start();
    std::atomic_bool cancellation{false};
    CompletionClient client({.config = network_config(mock.port())});
    Conversation conversation;
    const CompletionRequest request = client_request(conversation, 9, "Question");

    const CompletionResult result = complete(
        client, request, conversation, [](CompletionDelta) {}, cancellation);

    EXPECT_EQ(result.outcome, CompletionOutcome::protocol_error);
    EXPECT_NE(result.message.find("HTTP 503"), std::string::npos);
    EXPECT_NE(result.message.find("request rejected"), std::string::npos);
    mock.join();
}

TEST(CompletionClient, ReportsMalformedStreamingProtocolDirectly) {
    const std::string stream =
        "data: not-json\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"Partial\"}}]}\n\n"
        "data: [DONE]\n\n";
    MockHttpServer mock({http_response("text/event-stream", stream)});
    mock.start();
    std::atomic_bool cancellation{false};
    CompletionClient client({.config = network_config(mock.port())});
    Conversation conversation;
    const CompletionRequest request = client_request(conversation, 10, "Question");
    std::string output;

    const CompletionResult result = complete(
        client, request, conversation,
        [&output](CompletionDelta delta) { output += delta.text; }, cancellation);

    EXPECT_EQ(result.outcome, CompletionOutcome::protocol_error);
    EXPECT_NE(result.message.find("malformed JSON"), std::string::npos);
    EXPECT_EQ(output, "Partial");
    mock.join();
}

TEST(CompletionClient, RejectsAStreamWithoutTheCompletionMarker) {
    const std::string stream = "data: {\"choices\":[{\"delta\":{\"content\":\"Partial\"}}]}\n\n";
    MockHttpServer mock({http_response("text/event-stream", stream)});
    mock.start();
    std::atomic_bool cancellation{false};
    CompletionClient client({.config = network_config(mock.port())});
    Conversation conversation;
    const CompletionRequest request = client_request(conversation, 11, "Question");
    std::string output;

    const CompletionResult result = complete(
        client, request, conversation,
        [&output](CompletionDelta delta) { output += delta.text; }, cancellation);

    EXPECT_EQ(result.outcome, CompletionOutcome::protocol_error);
    EXPECT_NE(result.message.find("[DONE]"), std::string::npos);
    EXPECT_EQ(output, "Partial");
    mock.join();
}

TEST(CompletionClient, ReportsATruncatedResponseAsATransportError) {
    const std::string body = "data: {\"choices\":[{\"delta\":{\"content\":\"Partial\"}}]}\n\n";
    const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        "Content-Length: " + std::to_string(body.size() + 20)
        + "\r\nConnection: close\r\n\r\n" + body;
    MockHttpServer mock({response});
    mock.start();
    std::atomic_bool cancellation{false};
    CompletionClient client({.config = network_config(mock.port())});
    Conversation conversation;
    const CompletionRequest request = client_request(conversation, 12, "Question");
    std::string output;

    const CompletionResult result = complete(
        client, request, conversation,
        [&output](CompletionDelta delta) { output += delta.text; }, cancellation);

    EXPECT_EQ(result.outcome, CompletionOutcome::transport_error);
    EXPECT_NE(result.message.find("HTTP request failed"), std::string::npos);
    EXPECT_EQ(output, "Partial");
    mock.join();
}

TEST(CompletionClient, CancelsAnActiveStreamingTransfer) {
    const std::string body = "data: {\"choices\":[{\"delta\":{\"content\":\"Partial\"}}]}\n\n";
    const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        "Content-Length: " + std::to_string(body.size() + 20)
        + "\r\nConnection: close\r\n\r\n" + body;
    MockHttpServer mock({response}, true);
    mock.start();
    std::atomic_bool cancellation{false};
    CompletionClient client({.config = network_config(mock.port())});
    Conversation conversation;
    const CompletionRequest request = client_request(conversation, 13, "Question");
    std::string output;

    const CompletionResult result = complete(
        client, request, conversation,
        [&output, &cancellation](CompletionDelta delta) {
            output += delta.text;
            cancellation.store(true, std::memory_order_release);
        }, cancellation);

    EXPECT_EQ(result.outcome, CompletionOutcome::cancelled);
    EXPECT_EQ(output, "Partial");
    mock.join();
}

TEST(CompletionClient, ReportsAJsonErrorReturnedInsteadOfAStream) {
    MockHttpServer mock({http_response(
        "application/json", R"({"error":{"message":"model unavailable"}})")});
    mock.start();
    std::atomic_bool cancellation{false};
    CompletionClient client({.config = network_config(mock.port())});
    Conversation conversation;
    const CompletionRequest request = client_request(conversation, 14, "Question");

    const CompletionResult result = complete(
        client, request, conversation, [](CompletionDelta) {}, cancellation);

    EXPECT_EQ(result.outcome, CompletionOutcome::protocol_error);
    EXPECT_EQ(result.message.find("model unavailable"), std::string::npos);
    EXPECT_NE(result.message.find("HTTP 200"), std::string::npos);
    EXPECT_NE(result.message.find("application/json"), std::string::npos);
    mock.join();
}

TEST(CompletionClient, RejectsACompletedStreamWithoutText) {
    const std::string stream =
        "data: {\"choices\":[{\"delta\":{\"role\":\"assistant\"}}]}\n\n"
        "data: [DONE]\n\n";
    MockHttpServer mock({http_response("text/event-stream", stream)});
    mock.start();
    std::atomic_bool cancellation{false};
    CompletionClient client({.config = network_config(mock.port())});
    Conversation conversation;
    const CompletionRequest request = client_request(conversation, 15, "Question");

    const CompletionResult result = complete(
        client, request, conversation, [](CompletionDelta) {}, cancellation);

    EXPECT_EQ(result.outcome, CompletionOutcome::protocol_error);
    EXPECT_NE(result.message.find("without answer content"), std::string::npos);
    mock.join();
}

TEST(CompletionClient, IgnoresDataAfterTheCompletionMarker) {
    const std::string stream =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Complete\"}}]}\n\n"
        "data: [DONE]\n"
        "data: not-json\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\" ignored\"}}]}\n\n";
    MockHttpServer mock({http_response("text/event-stream", stream)});
    mock.start();
    std::atomic_bool cancellation{false};
    CompletionClient client({.config = network_config(mock.port())});
    Conversation conversation;
    const CompletionRequest request = client_request(conversation, 16, "Question");
    std::string output;

    const CompletionResult result = complete(
        client, request, conversation,
        [&output](CompletionDelta delta) { output += delta.text; }, cancellation);

    EXPECT_EQ(result.outcome, CompletionOutcome::completed);
    EXPECT_EQ(output, "Complete");
    mock.join();
}

TEST(CompletionClient, RejectsANonStreamingResponseWithoutText) {
    MockHttpServer mock({http_response(
        "application/json", R"({"choices":[{"message":{"content":""}}]})")});
    mock.start();
    std::atomic_bool cancellation{false};
    CompletionClient client({.config = network_config(mock.port(), false)});
    Conversation conversation;
    const CompletionRequest request = client_request(conversation, 17, "Question");

    const CompletionResult result = complete(
        client, request, conversation, [](CompletionDelta) {}, cancellation);

    EXPECT_EQ(result.outcome, CompletionOutcome::protocol_error);
    EXPECT_NE(result.message.find("without answer content"), std::string::npos);
    mock.join();
}

TEST(CompletionClient, DiscoversItsModelBeforeTheFirstCompletion) {
    MockHttpServer mock({
        http_response("application/json", R"({"data":[{"id":"discovered-model"}]})"),
        http_response("application/json", R"({"choices":[{"message":{"content":"Answer"}}]})"),
    });
    mock.start();
    Config config = network_config(mock.port(), false);
    config.model.clear();
    std::atomic_bool cancellation{false};
    CompletionClient client({.config = config});
    EXPECT_EQ(client.info().model, "discovered-model");
    Conversation conversation;
    const CompletionRequest request = client_request(conversation, 18, "Question");

    const CompletionResult result = complete(
        client, request, conversation, [](CompletionDelta) {}, cancellation);

    EXPECT_EQ(result.outcome, CompletionOutcome::completed);
    mock.join();
    ASSERT_EQ(mock.requests().size(), 2U);
    EXPECT_TRUE(mock.requests()[0].starts_with("GET /v1/models HTTP/1.1"));
    EXPECT_EQ(Json::parse(request_body(mock.requests()[1]))["model"], "discovered-model");
}

TEST(CompletionClient, StreamsStructuredReasoningBeforeAnswerWithAutoPrecedence) {
    const std::string stream =
        "data: {\"choices\":[{\"delta\":{"
        "\"reasoning_content\":\"Primary\","
        "\"reasoning\":\"Ignored\","
        "\"content\":\"Answer\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"reasoning\":\" late\"}}]}\n\n"
        "data: [DONE]\n\n";
    MockHttpServer mock({http_response("text/event-stream", stream)});
    mock.start();
    CompletionClient client({.config = network_config(mock.port())});
    Conversation conversation;
    const CompletionRequest request =
        client_request(conversation, 21, "Question");
    std::atomic_bool cancellation{false};
    std::vector<CompletionDelta> deltas;

    const CompletionResult result = complete(
        client,
        request,
        conversation,
        [&deltas](CompletionDelta delta) {
            deltas.push_back(std::move(delta));
        },
        cancellation);

    EXPECT_EQ(result.outcome, CompletionOutcome::completed);
    ASSERT_EQ(deltas.size(), 3U);
    EXPECT_EQ(deltas[0].kind, CompletionDeltaKind::reasoning);
    EXPECT_EQ(deltas[0].text, "Primary");
    EXPECT_EQ(deltas[1].kind, CompletionDeltaKind::answer);
    EXPECT_EQ(deltas[1].text, "Answer");
    EXPECT_EQ(deltas[2].kind, CompletionDeltaKind::reasoning);
    EXPECT_EQ(deltas[2].text, " late");
    mock.join();
}

TEST(CompletionClient, AppliesExplicitReasoningFormatStrictly) {
    const std::string stream =
        "data: {\"choices\":[{\"delta\":{"
        "\"reasoning_content\":{\"bad\":true},"
        "\"content\":\"Answer\"}}]}\n\n"
        "data: [DONE]\n\n";
    MockHttpServer mock({http_response("text/event-stream", stream)});
    mock.start();
    Config config = network_config(mock.port());
    config.reasoning_format = ReasoningFormat::reasoning_content;
    CompletionClient client({.config = config});
    Conversation conversation;
    const CompletionRequest request =
        client_request(conversation, 22, "Question");
    std::atomic_bool cancellation{false};
    std::string answer;

    const CompletionResult result = complete(
        client,
        request,
        conversation,
        [&answer](CompletionDelta delta) {
            if (delta.kind == CompletionDeltaKind::answer) {
                answer += delta.text;
            }
        },
        cancellation);

    EXPECT_EQ(result.outcome, CompletionOutcome::protocol_error);
    EXPECT_EQ(answer, "Answer");
    EXPECT_NE(result.message.find("not a string or null"), std::string::npos);
    EXPECT_NE(result.message.find("HTTP 200"), std::string::npos);
    EXPECT_EQ(result.message.find("{\"bad\""), std::string::npos);
    mock.join();
}

TEST(CompletionClient, ParsesNonStreamingReasoningAndRequiresAnAnswer) {
    MockHttpServer mock({
        http_response(
            "application/json",
            R"({"choices":[{"message":{"reasoning":"Think","content":"Answer"}}]})"),
        http_response(
            "application/json",
            R"({"choices":[{"message":{"reasoning":"Only","content":""}}]})"),
    });
    mock.start();
    Config config = network_config(mock.port(), false);
    config.reasoning_format = ReasoningFormat::reasoning;
    CompletionClient client({.config = config});
    std::atomic_bool cancellation{false};

    Conversation first_conversation;
    const CompletionRequest first =
        client_request(first_conversation, 23, "Question");
    std::vector<CompletionDelta> deltas;
    const CompletionResult success = complete(
        client,
        first,
        first_conversation,
        [&deltas](CompletionDelta delta) {
            deltas.push_back(std::move(delta));
        },
        cancellation);
    EXPECT_EQ(success.outcome, CompletionOutcome::completed);
    ASSERT_EQ(deltas.size(), 2U);
    EXPECT_EQ(deltas[0].kind, CompletionDeltaKind::reasoning);
    EXPECT_EQ(deltas[1].kind, CompletionDeltaKind::answer);

    Conversation second_conversation;
    const CompletionRequest second =
        client_request(second_conversation, 24, "Question");
    const CompletionResult failure = complete(
        client,
        second,
        second_conversation,
        [](CompletionDelta) {},
        cancellation);
    EXPECT_EQ(failure.outcome, CompletionOutcome::protocol_error);
    EXPECT_NE(
        failure.message.find("without answer content"),
        std::string::npos);
    mock.join();
}

TEST(CompletionClient, ReasoningOnlyStreamIsNotACompletedAnswer) {
    const std::string stream =
        "data: {\"choices\":[{\"delta\":{"
        "\"reasoning_content\":\"PRIVATE_ONLY_REASONING\"}}]}\n\n"
        "data: [DONE]\n\n";
    MockHttpServer mock({http_response("text/event-stream", stream)});
    mock.start();
    CompletionClient client({.config = network_config(mock.port())});
    Conversation conversation;
    const CompletionRequest request =
        client_request(conversation, 25, "Question");
    std::atomic_bool cancellation{false};
    std::vector<CompletionDelta> deltas;

    const CompletionResult result = complete(
        client,
        request,
        conversation,
        [&deltas](CompletionDelta delta) {
            deltas.push_back(std::move(delta));
        },
        cancellation);

    EXPECT_EQ(result.outcome, CompletionOutcome::protocol_error);
    EXPECT_NE(
        result.message.find("without answer content"),
        std::string::npos);
    EXPECT_EQ(
        result.message.find("PRIVATE_ONLY_REASONING"),
        std::string::npos);
    ASSERT_EQ(deltas.size(), 1U);
    EXPECT_EQ(deltas.front().kind, CompletionDeltaKind::reasoning);
    mock.join();
}

} // namespace
} // namespace cha
