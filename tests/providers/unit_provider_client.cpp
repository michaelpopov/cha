#include "providers/provider_client.h"
#include "chat/transcript.h"
#include "providers/api_key_store.h"
#include "providers/openai_oauth.h"
#include "support/mock_http_server.h"
#include "support/test_transcript.h"
#include "util/environment.h"
#include "util/logging.h"
#include "util/private_filesystem.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
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
    EXPECT_EQ(mock.requests().front().find("Authorization: Bearer"), std::string::npos);
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
        R"({"choices":[{"message":{"content":"private response"}}],"usage":{"prompt_tokens":12,"completion_tokens":5,"prompt_tokens_details":{"cached_tokens":9,"cache_write_tokens":7}}})";
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
    EXPECT_NE(output.find("cache_write_tokens=7"), std::string::npos);
    EXPECT_EQ(output.find("private prompt"), std::string::npos);
    EXPECT_EQ(output.find("private response"), std::string::npos);
}

TEST(ProviderClient, AddsCacheMetadataForSupportedHosts) {
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
    EXPECT_FALSE(short_body.contains("prompt_cache_options"));
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
    EXPECT_EQ(
        responses_body["prompt_cache_options"],
        Json({{"mode", "implicit"}, {"ttl", "30m"}}));
    EXPECT_FALSE(responses_body.contains("prompt_cache_retention"));
    ASSERT_TRUE(responses.session_id);
    EXPECT_EQ(*responses.session_id, request.run.prompt_cache_key);
    EXPECT_FALSE(responses_body.contains("previous_response_id"));
    EXPECT_FALSE(responses_body.contains("conversation"));

    CharacterDefinition openrouter = network_definition(443, false);
    openrouter.provider.config.host = "OPENROUTER.AI.";
    openrouter.provider.config.https = true;
    openrouter.provider.config.api = ProviderApi::chat_completions;
    ProviderClient openrouter_chat_client(shared_definition(openrouter));
    const RequestPayload openrouter_chat = openrouter_chat_client.prepare(request);
    const Json openrouter_chat_body = Json::parse(openrouter_chat.bytes);
    EXPECT_EQ(openrouter_chat_body["session_id"], request.run.prompt_cache_key);
    EXPECT_FALSE(openrouter_chat_body.contains("prompt_cache_key"));
    EXPECT_FALSE(openrouter_chat.session_id);

    openrouter.provider.config.api = ProviderApi::responses;
    ProviderClient openrouter_responses_client(shared_definition(openrouter));
    const RequestPayload openrouter_responses =
        openrouter_responses_client.prepare(request);
    const Json openrouter_responses_body = Json::parse(openrouter_responses.bytes);
    EXPECT_EQ(openrouter_responses_body["session_id"], request.run.prompt_cache_key);
    EXPECT_FALSE(openrouter_responses_body.contains("prompt_cache_key"));
    EXPECT_FALSE(openrouter_responses_body.contains("prompt_cache_options"));
    EXPECT_FALSE(openrouter_responses_body.contains("prompt_cache_retention"));
    EXPECT_FALSE(openrouter_responses.session_id);

    openrouter.provider.config.cache_retention = CacheRetention::off;
    ProviderClient openrouter_disabled_client(shared_definition(openrouter));
    const RequestPayload openrouter_disabled =
        openrouter_disabled_client.prepare(request);
    EXPECT_FALSE(Json::parse(openrouter_disabled.bytes).contains("session_id"));
    EXPECT_FALSE(openrouter_disabled.session_id);

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

    openrouter.provider.config.cache_retention = CacheRetention::short_;
    openrouter.provider.config.host = "openrouter.ai.example";
    ProviderClient openrouter_gateway_client(
        shared_definition(std::move(openrouter)));
    const RequestPayload openrouter_gateway =
        openrouter_gateway_client.prepare(request);
    const Json openrouter_gateway_body = Json::parse(openrouter_gateway.bytes);
    EXPECT_FALSE(openrouter_gateway_body.contains("session_id"));
    EXPECT_FALSE(openrouter_gateway.session_id);
}

TEST(ProviderClient, AddsOpenRouterWebSearchToolToChatRequests) {
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 34, "Question");
    const auto request_body_for = [&request](WebSearchMode mode) {
        CharacterDefinition definition = network_definition(443, false);
        definition.provider.config.host = "OPENROUTER.AI.";
        definition.provider.config.https = true;
        definition.provider.config.web_search = mode;
        ProviderClient client(shared_definition(std::move(definition)));
        return Json::parse(client.prepare(request).bytes);
    };

    const Json automatic = request_body_for(WebSearchMode::automatic);
    EXPECT_EQ(
        automatic["tools"],
        Json::array({Json{{"type", "openrouter:web_search"}}}));
    EXPECT_EQ(automatic["tool_choice"], "auto");

    const Json required = request_body_for(WebSearchMode::required);
    EXPECT_EQ(
        required["tools"],
        Json::array({Json{{"type", "openrouter:web_search"}}}));
    EXPECT_EQ(required["tool_choice"], "required");

    const Json off = request_body_for(WebSearchMode::off);
    EXPECT_FALSE(off.contains("tools"));
    EXPECT_FALSE(off.contains("tool_choice"));
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

TEST(ProviderClient, UnconfiguredProtocolDefaultsToWebSearchOff) {
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
    EXPECT_FALSE(body.contains("tools"));
    EXPECT_FALSE(body.contains("tool_choice"));
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

std::string base64url_encode(std::string_view input) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string encoded;
    int value = 0;
    int bits = -6;
    for (const unsigned char character : input) {
        value = (value << 8) + character;
        bits += 8;
        while (bits >= 0) {
            encoded.push_back(table[(value >> bits) & 0x3f]);
            bits -= 6;
        }
    }
    if (bits > -6) {
        encoded.push_back(table[((value << 8) >> (bits + 8)) & 0x3f]);
    }
    return encoded;
}

std::string jwt_for_account(std::string_view account_id) {
    const Json header = {{"alg", "none"}, {"typ", "JWT"}};
    const Json payload = {
        {"https://api.openai.com/auth",
         {{"chatgpt_account_id", std::string(account_id)}}}};
    return base64url_encode(header.dump()) + "."
        + base64url_encode(payload.dump()) + ".sig";
}

std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream file(path);
    return {
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()};
}

CharacterDefinition subscription_definition() {
    CharacterDefinition definition = test_definition();
    definition.provider.config.host = "chatgpt.com";
    definition.provider.config.port = 443;
    definition.provider.config.base_path = "/backend-api/codex";
    definition.provider.config.mode = Mode::net;
    definition.provider.config.https = true;
    definition.provider.config.api = ProviderApi::responses;
    definition.provider.config.auth = ProviderAuth::openai_subscription;
    definition.provider.config.model = "gpt-5.6-terra";
    definition.provider.config.stream = true;
    definition.provider.config.web_search = WebSearchMode::off;
    definition.provider.config.cache_retention = CacheRetention::off;
    return definition;
}

const std::string kCompletedStream =
    "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Hello\"}\n\n"
    "data: {\"type\":\"response.output_text.delta\",\"delta\":\" world\"}\n\n"
    "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\","
    "\"usage\":{\"input_tokens\":11,\"output_tokens\":4}}}\n\n";

class SubscriptionOwner {
public:
    SubscriptionOwner() {
        directory_ = std::filesystem::temp_directory_path()
            / ("cha_subscription_oauth_"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory_);
        path_ = directory_ / "cha.sqlite3.openai-auth.json";
    }

    ~SubscriptionOwner() {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    void write_bundle(
        std::string_view access_token,
        std::string_view refresh_token,
        std::int64_t expires_at,
        std::string_view account_id) {
        Json object;
        object["access_token"] = access_token;
        object["refresh_token"] = refresh_token;
        object["expires_at"] = expires_at;
        object["account_id"] = account_id;
        create_private_file(path_, object.dump());
    }

    OpenAiOAuth make_connected(
        std::string_view account_id = "acct_test",
        std::string_view refresh_token = "fixture-refresh") {
        const std::string access = jwt_for_account(account_id);
        write_bundle(access, refresh_token, 1'800'000'000, account_id);
        return OpenAiOAuth{path_, transport(), clock()};
    }

    OpenAiOAuth make_near_expiry(std::string_view account_id = "acct_test") {
        const std::string access = jwt_for_account(account_id);
        write_bundle(access, "old-refresh", unix_now() + 60, account_id);
        return OpenAiOAuth{path_, transport(), clock()};
    }

    OpenAiOAuthTransport transport() {
        return [this](const OpenAiOAuthHttpRequest& request) {
            if (on_auth_request) on_auth_request(request);
            std::lock_guard lock(mutex_);
            auth_requests.push_back(request);
            if (next >= responses.size()) {
                throw std::runtime_error("unexpected OpenAI auth request");
            }
            return responses[next++];
        };
    }

    OpenAiOAuthClock clock() {
        return [this] {
            std::lock_guard lock(mutex_);
            return now_;
        };
    }

    std::int64_t unix_now() {
        std::lock_guard lock(mutex_);
        return std::chrono::duration_cast<std::chrono::seconds>(
                   now_.time_since_epoch())
            .count();
    }

    void advance(std::chrono::seconds delay) {
        std::lock_guard lock(mutex_);
        now_ += delay;
    }

    void push_refresh(std::string_view refresh, std::string_view account_id) {
        std::lock_guard lock(mutex_);
        responses.push_back({
            200,
            Json{
                {"access_token", jwt_for_account(account_id)},
                {"refresh_token", refresh},
                {"expires_in", 3600},
            }.dump(),
        });
    }

    void push_login_success(std::string_view refresh, std::string_view account_id) {
        std::lock_guard lock(mutex_);
        responses.push_back({
            200,
            Json{
                {"device_auth_id", "fixture-device"},
                {"user_code", "TEST-ONLY"},
                {"interval", 1},
            }.dump(),
        });
        responses.push_back({
            200,
            Json{
                {"authorization_code", "fixture-code"},
                {"code_verifier", "fixture-verifier"},
            }.dump(),
        });
        responses.push_back({
            200,
            Json{
                {"access_token", jwt_for_account(account_id)},
                {"refresh_token", refresh},
                {"expires_in", 3600},
            }.dump(),
        });
    }

    Json stored() const { return Json::parse(file_bytes(path_)); }

    std::function<void(const OpenAiOAuthHttpRequest&)> on_auth_request;
    std::vector<OpenAiOAuthHttpRequest> auth_requests;

private:
    std::filesystem::path directory_;
    std::filesystem::path path_;
    std::mutex mutex_;
    std::chrono::system_clock::time_point now_{
        std::chrono::system_clock::time_point{std::chrono::seconds{1'700'000'000}}};
    std::vector<OpenAiOAuthHttpResponse> responses;
    std::size_t next{};
};

TEST(ProviderClient, MissingSavedApiKeyFailsWhenTheProviderIsUsed) {
    const std::filesystem::path directory = std::filesystem::temp_directory_path()
        / ("cha_provider_missing_key_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{directory};
    ApiKeyStore keys(directory / "api-keys.json");
    CharacterDefinition definition = network_definition(1, false);
    definition.provider.config.api_key_id = "api_key_99";
    try {
        (void)ProviderClient(
            shared_definition(std::move(definition)), nullptr, &keys);
        FAIL() << "expected missing API key rejection";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("api_key_99"), std::string::npos);
    }
}

TEST(ProviderClient, UsesASavedApiKeyWithoutReadingTheEnvironment) {
    ScopedEnvironmentVariable environment("OPENAI_API_KEY");
    ASSERT_TRUE(set_environment_variable("OPENAI_API_KEY", "environment-secret"));
    const std::filesystem::path directory = std::filesystem::temp_directory_path()
        / ("cha_provider_saved_key_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{directory};
    ApiKeyStore keys(directory / "api-keys.json");
    const ApiKeyInfo key = keys.create("Router", "saved-secret");

    CharacterDefinition definition = network_definition(443, false);
    definition.provider.config.api_key_id = key.id;
    std::vector<ProviderHttpRequest> requests;
    ProviderClient client(
        shared_definition(std::move(definition)),
        nullptr,
        &keys,
        [&requests](
            const ProviderHttpRequest& request,
            const std::atomic_bool&) {
            requests.push_back(request);
            return ProviderHttpResponse{
                .status = 200,
                .content_type = "application/json",
                .body = R"({"choices":[{"message":{"content":"Answer"}}]})",
            };
        });
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 81, "Question");
    std::atomic_bool cancellation{false};
    const GenerationResult result = complete(
        client, request, transcript, [](GenerationDelta) {}, cancellation);

    EXPECT_EQ(result.outcome, GenerationOutcome::completed);
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_NE(
        std::ranges::find(
            requests.front().headers,
            "Authorization: Bearer saved-secret"),
        requests.front().headers.end());
    EXPECT_EQ(
        std::ranges::find(
            requests.front().headers,
            "Authorization: Bearer environment-secret"),
        requests.front().headers.end());
}

TEST(ProviderClient, DoesNotUseAProcessEnvironmentKeyAsFallback) {
    ScopedEnvironmentVariable environment("OPENAI_API_KEY");
    ASSERT_TRUE(set_environment_variable("OPENAI_API_KEY", "environment-secret"));
    CharacterDefinition definition = network_definition(443, false);
    std::vector<ProviderHttpRequest> requests;
    ProviderClient client(
        shared_definition(std::move(definition)),
        nullptr,
        nullptr,
        [&requests](
            const ProviderHttpRequest& request,
            const std::atomic_bool&) {
            requests.push_back(request);
            return ProviderHttpResponse{
                .status = 200,
                .content_type = "application/json",
                .body = R"({"choices":[{"message":{"content":"Answer"}}]})",
            };
        });
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 82, "Question");
    std::atomic_bool cancellation{false};
    const GenerationResult result = complete(
        client, request, transcript, [](GenerationDelta) {}, cancellation);

    EXPECT_EQ(result.outcome, GenerationOutcome::completed);
    ASSERT_EQ(requests.size(), 1U);
    EXPECT_EQ(
        std::ranges::find_if(
            requests.front().headers,
            [](const std::string& header) {
                return header.starts_with("Authorization:");
            }),
        requests.front().headers.end());
}

TEST(ProviderClient, SubscriptionWithoutOwnerFailsClearly) {
    try {
        (void)ProviderClient(shared_definition(subscription_definition()));
        FAIL() << "expected missing owner rejection";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find("authentication owner"),
            std::string::npos);
    }
}

TEST(ProviderClient, StreamsSubscriptionRequestWithChaIdentity) {
    SubscriptionOwner owner;
    OpenAiOAuth oauth = owner.make_connected();
    std::vector<ProviderHttpRequest> captured;
    ProviderHttpTransport transport =
        [&captured](const ProviderHttpRequest& request, const std::atomic_bool&) {
            captured.push_back(request);
            return ProviderHttpResponse{
                200, "text/event-stream", kCompletedStream};
        };
    const SharedCharacterDefinition shared =
        shared_definition(subscription_definition());
    ProviderClient client(shared, &oauth, transport);
    std::atomic_bool cancellation{false};
    Transcript transcript;
    const GenerationRequest request = client_request(
        transcript, 40, "Question", {
            test::human_entry(
                1, {"human", "You"}, {"assistant", "Assistant"},
                "Earlier question", 6),
            make_character_entry(
                2, "assistant", "Assistant", "Earlier answer",
                EntryStatus::complete, 6),
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
    ASSERT_TRUE(result.usage.input_tokens);
    ASSERT_TRUE(result.usage.output_tokens);
    EXPECT_EQ(*result.usage.input_tokens, 11U);
    EXPECT_EQ(*result.usage.output_tokens, 4U);
    ASSERT_EQ(captured.size(), 1U);
    EXPECT_EQ(captured.front().url, "https://chatgpt.com/backend-api/codex/responses");
    EXPECT_EQ(
        character_runtime_info(*shared).api,
        "https://chatgpt.com/backend-api/codex/responses");
    const auto has_header = [&captured](std::string_view header) {
        return std::ranges::find(captured.front().headers, header)
            != captured.front().headers.end();
    };
    const std::string access = jwt_for_account("acct_test");
    EXPECT_TRUE(has_header("Authorization: Bearer " + access));
    EXPECT_TRUE(has_header("chatgpt-account-id: acct_test"));
    EXPECT_TRUE(has_header("Content-Type: application/json"));
    EXPECT_TRUE(has_header("Accept: text/event-stream"));
    EXPECT_TRUE(has_header("OpenAI-Beta: responses=experimental"));
    EXPECT_TRUE(has_header("originator: cha"));
    EXPECT_TRUE(has_header("User-Agent: cha"));
    const Json body = Json::parse(captured.front().body);
    EXPECT_EQ(body["model"], "gpt-5.6-terra");
    EXPECT_TRUE(body["stream"]);
    EXPECT_FALSE(body["store"]);
    EXPECT_EQ(body["instructions"], "You are a helpful assistant.");
    EXPECT_FALSE(body.contains("temperature"));
    EXPECT_FALSE(body.contains("max_output_tokens"));
    EXPECT_FALSE(body.contains("tools"));
    EXPECT_FALSE(body.contains("prompt_cache_key"));
    EXPECT_EQ(body["input"], Json::array({
        {{"role", "user"}, {"content", "from You:\nEarlier question"}},
        {{"role", "assistant"}, {"content", "Earlier answer"}},
        {{"role", "user"}, {"content", "from You:\nQuestion"}},
    }));
}

TEST(ProviderClient, CancelledSubscriptionRequestSendsNothingBeforeAuth) {
    SubscriptionOwner owner;
    OpenAiOAuth oauth = owner.make_connected();
    int model_calls = 0;
    int auth_calls = 0;
    owner.on_auth_request = [&auth_calls](const OpenAiOAuthHttpRequest&) {
        ++auth_calls;
    };
    ProviderHttpTransport transport =
        [&model_calls](const ProviderHttpRequest&, const std::atomic_bool&) {
            ++model_calls;
            return ProviderHttpResponse{200, "text/event-stream", kCompletedStream};
        };
    ProviderClient client(
        shared_definition(subscription_definition()), &oauth, transport);
    std::atomic_bool cancellation{true};
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 41, "skip");

    const GenerationResult result = complete(
        client, request, transcript, [](GenerationDelta) {}, cancellation);

    EXPECT_EQ(result.outcome, GenerationOutcome::cancelled);
    EXPECT_EQ(model_calls, 0);
    EXPECT_EQ(auth_calls, 0);
}

TEST(ProviderClient, CancellationDuringRefreshSavesCredentialsAndSkipsModel) {
    SubscriptionOwner owner;
    std::mutex mutex;
    std::condition_variable ready;
    bool refresh_started = false;
    bool finish_refresh = false;
    owner.on_auth_request = [&](const OpenAiOAuthHttpRequest&) {
        {
            std::lock_guard lock(mutex);
            refresh_started = true;
        }
        ready.notify_all();
        std::unique_lock lock(mutex);
        ready.wait(lock, [&] { return finish_refresh; });
    };
    owner.push_refresh("new-refresh", "acct_test");
    OpenAiOAuth oauth = owner.make_near_expiry();
    int model_calls = 0;
    ProviderHttpTransport transport =
        [&model_calls](const ProviderHttpRequest&, const std::atomic_bool&) {
            ++model_calls;
            return ProviderHttpResponse{200, "text/event-stream", kCompletedStream};
        };
    ProviderClient client(
        shared_definition(subscription_definition()), &oauth, transport);
    std::atomic_bool cancellation{false};
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 42, "refresh");
    GenerationResult result{};
    std::thread worker([&] {
        result = complete(
            client, request, transcript, [](GenerationDelta) {}, cancellation);
    });
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(ready.wait_for(lock, std::chrono::seconds{2}, [&] {
            return refresh_started;
        }));
    }
    cancellation.store(true, std::memory_order_release);
    {
        std::lock_guard lock(mutex);
        finish_refresh = true;
    }
    ready.notify_all();
    worker.join();

    EXPECT_EQ(result.outcome, GenerationOutcome::cancelled);
    EXPECT_EQ(model_calls, 0);
    EXPECT_EQ(owner.stored().at("refresh_token"), "new-refresh");
    EXPECT_EQ(oauth.status().state, OpenAiOAuthState::connected);
}

TEST(ProviderClient, CancellationDuringFailedRefreshCleansUpAndSkipsModel) {
    SubscriptionOwner owner;
    std::atomic_bool cancellation{false};
    int auth_calls = 0;
    owner.on_auth_request = [&](const OpenAiOAuthHttpRequest&) {
        ++auth_calls;
        cancellation.store(true, std::memory_order_release);
        throw std::runtime_error("synthetic refresh failure");
    };
    OpenAiOAuth oauth = owner.make_near_expiry();
    int model_calls = 0;
    ProviderHttpTransport transport =
        [&](const ProviderHttpRequest&, const std::atomic_bool&) {
            ++model_calls;
            return ProviderHttpResponse{200, "text/event-stream", kCompletedStream};
        };
    ProviderClient client(
        shared_definition(subscription_definition()), &oauth, transport);
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 43, "refresh");

    const GenerationResult result = complete(
        client, request, transcript, [](GenerationDelta) {}, cancellation);

    EXPECT_EQ(result.outcome, GenerationOutcome::cancelled);
    EXPECT_TRUE(result.message.empty());
    EXPECT_EQ(auth_calls, 1);
    EXPECT_EQ(model_calls, 0);
    EXPECT_EQ(oauth.status().state, OpenAiOAuthState::signed_out);
    EXPECT_THROW(oauth.credentials(), std::runtime_error);
    EXPECT_EQ(auth_calls, 1);
}

TEST(ProviderClient, SubscriptionUnauthorizedDoesNotRetryOrMutateCredentials) {
    SubscriptionOwner owner;
    OpenAiOAuth oauth = owner.make_connected("acct_old", "keep-refresh");
    std::vector<ProviderHttpRequest> captured;
    ProviderHttpTransport transport =
        [&captured](const ProviderHttpRequest& request, const std::atomic_bool&) {
            captured.push_back(request);
            return ProviderHttpResponse{401, "application/json", R"({"error":"no"})"};
        };
    ProviderClient client(
        shared_definition(subscription_definition()), &oauth, transport);
    std::atomic_bool cancellation{false};
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 43, "expired");

    const GenerationResult result = complete(
        client, request, transcript, [](GenerationDelta) {}, cancellation);

    EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
    EXPECT_EQ(
        result.message,
        "ChatGPT rejected this request. Reconnect before using this provider.");
    ASSERT_EQ(captured.size(), 1U);
    EXPECT_EQ(oauth.status().state, OpenAiOAuthState::connected);
    EXPECT_EQ(owner.stored().at("refresh_token"), "keep-refresh");
    EXPECT_EQ(oauth.credentials().account_id, "acct_old");
}

TEST(ProviderClient, OlderSubscriptionUnauthorizedLeavesLaterLoginIntact) {
    SubscriptionOwner owner;
    OpenAiOAuth oauth = owner.make_connected("acct_old", "old-refresh");
    owner.push_login_success("new-refresh", "acct_new");
    std::vector<ProviderHttpRequest> captured;
    ProviderHttpTransport transport =
        [&](const ProviderHttpRequest& request, const std::atomic_bool&) {
            captured.push_back(request);
            (void)oauth.disconnect();
            (void)oauth.start();
            owner.advance(std::chrono::seconds{1});
            (void)oauth.poll();
            return ProviderHttpResponse{401, "application/json", "{}"};
        };
    ProviderClient client(
        shared_definition(subscription_definition()), &oauth, transport);
    std::atomic_bool cancellation{false};
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 44, "stale");

    const GenerationResult result = complete(
        client, request, transcript, [](GenerationDelta) {}, cancellation);

    EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
    EXPECT_EQ(
        result.message,
        "ChatGPT rejected this request. Reconnect before using this provider.");
    ASSERT_EQ(captured.size(), 1U);
    EXPECT_EQ(oauth.status().state, OpenAiOAuthState::connected);
    EXPECT_EQ(oauth.credentials().account_id, "acct_new");
    EXPECT_EQ(owner.stored().at("refresh_token"), "new-refresh");
}

TEST(ProviderClientLive, SubscriptionStreamedRequest) {
    if (std::getenv("CHA_OPENAI_OAUTH_LIVE") == nullptr) {
        GTEST_SKIP() << "set CHA_OPENAI_OAUTH_LIVE=1 to run the live request";
    }

    const std::filesystem::path directory =
        std::filesystem::temp_directory_path()
        / ("cha_subscription_live_"
           + std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    const std::filesystem::path path =
        directory / "cha.sqlite3.openai-auth.json";
    struct Cleanup {
        std::filesystem::path directory;
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code error;
            std::filesystem::remove(path, error);
            std::filesystem::remove_all(directory, error);
        }
    } cleanup{directory, path};

    OpenAiOAuth oauth{path};
    const OpenAiOAuthSnapshot started = oauth.start();
    ASSERT_EQ(started.state, OpenAiOAuthState::waiting);
    ASSERT_TRUE(started.user_code);
    ASSERT_TRUE(started.verification_url);
    std::cerr
        << "\nApprove this CHA login in a browser:\n"
        << "  URL:  " << *started.verification_url << "\n"
        << "  Code: " << *started.user_code << "\n"
        << std::flush;

    OpenAiOAuthSnapshot snapshot = started;
    while (snapshot.state == OpenAiOAuthState::waiting) {
        const auto delay = snapshot.next_poll_delay_ms.value_or(1000);
        std::this_thread::sleep_for(std::chrono::milliseconds{delay});
        snapshot = oauth.poll();
        if (snapshot.error) {
            FAIL() << "live login failed with a sanitized error";
        }
    }
    ASSERT_EQ(snapshot.state, OpenAiOAuthState::connected);

    CharacterDefinition definition = subscription_definition();
    definition.system_prompt = "Reply with exactly: pong";
    ProviderClient client(shared_definition(std::move(definition)), &oauth);
    std::atomic_bool cancellation{false};
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 45, "ping");
    std::string answer;
    const GenerationResult result = complete(
        client, request, transcript,
        [&answer](GenerationDelta delta) {
            if (delta.kind == GenerationDeltaKind::answer) answer += delta.text;
        },
        cancellation);

    std::cerr
        << "Live subscription request outcome="
        << static_cast<int>(result.outcome)
        << " answer_bytes=" << answer.size()
        << " input_tokens="
        << (result.usage.input_tokens ? std::to_string(*result.usage.input_tokens)
                                      : "unreported")
        << " output_tokens="
        << (result.usage.output_tokens ? std::to_string(*result.usage.output_tokens)
                                       : "unreported")
        << "\n"
        << std::flush;
    EXPECT_EQ(result.outcome, GenerationOutcome::completed);
    EXPECT_FALSE(answer.empty());
    (void)oauth.disconnect();
}

} // namespace
} // namespace cha
