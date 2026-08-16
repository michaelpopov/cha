#include "agents/provider_client.h"
#include "chat/transcript.h"
#include "support/mock_http_server.h"
#include "support/test_transcript.h"
#include "util/logging.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <array>
#include <barrier>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace cha {
namespace {

using Json = nlohmann::json;

GenerationRequest client_request(
    Transcript& transcript,
    RequestId request_id,
    std::string prompt,
    std::vector<TranscriptEntry> history = {}) {
    for (TranscriptEntry& entry : history) {
        transcript.add_entry(std::move(entry));
    }
    GenerationRequest input{
        .history = std::make_shared<const ModelHistory>(
            transcript.model_history()),
        .run = {
            .request_id = request_id,
            .target = {"assistant", "Assistant"},
            .author = {"human", "You"},
            .prompt_text = std::move(prompt),
        },
    };
    transcript.add_entry(test::human_entry(
        1000 + request_id, {"human", "You"}, {"assistant", "Assistant"},
        input.run.prompt_text, request_id));
    return input;
}

GenerationResult complete(
    ProviderClient& client,
    const GenerationRequest& input,
    const Transcript&,
    const GenerationDeltaSink& on_delta,
    const std::atomic_bool& cancellation) {
    RequestPayload payload = client.prepare(input);
    return client.perform(std::move(payload), on_delta, cancellation);
}

CharacterDefinition test_definition(
    std::optional<std::string> description = std::nullopt) {
    return {
        .character = {
            .id = "assistant",
            .display_name = "Assistant",
            .description = std::move(description),
        },
    };
}

CharacterDefinition network_definition(int port, bool stream = true) {
    CharacterDefinition definition = test_definition();
    definition.backend.host = "127.0.0.1";
    definition.backend.port = port;
    definition.backend.mode = Mode::net;
    definition.backend.model = "configured-model";
    definition.backend.stream = stream;
    definition.backend.api = ProviderApi::chat_completions;
    definition.backend.web_search = WebSearchMode::off;
    return definition;
}

CharacterDefinition responses_network_definition(int port, bool stream = true) {
    CharacterDefinition definition = network_definition(port, stream);
    definition.backend.api = ProviderApi::responses;
    definition.backend.web_search = WebSearchMode::automatic;
    return definition;
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

class DiagnosticLogFile {
public:
    DiagnosticLogFile()
        : directory_(std::filesystem::temp_directory_path()
            / ("cha_generation_logging_"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()))),
          path_(directory_ / "cha.log") {
        shutdown_diagnostic_logging();
        initialize_diagnostic_logging(path_, "info");
    }

    ~DiagnosticLogFile() {
        shutdown_diagnostic_logging();
        std::filesystem::remove_all(directory_);
    }

    std::string contents() const {
        std::ifstream file(path_);
        return {
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
    }

private:
    std::filesystem::path directory_;
    std::filesystem::path path_;
};

TEST(ProviderClient, EchoesOnePromptInTestMode) {
    std::atomic_bool cancellation{false};
    ProviderClient client(test_definition("Helpful character"));
    Transcript transcript;
    GenerationRequest request = client_request(transcript, 1, "hello");
    std::vector<std::string> deltas;

    const GenerationResult result = complete(
        client, request, transcript,
        [&deltas](GenerationDelta delta) {
            EXPECT_EQ(delta.kind, GenerationDeltaKind::answer);
            deltas.push_back(std::move(delta.text));
        },
        cancellation);

    EXPECT_EQ(result.outcome, GenerationOutcome::completed);
    EXPECT_EQ(deltas, (std::vector<std::string>{"hello"}));
    EXPECT_EQ(client.info().character.description, "Helpful character");
}

TEST(ProviderClient, ConstructsSessionLocalNetworkClientsConcurrently) {
    // This file is POSIX-only. The cross-platform construction coverage is the
    // ConcurrentControllers counterpart; this duplicate is order-dependent
    // smoke coverage because an earlier curl persona may already have initialized
    // curl_global()'s magic static. C++ guarantees thread-safe initialization.
    std::barrier start(3);
    std::array<ModelBackendInfo, 2> infos;
    std::array<std::exception_ptr, 2> failures;
    std::array<std::thread, 2> threads;

    for (std::size_t index = 0; index < threads.size(); ++index) {
        threads[index] = std::thread([&, index] {
            try {
                CharacterDefinition definition = network_definition(9);
                definition.character.id = "assistant-" + std::to_string(index);
                definition.character.display_name = "Assistant " + std::to_string(index);
                start.arrive_and_wait();
                ProviderClient client(std::move(definition));
                infos[index] = client.info();
            } catch (...) {
                failures[index] = std::current_exception();
            }
        });
    }
    start.arrive_and_wait();
    for (std::thread& thread : threads) {
        thread.join();
    }

    for (const std::exception_ptr& failure : failures) {
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
    EXPECT_EQ(infos[0].character.id, "assistant-0");
    EXPECT_EQ(infos[1].character.id, "assistant-1");
}

TEST(ProviderClient, RejectsAnAlreadyCancelledRequestBeforeDispatch) {
    std::atomic_bool cancellation{true};
    ProviderClient client(test_definition());
    Transcript transcript;
    GenerationRequest request = client_request(transcript, 2, "do not dispatch");
    bool received_delta = false;

    const GenerationResult result = complete(
        client, request, transcript,
        [&received_delta](GenerationDelta) { received_delta = true; },
        cancellation);

    EXPECT_EQ(result.outcome, GenerationOutcome::cancelled);
    EXPECT_FALSE(received_delta);
}

TEST(ProviderClient, StreamsDeltasAndBuildsTheProviderRequest) {
    const std::string stream =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\" world\"}}]}\n\n"
        "data: [DONE]\n\n";
    MockHttpServer mock({http_response("text/event-stream", stream)});
    mock.start();

    CharacterDefinition definition = network_definition(mock.port());
    definition.backend.temperature = 0.25;
    definition.backend.reasoning_effort = "medium";
    definition.backend.api_key = "test-key";
    definition.system_prompt = "Be concise.";
    std::atomic_bool cancellation{false};
    ProviderClient client(std::move(definition));
    Transcript transcript;
    const GenerationRequest request = client_request(
        transcript, 7, "Question", {
            test::human_entry(1, {"human", "You"}, {"assistant", "Assistant"}, "Earlier question", 6),
            make_character_entry(2, "assistant", "Assistant", "Earlier answer", EntryStatus::complete, 6),
            make_notice_entry(3, "hidden"),
            make_character_entry(4, "other", "Other", "Other answer", EntryStatus::complete, 6),
        });
    std::vector<std::string> deltas;

    const GenerationResult result = complete(
        client, request, transcript,
        [&deltas](GenerationDelta delta) {
            deltas.push_back(std::move(delta.text));
        },
        cancellation);

    EXPECT_EQ(result.outcome, GenerationOutcome::completed);
    EXPECT_EQ(deltas, (std::vector<std::string>{"Hello", " world"}));
    mock.join();
    ASSERT_EQ(mock.requests().size(), 1U);
    EXPECT_TRUE(mock.requests().front().starts_with("POST /v1/chat/completions HTTP/1.1"));
    EXPECT_NE(mock.requests().front().find("Authorization: Bearer test-key"), std::string::npos);
    const Json body = Json::parse(request_body(mock.requests().front()));
    EXPECT_EQ(body["model"], "configured-model");
    EXPECT_TRUE(body["stream"]);
    EXPECT_TRUE(body["stream_options"]["include_usage"]);
    EXPECT_DOUBLE_EQ(body["temperature"], 0.25);
    EXPECT_EQ(body["reasoning_effort"], "medium");
    EXPECT_EQ(body["messages"], Json::array({
        {{"role", "system"}, {"content", "Be concise."}},
        {{"role", "user"}, {"content", "from You:\nEarlier question"}},
        {{"role", "assistant"}, {"content", "Earlier answer"}},
        {{"role", "user"},
         {"content",
          "Shared chat history (JSONL):\n"
          R"({"kind":"character","speaker":"Other","text":"Other answer"})"}},
        {{"role", "user"}, {"content", "from You:\nQuestion"}},
    }));
    EXPECT_TRUE(client.info().api.ends_with("/v1/chat/completions"));
}

TEST(ProviderClient, OmitsEmptySystemPromptAndEscapesTranscriptContent) {
    MockHttpServer mock({http_response(
        "application/json", R"({"choices":[{"message":{"content":"Answer"}}]})")});
    mock.start();
    std::atomic_bool cancellation{false};
    ProviderClient client(network_definition(mock.port(), false));
    Transcript transcript;
    const std::string prompt = "quote \" and newline\n and backslash \\";
    const GenerationRequest request = client_request(transcript, 19, prompt);

    const GenerationResult result = complete(
        client, request, transcript, [](GenerationDelta) {}, cancellation);

    EXPECT_EQ(result.outcome, GenerationOutcome::completed);
    mock.join();
    const Json body = Json::parse(request_body(mock.requests().front()));
    EXPECT_DOUBLE_EQ(body["temperature"], 1.0);
    ASSERT_EQ(body["messages"].size(), 1U);
    EXPECT_EQ(body["messages"][0]["role"], "user");
    EXPECT_EQ(body["messages"][0]["content"], "from You:\n" + prompt);
}

TEST(ProviderClient, RejectsInvalidUtf8WhenPreparingRequest) {
    ProviderClient client(network_definition(1, false));
    Transcript transcript;
    const GenerationRequest request = client_request(
        transcript,
        20,
        std::string("\xc0\x80", 2));

    std::string message;
    try {
        (void)client.prepare(request);
    } catch (const std::runtime_error& error) {
        message = error.what();
    }

    EXPECT_EQ(message, "Model request contains invalid UTF-8");
}

TEST(ProviderClient, HandlesNonStreamingProviderResponse) {
    MockHttpServer mock({http_response(
        "application/json", R"({"choices":[{"message":{"content":"Answer"}}]})")});
    mock.start();
    std::atomic_bool cancellation{false};
    ProviderClient client(network_definition(mock.port(), false));
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 8, "Question");
    std::string output;

    const GenerationResult result = complete(
        client, request, transcript,
        [&output](GenerationDelta delta) { output += delta.text; }, cancellation);

    EXPECT_EQ(result.outcome, GenerationOutcome::completed);
    EXPECT_EQ(output, "Answer");
    mock.join();
}

TEST(ProviderClient, LogsTransportMetadataWithoutPayloads) {
    const std::string response_body =
        R"({"choices":[{"message":{"content":"private response"}}],"usage":{"prompt_tokens":12,"completion_tokens":5}})";
    MockHttpServer mock({http_response("application/json", response_body)});
    mock.start();
    DiagnosticLogFile log;
    std::atomic_bool cancellation{false};
    ProviderClient client(network_definition(mock.port(), false));
    Transcript transcript;
    const GenerationRequest request =
        client_request(transcript, 77, "private prompt");

    const GenerationResult result = complete(
        client,
        request,
        transcript,
        [](GenerationDelta) {},
        cancellation);
    mock.join();

    EXPECT_EQ(result.outcome, GenerationOutcome::completed);
    const std::string output = log.contents();
    EXPECT_NE(output.find("HTTP request completed"), std::string::npos);
    EXPECT_NE(output.find("status=200"), std::string::npos);
    EXPECT_NE(
        output.find("content_type=application/json"),
        std::string::npos);
    EXPECT_NE(output.find("request_bytes="), std::string::npos);
    EXPECT_NE(output.find("response_bytes="), std::string::npos);
    EXPECT_NE(output.find("duration_ms="), std::string::npos);
    EXPECT_NE(output.find("input_tokens=12"), std::string::npos);
    EXPECT_NE(output.find("output_tokens=5"), std::string::npos);
    EXPECT_EQ(output.find("private prompt"), std::string::npos);
    EXPECT_EQ(output.find("private response"), std::string::npos);
}

TEST(ProviderClient, ReportsProviderHttpFailure) {
    MockHttpServer mock({status_response(
        503, "Service Unavailable", "application/json",
        R"({"error":{"message":"request rejected"}})")});
    mock.start();
    std::atomic_bool cancellation{false};
    ProviderClient client(network_definition(mock.port()));
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 9, "Question");

    const GenerationResult result = complete(
        client, request, transcript, [](GenerationDelta) {}, cancellation);

    EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
    EXPECT_NE(result.message.find("HTTP 503"), std::string::npos);
    EXPECT_NE(result.message.find("request rejected"), std::string::npos);
    mock.join();
}

TEST(ProviderClient, ReportsATruncatedResponseAsATransportError) {
    const std::string body = "data: {\"choices\":[{\"delta\":{\"content\":\"Partial\"}}]}\n\n";
    const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        "Content-Length: " + std::to_string(body.size() + 20)
        + "\r\nConnection: close\r\n\r\n" + body;
    MockHttpServer mock({response});
    mock.start();
    std::atomic_bool cancellation{false};
    ProviderClient client(network_definition(mock.port()));
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 12, "Question");
    std::string output;

    const GenerationResult result = complete(
        client, request, transcript,
        [&output](GenerationDelta delta) { output += delta.text; }, cancellation);

    EXPECT_EQ(result.outcome, GenerationOutcome::transport_error);
    EXPECT_NE(result.message.find("HTTP request failed"), std::string::npos);
    EXPECT_EQ(output, "Partial");
    mock.join();
}

TEST(ProviderClient, CancelsAnActiveStreamingTransfer) {
    const std::string body = "data: {\"choices\":[{\"delta\":{\"content\":\"Partial\"}}]}\n\n";
    const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        "Content-Length: " + std::to_string(body.size() + 20)
        + "\r\nConnection: close\r\n\r\n" + body;
    MockHttpServer mock({response}, true);
    mock.start();
    std::atomic_bool cancellation{false};
    ProviderClient client(network_definition(mock.port()));
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 13, "Question");
    std::string output;

    const GenerationResult result = complete(
        client, request, transcript,
        [&output, &cancellation](GenerationDelta delta) {
            output += delta.text;
            cancellation.store(true, std::memory_order_release);
        }, cancellation);

    EXPECT_EQ(result.outcome, GenerationOutcome::cancelled);
    EXPECT_EQ(output, "Partial");
    mock.join();
}

TEST(ProviderClient, ReportsAJsonErrorReturnedInsteadOfAStream) {
    MockHttpServer mock({http_response(
        "application/json", R"({"error":{"message":"model unavailable"}})")});
    mock.start();
    std::atomic_bool cancellation{false};
    ProviderClient client(network_definition(mock.port()));
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 14, "Question");

    const GenerationResult result = complete(
        client, request, transcript, [](GenerationDelta) {}, cancellation);

    EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
    EXPECT_EQ(result.message.find("model unavailable"), std::string::npos);
    EXPECT_NE(result.message.find("HTTP 200"), std::string::npos);
    EXPECT_NE(result.message.find("application/json"), std::string::npos);
    mock.join();
}

TEST(ProviderClient, DiscoversItsModelBeforeTheFirstGeneration) {
    MockHttpServer mock({
        http_response("application/json", R"({"data":[{"id":"discovered-model"}]})"),
        http_response("application/json", R"({"choices":[{"message":{"content":"Answer"}}]})"),
    });
    mock.start();
    CharacterDefinition definition = network_definition(mock.port(), false);
    definition.backend.model.clear();
    std::atomic_bool cancellation{false};
    ProviderClient client(std::move(definition));
    EXPECT_EQ(client.info().model, "discovered-model");
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 18, "Question");

    const GenerationResult result = complete(
        client, request, transcript, [](GenerationDelta) {}, cancellation);

    EXPECT_EQ(result.outcome, GenerationOutcome::completed);
    mock.join();
    ASSERT_EQ(mock.requests().size(), 2U);
    EXPECT_TRUE(mock.requests()[0].starts_with("GET /v1/models HTTP/1.1"));
    EXPECT_EQ(Json::parse(request_body(mock.requests()[1]))["model"], "discovered-model");
}

TEST(ProviderClient, StreamsResponsesApiAnswerAndBuildsResponsesRequest) {
    const std::string stream =
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Hello\"}\n\n"
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\" world\"}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n\n";
    MockHttpServer mock({http_response("text/event-stream", stream)});
    mock.start();

    CharacterDefinition definition = responses_network_definition(mock.port());
    definition.backend.temperature = 0.25;
    definition.backend.reasoning_effort = "medium";
    definition.backend.api_key = "test-key";
    definition.system_prompt = "Be concise.";
    std::atomic_bool cancellation{false};
    ProviderClient client(std::move(definition));
    Transcript transcript;
    const GenerationRequest request = client_request(
        transcript, 27, "Question", {
            test::human_entry(1, {"human", "You"}, {"assistant", "Assistant"}, "Earlier question", 6),
            make_character_entry(2, "assistant", "Assistant", "Earlier answer", EntryStatus::complete, 6),
        });
    std::vector<std::string> deltas;

    const GenerationResult result = complete(
        client, request, transcript,
        [&deltas](GenerationDelta delta) {
            deltas.push_back(std::move(delta.text));
        },
        cancellation);

    EXPECT_EQ(result.outcome, GenerationOutcome::completed);
    EXPECT_EQ(deltas, (std::vector<std::string>{"Hello", " world"}));
    mock.join();
    ASSERT_EQ(mock.requests().size(), 1U);
    EXPECT_TRUE(mock.requests().front().starts_with("POST /v1/responses HTTP/1.1"));
    EXPECT_TRUE(client.info().api.ends_with("/v1/responses"));
    const Json body = Json::parse(request_body(mock.requests().front()));
    EXPECT_EQ(body["model"], "configured-model");
    EXPECT_TRUE(body["stream"]);
    EXPECT_FALSE(body["store"]);
    EXPECT_DOUBLE_EQ(body["temperature"], 0.25);
    EXPECT_EQ(body["reasoning"]["effort"], "medium");
    EXPECT_FALSE(body.contains("reasoning_effort"));
    EXPECT_FALSE(body.contains("include"));
    EXPECT_EQ(body["tools"], Json::array({Json{{"type", "web_search"}}}));
    EXPECT_EQ(body["tool_choice"], "auto");
    EXPECT_EQ(body["instructions"], "Be concise.");
    EXPECT_EQ(body["input"], Json::array({
        {{"role", "user"}, {"content", "from You:\nEarlier question"}},
        {{"role", "assistant"}, {"content", "Earlier answer"}},
        {{"role", "user"}, {"content", "from You:\nQuestion"}},
    }));
}

TEST(ProviderClient, PrefixesProviderEndpointsWithConfiguredBasePath) {
    MockHttpServer mock({http_response(
        "application/json", R"({"choices":[{"message":{"content":"Answer"}}]})")});
    mock.start();
    CharacterDefinition definition = network_definition(mock.port(), false);
    definition.backend.base_path = "/api";
    std::atomic_bool cancellation{false};
    ProviderClient client(std::move(definition));
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 33, "Question");

    const GenerationResult result = complete(
        client, request, transcript, [](GenerationDelta) {}, cancellation);

    EXPECT_EQ(result.outcome, GenerationOutcome::completed);
    mock.join();
    ASSERT_EQ(mock.requests().size(), 1U);
    EXPECT_TRUE(mock.requests().front().starts_with("POST /api/v1/chat/completions HTTP/1.1"));
    EXPECT_TRUE(client.info().api.ends_with("/api/v1/chat/completions"));
}

TEST(ProviderClient, HandlesNonStreamingResponsesApiResponse) {
    MockHttpServer mock({http_response(
        "application/json",
        R"({"status":"completed","output":[{"type":"message","role":"assistant",)"
        R"("content":[{"type":"output_text","text":"Answer"}]}]})")});
    mock.start();
    std::atomic_bool cancellation{false};
    ProviderClient client(responses_network_definition(mock.port(), false));
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 28, "Question");
    std::string output;

    const GenerationResult result = complete(
        client, request, transcript,
        [&output](GenerationDelta delta) { output += delta.text; }, cancellation);

    EXPECT_EQ(result.outcome, GenerationOutcome::completed);
    EXPECT_EQ(output, "Answer");
    mock.join();
    ASSERT_EQ(mock.requests().size(), 1U);
    EXPECT_TRUE(mock.requests().front().starts_with("POST /v1/responses HTTP/1.1"));
}

TEST(ProviderClient, UnconfiguredProtocolDefaultsToMandatoryWebSearch) {
    MockHttpServer mock({http_response(
        "application/json",
        R"({"status":"completed","output":[{"type":"message","role":"assistant",)"
        R"("content":[{"type":"output_text","text":"Answer"}]}]})")});
    mock.start();
    CharacterDefinition definition = test_definition();
    definition.backend.host = "127.0.0.1";
    definition.backend.port = mock.port();
    definition.backend.mode = Mode::net;
    definition.backend.model = "configured-model";
    definition.backend.stream = false;
    std::atomic_bool cancellation{false};
    ProviderClient client(std::move(definition));
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 32, "Question");

    const GenerationResult result = complete(
        client, request, transcript, [](GenerationDelta) {}, cancellation);

    EXPECT_EQ(result.outcome, GenerationOutcome::completed);
    mock.join();
    ASSERT_EQ(mock.requests().size(), 1U);
    EXPECT_TRUE(mock.requests().front().starts_with("POST /v1/responses HTTP/1.1"));
    const Json body = Json::parse(request_body(mock.requests().front()));
    EXPECT_EQ(body["tools"], Json::array({Json{{"type", "web_search"}}}));
    EXPECT_EQ(body["tool_choice"], "required");
}

TEST(ProviderClient, ReportsATruncatedResponsesStreamAsATransportError) {
    const std::string body =
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Partial\"}\n\n";
    const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        "Content-Length: " + std::to_string(body.size() + 20)
        + "\r\nConnection: close\r\n\r\n" + body;
    MockHttpServer mock({response});
    mock.start();
    std::atomic_bool cancellation{false};
    ProviderClient client(responses_network_definition(mock.port()));
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 29, "Question");
    std::string output;

    const GenerationResult result = complete(
        client, request, transcript,
        [&output](GenerationDelta delta) { output += delta.text; }, cancellation);

    EXPECT_EQ(result.outcome, GenerationOutcome::transport_error);
    EXPECT_NE(result.message.find("HTTP request failed"), std::string::npos);
    EXPECT_EQ(output, "Partial");
    mock.join();
}

TEST(ProviderClient, CancelsAnActiveResponsesStreamingTransfer) {
    const std::string body =
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Partial\"}\n\n";
    const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        "Content-Length: " + std::to_string(body.size() + 20)
        + "\r\nConnection: close\r\n\r\n" + body;
    MockHttpServer mock({response}, true);
    mock.start();
    std::atomic_bool cancellation{false};
    ProviderClient client(responses_network_definition(mock.port()));
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 30, "Question");
    std::string output;

    const GenerationResult result = complete(
        client, request, transcript,
        [&output, &cancellation](GenerationDelta delta) {
            output += delta.text;
            cancellation.store(true, std::memory_order_release);
        }, cancellation);

    EXPECT_EQ(result.outcome, GenerationOutcome::cancelled);
    EXPECT_EQ(output, "Partial");
    mock.join();
}

TEST(ProviderClient, DiscoversModelBeforeResponsesGeneration) {
    MockHttpServer mock({
        http_response("application/json", R"({"data":[{"id":"discovered-model"}]})"),
        http_response(
            "application/json",
            R"({"status":"completed","output":[{"type":"message","role":"assistant",)"
            R"("content":[{"type":"output_text","text":"Answer"}]}]})"),
    });
    mock.start();
    CharacterDefinition definition = responses_network_definition(mock.port(), false);
    definition.backend.model.clear();
    std::atomic_bool cancellation{false};
    ProviderClient client(std::move(definition));
    EXPECT_EQ(client.info().model, "discovered-model");
    EXPECT_TRUE(client.info().api.ends_with("/v1/responses"));
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 31, "Question");

    const GenerationResult result = complete(
        client, request, transcript, [](GenerationDelta) {}, cancellation);

    EXPECT_EQ(result.outcome, GenerationOutcome::completed);
    mock.join();
    ASSERT_EQ(mock.requests().size(), 2U);
    EXPECT_TRUE(mock.requests()[0].starts_with("GET /v1/models HTTP/1.1"));
    EXPECT_TRUE(mock.requests()[1].starts_with("POST /v1/responses HTTP/1.1"));
    EXPECT_EQ(Json::parse(request_body(mock.requests()[1]))["model"], "discovered-model");
}

} // namespace
} // namespace cha
