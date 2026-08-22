#include "providers/provider_client.h"
#include "chat/transcript.h"
#include "support/mock_http_server.h"
#include "support/test_transcript.h"
#include "util/environment.h"
#include "util/logging.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cha {
namespace {

using Json = nlohmann::json;

class ScopedEnvironmentVariable {
public:
    explicit ScopedEnvironmentVariable(std::string name) : name_(std::move(name)) {
        if (const char* value = std::getenv(name_.c_str())) previous_ = value;
    }

    ~ScopedEnvironmentVariable() {
        if (previous_) {
            (void)set_environment_variable(name_, *previous_);
        } else {
            (void)unset_environment_variable(name_);
        }
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
};

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
        .provider = {.id = "test", .config = {.model = "fake"}},
    };
}

CharacterDefinition network_definition(int port, bool stream = true) {
    CharacterDefinition definition = test_definition();
    definition.provider.config.host = "127.0.0.1";
    definition.provider.config.port = port;
    definition.provider.config.mode = Mode::net;
    definition.provider.config.model = "configured-model";
    definition.provider.config.stream = stream;
    definition.provider.config.api = ProviderApi::chat_completions;
    definition.provider.config.web_search = WebSearchMode::off;
    return definition;
}

CharacterDefinition responses_network_definition(int port, bool stream = true) {
    CharacterDefinition definition = network_definition(port, stream);
    definition.provider.config.api = ProviderApi::responses;
    definition.provider.config.web_search = WebSearchMode::automatic;
    return definition;
}

SharedCharacterDefinition shared_definition(CharacterDefinition definition) {
    return share_character_definitions({std::move(definition)}).front();
}

std::string status_response(
    int status,
    std::string_view reason,
    std::string_view content_type,
    const std::string& body,
    std::string_view extra_headers = {}) {
    return "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason)
        + "\r\nContent-Type: " + std::string(content_type)
        + "\r\nContent-Length: " + std::to_string(body.size())
        + "\r\n" + std::string(extra_headers)
        + "Connection: close\r\n\r\n" + body;
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
    const SharedCharacterDefinition definition =
        share_character_definitions({test_definition("Helpful character")}).front();
    ProviderClient client(definition);
    ProviderClient second_client(definition);
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
    EXPECT_EQ(second_client.prepare(request).bytes, "hello");
}

TEST(ProviderClient, RejectsAnAlreadyCancelledRequestBeforeDispatch) {
    std::atomic_bool cancellation{true};
    ProviderClient client(shared_definition(test_definition()));
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
    constexpr std::string_view api_key_variable =
        "CHA_PROVIDER_CLIENT_AUTHORIZATION_TEST_KEY";
    ScopedEnvironmentVariable environment{std::string(api_key_variable)};
    ASSERT_TRUE(set_environment_variable(api_key_variable, "test-key"));
    const std::string stream =
        "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\" world\"}}]}\n\n"
        "data: [DONE]\n\n";
    MockHttpServer mock({http_response("text/event-stream", stream)});
    mock.start();

    CharacterDefinition configured = network_definition(mock.port());
    configured.provider.config.temperature = 0.25;
    configured.provider.config.max_tokens = 200;
    configured.provider.config.reasoning_effort = "medium";
    configured.provider.config.api_key_env = api_key_variable;
    configured.system_prompt = "Be concise.";
    const SharedCharacterDefinition definition =
        share_character_definitions({std::move(configured)}).front();
    const CharacterRuntimeInfo runtime = character_runtime_info(*definition);
    EXPECT_EQ(runtime.id, "assistant");
    EXPECT_EQ(runtime.model, "configured-model");
    EXPECT_TRUE(runtime.api.ends_with("/v1/chat/completions"));
    EXPECT_TRUE(runtime.streaming);
    std::atomic_bool cancellation{false};
    ProviderClient client(definition);
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
    EXPECT_EQ(body["max_tokens"], 200);
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
    EXPECT_TRUE(character_runtime_info(*definition).api.ends_with(
        "/v1/chat/completions"));
}

TEST(ProviderClient, OmitsEmptySystemPromptAndEscapesTranscriptContent) {
    MockHttpServer mock({http_response(
        "application/json", R"({"choices":[{"message":{"content":"Answer"}}]})")});
    mock.start();
    std::atomic_bool cancellation{false};
    ProviderClient client(shared_definition(network_definition(mock.port(), false)));
    Transcript transcript;
    const std::string prompt = "quote \" and newline\n and backslash \\";
    const GenerationRequest request = client_request(transcript, 19, prompt);

    const GenerationResult result = complete(
        client, request, transcript, [](GenerationDelta) {}, cancellation);

    EXPECT_EQ(result.outcome, GenerationOutcome::completed);
    mock.join();
    const Json body = Json::parse(request_body(mock.requests().front()));
    EXPECT_FALSE(body.contains("temperature"));
    EXPECT_FALSE(body.contains("max_tokens"));
    ASSERT_EQ(body["messages"].size(), 1U);
    EXPECT_EQ(body["messages"][0]["role"], "user");
    EXPECT_EQ(body["messages"][0]["content"], "from You:\n" + prompt);
}

TEST(ProviderClient, RejectsInvalidUtf8WhenPreparingRequest) {
    ProviderClient client(shared_definition(network_definition(1, false)));
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
    ProviderClient client(shared_definition(network_definition(mock.port(), false)));
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
        R"({"choices":[{"message":{"content":"private response"}}],"usage":{"prompt_tokens":12,"completion_tokens":5,"prompt_tokens_details":{"cached_tokens":9}}})";
    MockHttpServer mock({http_response("application/json", response_body)});
    mock.start();
    DiagnosticLogFile log;
    std::atomic_bool cancellation{false};
    ProviderClient client(shared_definition(network_definition(mock.port(), false)));
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
    EXPECT_NE(output.find("cache_read_tokens=9"), std::string::npos);
    EXPECT_EQ(output.find("private prompt"), std::string::npos);
    EXPECT_EQ(output.find("private response"), std::string::npos);
}

TEST(ProviderClient, AddsCacheMetadataOnlyForDirectOpenAi) {
    Transcript transcript;
    GenerationRequest request = client_request(transcript, 91, "Question");
    request.run.prompt_cache_key = "forum/session/assistant";
    request.run.created_at = 1'700'000'003;

    CharacterDefinition direct = network_definition(443, false);
    direct.provider.config.host = "API.OPENAI.COM.";
    direct.provider.config.https = true;
    direct.system_prompt = "Stable instructions";
    direct.provider.config.api = ProviderApi::chat_completions;
    ProviderClient chat_client(shared_definition(direct));
    const RequestPayload chat = chat_client.prepare(request);
    EXPECT_EQ(Json::parse(chat.bytes)["prompt_cache_key"], request.run.prompt_cache_key);
    EXPECT_FALSE(chat.session_id);

    direct.provider.config.api = ProviderApi::responses;
    ProviderClient short_client(shared_definition(direct));
    const RequestPayload short_payload = short_client.prepare(request);
    const Json short_body = Json::parse(short_payload.bytes);
    EXPECT_EQ(short_body["prompt_cache_key"], request.run.prompt_cache_key);
    EXPECT_FALSE(short_body.contains("prompt_cache_retention"));
    ASSERT_TRUE(short_payload.session_id);
    EXPECT_EQ(*short_payload.session_id, request.run.prompt_cache_key);

    GenerationRequest later_request = request;
    later_request.run.created_at = 1'700'000'004;
    const RequestPayload later_payload = short_client.prepare(later_request);
    const Json later_body = Json::parse(later_payload.bytes);
    EXPECT_EQ(later_body["instructions"], short_body["instructions"]);
    EXPECT_EQ(later_body["prompt_cache_key"], short_body["prompt_cache_key"]);
    ASSERT_TRUE(later_payload.session_id);
    EXPECT_EQ(*later_payload.session_id, *short_payload.session_id);

    direct.provider.config.cache_retention = CacheRetention::long_;
    ProviderClient responses_client(shared_definition(direct));
    const RequestPayload responses = responses_client.prepare(request);
    const Json responses_body = Json::parse(responses.bytes);
    EXPECT_EQ(responses_body["prompt_cache_key"], request.run.prompt_cache_key);
    EXPECT_EQ(responses_body["prompt_cache_retention"], "24h");
    ASSERT_TRUE(responses.session_id);
    EXPECT_EQ(*responses.session_id, request.run.prompt_cache_key);
    EXPECT_FALSE(responses_body.contains("previous_response_id"));
    EXPECT_FALSE(responses_body.contains("conversation"));

    direct.provider.config.cache_retention = CacheRetention::off;
    ProviderClient disabled_client(shared_definition(direct));
    const RequestPayload disabled = disabled_client.prepare(request);
    EXPECT_FALSE(Json::parse(disabled.bytes).contains("prompt_cache_key"));
    EXPECT_FALSE(disabled.session_id);

    direct.provider.config.cache_retention = CacheRetention::short_;
    direct.provider.config.host = "api.openai.com.example";
    ProviderClient gateway_client(shared_definition(std::move(direct)));
    const RequestPayload gateway = gateway_client.prepare(request);
    EXPECT_FALSE(Json::parse(gateway.bytes).contains("prompt_cache_key"));
    EXPECT_FALSE(gateway.session_id);
}

TEST(ProviderClient, ReportsProviderHttpFailure) {
    MockHttpServer mock({status_response(
        503, "Service Unavailable", "application/json",
        R"({"error":{"message":"request rejected"}})")});
    mock.start();
    std::atomic_bool cancellation{false};
    ProviderClient client(shared_definition(network_definition(mock.port())));
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 9, "Question");

    const GenerationResult result = complete(
        client, request, transcript, [](GenerationDelta) {}, cancellation);

    EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
    EXPECT_NE(result.message.find("HTTP 503"), std::string::npos);
    EXPECT_NE(result.message.find("request rejected"), std::string::npos);
    mock.join();
}

TEST(ProviderClient, ClassifiesActionableProviderFailures) {
    struct Case {
        int status;
        std::string_view reason;
        std::string body;
        std::string_view expected;
    };
    const std::vector<Case> cases{
        {429, "Too Many Requests",
         R"({"error":{"code":"insufficient_quota","message":"quota exceeded"}})",
         "Provider quota or billing limit exceeded."},
        {429, "Too Many Requests",
         R"({"error":{"message":"rate limit reached"}})",
         "Provider rate limit exceeded."},
        {401, "Unauthorized",
         R"({"error":{"message":"invalid API key"}})",
         "Provider authentication or permission was rejected."},
        {400, "Bad Request",
         R"({"error":{"message":"maximum context length exceeded"}})",
         "Prompt exceeds the model's context window."},
        {400, "Bad Request",
         R"({"error":{"message":"bad request"}})",
         "Prompt exceeds the model's context window."},
        {413, "Payload Too Large", "", "Prompt exceeds the model's context window."},
        // A bare 400 says nothing about length, so it stays unclassified.
        {400, "Bad Request", "",
         "Inference server returned HTTP 400: unknown server error"},
        // Rate-limit and authentication errors often link to a billing page,
        // which must not turn them into a quota verdict.
        {429, "Too Many Requests",
         R"({"error":{"message":"Rate limit reached. See https://example.test/account/billing."}})",
         "Provider rate limit exceeded."},
        {402, "Payment Required",
         R"({"error":{"message":"Add credits in your billing settings."}})",
         "Provider quota or billing limit exceeded."},
    };

    for (const Case& test_case : cases) {
        SCOPED_TRACE(test_case.status);
        MockHttpServer mock({status_response(
            test_case.status,
            test_case.reason,
            "application/json",
            test_case.body)});
        mock.start();
        std::atomic_bool cancellation{false};
        ProviderClient client(shared_definition(network_definition(mock.port(), false)));
        Transcript transcript;
        const GenerationRequest request = client_request(transcript, 92, "Question");

        const GenerationResult result = complete(
            client, request, transcript, [](GenerationDelta) {}, cancellation);

        EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
        EXPECT_EQ(result.message, test_case.expected);
        mock.join();
    }
}

TEST(ProviderClient, ClassifiesProviderErrorsInsideSuccessfulResponses) {
    struct Case {
        CharacterDefinition definition;
        std::string content_type;
        std::string body;
        std::string_view expected;
    };
    std::vector<Case> cases;
    cases.push_back({
        responses_network_definition(0),
        "text/event-stream",
        "data: {\"type\":\"error\",\"message\":\"quota exceeded\"}\n\n",
        "Provider quota or billing limit exceeded.",
    });
    cases.push_back({
        network_definition(0, false),
        "application/json",
        R"({"error":{"message":"invalid API key"}})",
        "Provider authentication or permission was rejected.",
    });

    for (Case& test_case : cases) {
        MockHttpServer mock({http_response(test_case.content_type, test_case.body)});
        mock.start();
        test_case.definition.provider.config.port = mock.port();
        std::atomic_bool cancellation{false};
        ProviderClient client(shared_definition(std::move(test_case.definition)));
        Transcript transcript;
        const GenerationRequest request = client_request(transcript, 95, "Question");

        const GenerationResult result = complete(
            client, request, transcript, [](GenerationDelta) {}, cancellation);

        EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
        EXPECT_EQ(result.message, test_case.expected);
        mock.join();
    }
}

TEST(ProviderClient, KeepsStreamDiagnosisWhenModelOutputResemblesAProviderError) {
    // The answer text names a provider failure the request did not have. Only
    // the decoder knows what actually went wrong with this stream.
    const std::string stream =
        "data: {\"choices\":[{\"delta\":{\"content\":"
        "\"Your rate limit is per-organization.\"}}]}\n\n";
    MockHttpServer mock({http_response("text/event-stream", stream)});
    mock.start();
    std::atomic_bool cancellation{false};
    ProviderClient client(shared_definition(network_definition(mock.port())));
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 97, "Question");

    const GenerationResult result = complete(
        client, request, transcript, [](GenerationDelta) {}, cancellation);
    mock.join();

    EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
    EXPECT_NE(result.message.find("[DONE]"), std::string::npos);
    EXPECT_EQ(result.message.find("rate limit"), std::string::npos);
}

TEST(ProviderClient, BoundsProviderErrorInsideSuccessfulResponse) {
    const std::string provider_message(10'000, 'x');
    const std::string body =
        "data: {\"type\":\"error\",\"message\":\"" + provider_message
        + "\"}\n\n";
    MockHttpServer mock({http_response("text/event-stream", body)});
    mock.start();
    std::atomic_bool cancellation{false};
    ProviderClient client(shared_definition(responses_network_definition(mock.port())));
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 96, "Question");

    const GenerationResult result = complete(
        client, request, transcript, [](GenerationDelta) {}, cancellation);
    mock.join();

    EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
    EXPECT_LE(result.message.size(), 512U);
}

TEST(ProviderClient, BoundsProviderErrorsAndDoesNotLogResponseBodies) {
    const std::string body = "SENSITIVE_RESPONSE_BODY "
        + std::string(10'000, 'x') + "UNRETAINED_TAIL";
    MockHttpServer mock({status_response(
        503, "Service Unavailable", "text/plain", body)});
    mock.start();
    DiagnosticLogFile log;
    std::atomic_bool cancellation{false};
    ProviderClient client(shared_definition(network_definition(mock.port(), false)));
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 93, "Question");

    const GenerationResult result = complete(
        client, request, transcript, [](GenerationDelta) {}, cancellation);
    mock.join();

    EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
    EXPECT_LT(result.message.size(), 600U);
    EXPECT_EQ(result.message.find("UNRETAINED_TAIL"), std::string::npos);
    const std::string output = log.contents();
    EXPECT_EQ(output.find("SENSITIVE_RESPONSE_BODY"), std::string::npos);
    EXPECT_EQ(output.find("UNRETAINED_TAIL"), std::string::npos);
}

TEST(ProviderClient, LogsTheHighestPriorityProviderRequestId) {
    const std::string body =
        R"({"choices":[{"message":{"content":"Answer"}}]})";
    MockHttpServer mock({status_response(
        200,
        "OK",
        "application/json",
        body,
        "cf-ray: fallback-id\r\nX-Request-Id: preferred-id\r\n")});
    mock.start();
    DiagnosticLogFile log;
    std::atomic_bool cancellation{false};
    ProviderClient client(shared_definition(network_definition(mock.port(), false)));
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 94, "Question");

    const GenerationResult result = complete(
        client, request, transcript, [](GenerationDelta) {}, cancellation);
    mock.join();

    EXPECT_EQ(result.outcome, GenerationOutcome::completed);
    EXPECT_NE(
        log.contents().find("provider_request_id=preferred-id"),
        std::string::npos);
}

TEST(ProviderClient, BoundsOverallAndIdleGenerationTime) {
    struct Case {
        int timeout_s;
        int idle_timeout_s;
        std::chrono::milliseconds maximum_elapsed;
        std::string_view expected_message;
    };
    for (const Case test_case : std::vector<Case>{
             {1, 5, std::chrono::milliseconds(1800), "Timeout"},
             {5, 1, std::chrono::milliseconds(1800), "idle timeout"}}) {
        SCOPED_TRACE(
            "timeout=" + std::to_string(test_case.timeout_s)
            + " idle=" + std::to_string(test_case.idle_timeout_s));
        // The idle clock starts at the first body byte, so the response
        // opens with one SSE keepalive and then stalls mid-body.
        const std::string stalled_response =
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
            "Content-Length: 40\r\nConnection: close\r\n\r\n: ping\n\n";
        MockHttpServer mock(
            {stalled_response},
            false,
            std::chrono::milliseconds(2000));
        mock.start();
        CharacterDefinition definition = network_definition(mock.port());
        definition.provider.config.timeout_s = test_case.timeout_s;
        definition.provider.config.idle_timeout_s = test_case.idle_timeout_s;
        std::atomic_bool cancellation{false};
        GenerationResult result;
        const auto started_at = std::chrono::steady_clock::now();
        {
            ProviderClient client(shared_definition(std::move(definition)));
            Transcript transcript;
            const GenerationRequest request = client_request(transcript, 95, "Question");
            result = complete(
                client, request, transcript, [](GenerationDelta) {}, cancellation);
        }
        const auto elapsed = std::chrono::steady_clock::now() - started_at;

        EXPECT_EQ(result.outcome, GenerationOutcome::transport_error);
        EXPECT_NE(result.message.find(test_case.expected_message), std::string::npos);
        EXPECT_LT(elapsed, test_case.maximum_elapsed);
        mock.join();
    }
}

TEST(ProviderClient, WaitsThroughProviderThinkTimeBeforeTheFirstByte) {
    // The server accepts the request and sends no body while it "thinks",
    // either silently or after its response headers. A reasoning model behind
    // a streaming request looks exactly like the second case, so only the
    // overall timeout may end either one.
    constexpr auto think_time = std::chrono::milliseconds(1500);
    const std::string headers_only =
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        "Content-Length: 20\r\nConnection: close\r\n\r\n";
    for (const std::string& response : {std::string(), headers_only}) {
        SCOPED_TRACE(response.empty() ? "silent" : "headers first");
        MockHttpServer mock({response}, false, think_time);
        mock.start();
        CharacterDefinition definition =
            network_definition(mock.port(), !response.empty());
        definition.provider.config.timeout_s = 600;
        definition.provider.config.idle_timeout_s = 1;
        std::atomic_bool cancellation{false};
        GenerationResult result;
        const auto started_at = std::chrono::steady_clock::now();
        {
            ProviderClient client(shared_definition(std::move(definition)));
            Transcript transcript;
            const GenerationRequest request = client_request(transcript, 98, "Question");
            result = complete(
                client, request, transcript, [](GenerationDelta) {}, cancellation);
        }
        const auto elapsed = std::chrono::steady_clock::now() - started_at;
        mock.join();

        EXPECT_EQ(result.outcome, GenerationOutcome::transport_error);
        EXPECT_EQ(result.message.find("idle timeout"), std::string::npos);
        EXPECT_GE(elapsed, think_time - std::chrono::milliseconds(300));
    }
}

TEST(ProviderClient, ReportsATruncatedResponseAsATransportError) {
    const std::string body = "data: {\"choices\":[{\"delta\":{\"content\":\"Partial\"}}]}\n\n";
    const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
        "Content-Length: " + std::to_string(body.size() + 20)
        + "\r\nConnection: close\r\n\r\n" + body;
    MockHttpServer mock({response});
    mock.start();
    std::atomic_bool cancellation{false};
    ProviderClient client(shared_definition(network_definition(mock.port())));
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
    ProviderClient client(shared_definition(network_definition(mock.port())));
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
    ProviderClient client(shared_definition(network_definition(mock.port())));
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

TEST(ProviderClient, RejectsAnEmptyConfiguredModelWithoutContactingTheProvider) {
    CharacterDefinition definition = network_definition(1, false);
    definition.provider.config.model.clear();
    try {
        (void)ProviderClient(shared_definition(std::move(definition)));
        FAIL() << "expected missing model rejection";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("non-empty configured model"), std::string::npos);
    }
}

TEST(ProviderClient, StreamsResponsesApiAnswerAndBuildsResponsesRequest) {
    const std::string stream =
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Hello\"}\n\n"
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\" world\"}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n\n";
    MockHttpServer mock({http_response("text/event-stream", stream)});
    mock.start();

    CharacterDefinition definition = responses_network_definition(mock.port());
    definition.provider.config.temperature = 0.25;
    definition.provider.config.max_tokens = 8;
    definition.provider.config.reasoning_effort = "medium";
    definition.system_prompt = "Be concise.";
    std::atomic_bool cancellation{false};
    const SharedCharacterDefinition shared = shared_definition(std::move(definition));
    ProviderClient client(shared);
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
    EXPECT_TRUE(character_runtime_info(*shared).api.ends_with("/v1/responses"));
    const Json body = Json::parse(request_body(mock.requests().front()));
    EXPECT_EQ(body["model"], "configured-model");
    EXPECT_TRUE(body["stream"]);
    EXPECT_FALSE(body["store"]);
    EXPECT_DOUBLE_EQ(body["temperature"], 0.25);
    EXPECT_EQ(body["max_output_tokens"], 16);
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
    definition.provider.config.base_path = "/api";
    std::atomic_bool cancellation{false};
    const SharedCharacterDefinition shared = shared_definition(std::move(definition));
    ProviderClient client(shared);
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 33, "Question");

    const GenerationResult result = complete(
        client, request, transcript, [](GenerationDelta) {}, cancellation);

    EXPECT_EQ(result.outcome, GenerationOutcome::completed);
    mock.join();
    ASSERT_EQ(mock.requests().size(), 1U);
    EXPECT_TRUE(mock.requests().front().starts_with("POST /api/v1/chat/completions HTTP/1.1"));
    EXPECT_TRUE(character_runtime_info(*shared).api.ends_with(
        "/api/v1/chat/completions"));
}

TEST(ProviderClient, HandlesNonStreamingResponsesApiResponse) {
    MockHttpServer mock({http_response(
        "application/json",
        R"({"status":"completed","output":[{"type":"message","role":"assistant",)"
        R"("content":[{"type":"output_text","text":"Answer"}]}]})")});
    mock.start();
    std::atomic_bool cancellation{false};
    ProviderClient client(shared_definition(responses_network_definition(mock.port(), false)));
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
    definition.provider.config.host = "127.0.0.1";
    definition.provider.config.port = mock.port();
    definition.provider.config.mode = Mode::net;
    definition.provider.config.model = "configured-model";
    definition.provider.config.stream = false;
    std::atomic_bool cancellation{false};
    ProviderClient client(shared_definition(std::move(definition)));
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
    ProviderClient client(shared_definition(responses_network_definition(mock.port())));
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
    ProviderClient client(shared_definition(responses_network_definition(mock.port())));
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

} // namespace
} // namespace cha
