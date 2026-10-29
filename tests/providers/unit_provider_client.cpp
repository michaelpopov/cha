#include "providers/provider_client.h"
#include "providers/maintenance.h"
#include "support/test_workspace.h"
#include "workspace/workspace_config_store.h"
#include "chat/transcript.h"
#include "providers/api_key_store.h"
#include "providers/openai_oauth.h"
#include "providers/web_search.h"
#include "support/mock_http_server.h"
#include "support/test_transcript.h"
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
#include <future>
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
    explicit DiagnosticLogFile(std::string_view level = "info")
        : directory_(std::filesystem::temp_directory_path()
            / ("cha_generation_logging_"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()))),
          path_(directory_ / "cha.log") {
        shutdown_diagnostic_logging();
        initialize_diagnostic_logging(path_, level);
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

TEST(ProviderClient, BuildsEndpointsForConfiguredProviderBasePaths) {
    struct Case {
        const char* host;
        const char* base_path;
        const char* expected;
    };
    const Case cases[]{
        {"generativelanguage.googleapis.com", "/v1beta/openai",
            "https://generativelanguage.googleapis.com:443/v1beta/openai/chat/completions"},
        {"openrouter.ai", "/api", "https://openrouter.ai:443/api/v1/chat/completions"},
        {"api.fireworks.ai", "/inference",
            "https://api.fireworks.ai:443/inference/v1/chat/completions"},
        {"api.cerebras.ai", "", "https://api.cerebras.ai:443/v1/chat/completions"},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.host);
        ModelBackendConfig config;
        config.host = item.host;
        config.port = 443;
        config.https = true;
        config.base_path = item.base_path;
        config.api = ProviderApi::chat_completions;
        EXPECT_EQ(provider_endpoint(config), item.expected);
    }
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
    EXPECT_TRUE(provider_endpoint(definition->provider.config).ends_with(
        "/v1/chat/completions"));
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
    EXPECT_TRUE(body["messages"][0]["content"].get<std::string>().starts_with("Be concise.\n\n"));
    EXPECT_EQ(body["messages"], Json::array({
        {{"role", "system"}, {"content", body["messages"][0]["content"]}},
        {{"role", "user"}, {"content", "from You:\nEarlier question"}},
        {{"role", "assistant"}, {"content", "Earlier answer"}},
        {{"role", "user"},
         {"content",
          "Shared chat history (JSONL):\n"
          R"({"kind":"character","speaker":"Other","text":"Other answer"})"}},
        {{"role", "user"}, {"content", "from You:\nQuestion"}},
    }));
    EXPECT_TRUE(provider_endpoint(definition->provider.config).ends_with(
        "/v1/chat/completions"));
}

TEST(ProviderClient, AddsToolInstructionsToEmptySystemPromptAndEscapesTranscriptContent) {
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
    EXPECT_FALSE(body.contains("provider"));
    ASSERT_EQ(body["messages"].size(), 2U);
    EXPECT_EQ(body["messages"][0]["role"], "system");
    EXPECT_TRUE(body["messages"][0]["content"].get<std::string>().starts_with("<tool_availability>\n"));
    EXPECT_EQ(body["messages"][1]["role"], "user");
    EXPECT_EQ(body["messages"][1]["content"], "from You:\n" + prompt);
}

TEST(ProviderClient, RestrictsOpenRouterTargetsForBothApiFormats) {
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 92, "Question");
    CharacterDefinition definition = network_definition(443, false);
    definition.provider.config.host = "OPENROUTER.AI.";
    definition.provider.config.https = true;
    definition.provider.config.openrouter_targets = {"CoreWeave", "Crusoe"};

    ProviderClient chat_client(shared_definition(definition));
    const Json chat_body = Json::parse(chat_client.prepare(request).bytes);
    EXPECT_EQ(chat_body["provider"], Json({
        {"order", Json::array({"CoreWeave", "Crusoe"})},
        {"allow_fallbacks", false},
    }));

    definition.provider.config.api = ProviderApi::responses;
    ProviderClient responses_client(shared_definition(std::move(definition)));
    const Json responses_body = Json::parse(
        responses_client.prepare(request).bytes);
    EXPECT_EQ(responses_body["provider"], Json({
        {"order", Json::array({"CoreWeave", "Crusoe"})},
        {"allow_fallbacks", false},
    }));
}

TEST(ProviderClient, ReportsSystemPromptAndConversationTextSizes) {
    Transcript transcript;
    const GenerationRequest request = client_request(
        transcript,
        93,
        "Current question",
        {
            test::human_entry(
                1, {"human", "You"}, {"assistant", "Assistant"},
                "Earlier question", 1),
            make_character_entry(
                2, "assistant", "Assistant", "Earlier answer",
                EntryStatus::complete, 1),
        });
    CharacterDefinition definition = network_definition(443, false);
    definition.system_prompt = "System instructions";

    for (const ProviderApi api : {
             ProviderApi::chat_completions, ProviderApi::responses}) {
        definition.provider.config.api = api;
        ProviderClient client(shared_definition(definition));
        const RequestPayload payload = client.prepare(request);
        ASSERT_TRUE(payload.text_sizes);
        const auto body = Json::parse(payload.bytes);
        const auto instructions = (api == ProviderApi::responses ? body["instructions"]
            : body["messages"][0]["content"]).get<std::string>();
        EXPECT_EQ(
            payload.text_sizes->system_prompt_bytes,
            instructions.size());
        EXPECT_EQ(
            payload.text_sizes->conversation_bytes,
            std::string("from You:\nEarlier question").size()
                + std::string("Earlier answer").size()
                + std::string("from You:\nCurrent question").size());
    }
}

TEST(ProviderClient, ToolInstructionsMatchCapabilitiesAttachedToEachRequest) {
    for (const auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        for (const auto hosted : {WebSearchMode::off, WebSearchMode::automatic, WebSearchMode::required}) {
            auto definition = network_definition(443, false);
            definition.provider.config.api = api;
            definition.provider.config.host = "openrouter.ai";
            definition.provider.config.web_search = hosted;
            definition.system_prompt = "Original instructions.";
            ProviderClient client(shared_definition(definition));
            for (const bool search : {false, true}) {
                for (const bool read : {false, true}) {
                    SCOPED_TRACE(static_cast<int>(api));
                    SCOPED_TRACE(static_cast<int>(hosted));
                    SCOPED_TRACE(search);
                    SCOPED_TRACE(read);
                    Transcript transcript;
                    auto request = client_request(transcript, 1, "Verify this");
                    if (search) request.web_search_tool = [](auto, const auto&) { return "[]"; };
                    if (read) request.web_read_tool = [](auto, const auto&, const auto&) { return "Page text"; };
                    const auto payload = client.prepare(request);
                    const auto body = Json::parse(payload.bytes);
                    const auto instructions = (api == ProviderApi::responses ? body["instructions"]
                        : body["messages"][0]["content"]).get<std::string>();
                    EXPECT_TRUE(instructions.starts_with("Original instructions.\n\n<tool_availability>\n"));
                    EXPECT_NE(instructions.find(search || hosted != WebSearchMode::off
                        ? "Web search is available for this request."
                        : "Web search is unavailable for this request."), std::string::npos);
                    EXPECT_NE(instructions.find(read
                        ? "Web page reading is available for this request."
                        : "Web page reading is unavailable for this request."), std::string::npos);
                    EXPECT_NE(instructions.find("say which requested facts you could not verify"), std::string::npos);
                    EXPECT_NE(instructions.find("Do not give unverified details as facts."), std::string::npos);
                    EXPECT_NE(instructions.find("unless you actually did"), std::string::npos);
                    const auto tool_count = (hosted != WebSearchMode::off) + search + read;
                    if (tool_count) EXPECT_EQ(body["tools"].size(), tool_count);
                    else EXPECT_FALSE(body.contains("tools"));
                    ASSERT_TRUE(payload.text_sizes);
                    EXPECT_EQ(payload.text_sizes->system_prompt_bytes, instructions.size());
                    if (api == ProviderApi::chat_completions) {
                        ASSERT_EQ(body["messages"].size(), 2U);
                        EXPECT_EQ(body["messages"][0]["role"], "system");
                        EXPECT_EQ(body["messages"][1]["role"], "user");
                    }
                }
            }
        }
    }
}

TEST(ProviderClient, PreservesToolAvailabilityTagsInsidePromptDocuments) {
    const std::string original =
        "Character instructions with an example:\n<tool_availability>\n"
        "Example capability text.\n</tool_availability>\n\n"
        "<forum_instructions>Keep the forum rules.</forum_instructions>\n\n"
        "<participant_profile>Keep the participant context.</participant_profile>";
    for (const auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        auto definition = network_definition(443, false);
        definition.provider.config.api = api;
        definition.system_prompt = original;
        ProviderClient client(shared_definition(definition));
        Transcript transcript;
        const auto body = Json::parse(client.prepare(client_request(transcript, 1, "Hello")).bytes);
        const auto instructions = (api == ProviderApi::responses ? body["instructions"]
            : body["messages"][0]["content"]).get<std::string>();
        EXPECT_TRUE(instructions.starts_with(original + "\n\n<tool_availability>\n"));
        EXPECT_NE(instructions.find("Web search is unavailable for this request."), std::string::npos);
    }
}

TEST(ProviderClient, AuxiliaryRequestsKeepTheirSystemInstructions) {
    for (const auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        auto definition = network_definition(443, false);
        definition.provider.config.api = api;
        definition.system_prompt = "Return only a short session name.";
        ProviderClient client(shared_definition(definition));
        Transcript transcript;
        auto request = client_request(transcript, 1, "Plan a garden");
        request.include_tool_instructions = false;
        const auto payload = client.prepare(request);
        const auto body = Json::parse(payload.bytes);
        const auto instructions = (api == ProviderApi::responses ? body["instructions"]
            : body["messages"][0]["content"]).get<std::string>();
        EXPECT_EQ(instructions, definition.system_prompt);
        ASSERT_TRUE(payload.text_sizes);
        EXPECT_EQ(payload.text_sizes->system_prompt_bytes, definition.system_prompt.size());
        EXPECT_FALSE(body.contains("tools"));
    }
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

TEST(ProviderClient, DebugLogsSuccessfulRequestsAndResponsesButRedactsTheActiveApiKey) {
    test::TestWorkspace workspace;
    auto config = WorkspaceConfigStore::open(test::import_test_database(workspace.root()));
    ApiKeyStore keys(*config);
    const auto key = keys.create("Test", "secret-model-key");
    for (bool stream : {false, true}) {
        SCOPED_TRACE(stream);
        const std::string body = stream
            ? "data: {\"choices\":[{\"delta\":{\"content\":\"debug answer secret-model-key\"}}]}\n\n"
                "data: [DONE]\n\n"
            : R"({"choices":[{"message":{"content":"debug answer secret-model-key"}}]})";
        MockHttpServer mock({http_response(stream ? "text/event-stream" : "application/json", body)});
        mock.start();
        DiagnosticLogFile log("debug");
        auto definition = network_definition(mock.port(), stream);
        definition.system_prompt = "debug system instructions";
        definition.provider.config.api_key_id = key.id;
        ProviderClient client(shared_definition(definition), nullptr, &keys);
        Transcript transcript;
        const auto result = complete(client, client_request(transcript, 77, "debug user prompt secret-model-key"),
            transcript, [](GenerationDelta) {}, std::atomic_bool{false});
        mock.join();
        ASSERT_EQ(mock.requests().size(), 1u);
        EXPECT_NE(mock.requests().front().find("Authorization: Bearer secret-model-key\r\n"),
            std::string::npos);
        EXPECT_EQ(result.outcome, GenerationOutcome::completed);
        const auto output = log.contents();
        EXPECT_NE(output.find("HTTP model request body"), std::string::npos);
        EXPECT_NE(output.find("debug system instructions"), std::string::npos);
        EXPECT_NE(output.find("debug user prompt [REDACTED]"), std::string::npos);
        EXPECT_NE(output.find("HTTP model raw response"), std::string::npos);
        EXPECT_NE(output.find("debug answer [REDACTED]"), std::string::npos);
        EXPECT_EQ(output.find("secret-model-key"), std::string::npos);
        EXPECT_EQ(output.find("HTTP model result diagnostic"), std::string::npos);
        EXPECT_EQ(output.find("data=\"\""), std::string::npos);
        if (stream) {
            EXPECT_NE(output.find("[DONE]"), std::string::npos);
        }
        EXPECT_EQ(output.find("Authorization:"), std::string::npos);
    }
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

TEST(ProviderClient, DebugLogsFullHttpErrorsButRedactsTheActiveApiKey) {
    test::TestWorkspace workspace;
    auto config = WorkspaceConfigStore::open(test::import_test_database(workspace.root()));
    ApiKeyStore keys(*config);
    const auto key = keys.create("Test", "secret-model-key");
    const std::string body = std::string(5000, 'x') + " diagnostic-tail secret-model-key";
    MockHttpServer mock({status_response(500, "Error", "text/plain", body)});
    mock.start();
    DiagnosticLogFile log("debug");
    auto definition = network_definition(mock.port(), true);
    definition.provider.config.api_key_id = key.id;
    ProviderClient client(shared_definition(definition), nullptr, &keys);
    Transcript transcript;
    const auto result = complete(client, client_request(transcript, 78, "Test failure"),
        transcript, [](GenerationDelta) {}, std::atomic_bool{false});
    mock.join();
    EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
    const auto output = log.contents();
    EXPECT_NE(output.find("diagnostic-tail [REDACTED]"), std::string::npos);
    EXPECT_EQ(output.find("secret-model-key"), std::string::npos);
    EXPECT_EQ(output.find("Authorization:"), std::string::npos);
}

TEST(ProviderClient, RequestsFastModeForOpenAiAndOpenRouterEndpoints) {
    Transcript transcript;
    const GenerationRequest request = client_request(transcript, 91, "Question");
    struct Case { const char* host; bool priority; };
    const Case cases[]{
        {"api.openai.com", true}, {"API.OPENAI.COM.", true},
        {"openrouter.ai", true}, {"OPENROUTER.AI.", true},
        {"api.x.ai", false}, {"api.openai.com.example.test", false},
        {"openrouter.ai.example.test", false},
    };
    for (const ProviderApi api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        for (const bool stream : {false, true}) {
            for (const auto& item : cases) {
                SCOPED_TRACE(item.host);
                SCOPED_TRACE(to_string(api));
                SCOPED_TRACE(stream);
                auto definition = network_definition(443, stream);
                definition.provider.config.host = item.host;
                definition.provider.config.https = true;
                definition.provider.config.api = api;
                definition.provider.config.cache_retention = CacheRetention::off;
                ProviderClient client(shared_definition(std::move(definition)));
                const Json body = Json::parse(client.prepare(request).bytes);
                if (item.priority) {
                    EXPECT_EQ(body.at("service_tier"), "priority");
                } else {
                    EXPECT_FALSE(body.contains("service_tier"));
                }
            }
        }
    }
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
    EXPECT_TRUE(result.retryable);
    mock.join();
}

TEST(ProviderClient, RefusedConnectionIsRetryable) {
    int port;
    { MockHttpServer server({}); port = server.port(); }
    ProviderClient client(shared_definition(network_definition(port, false)));
    Transcript transcript;
    std::atomic_bool cancellation{};
    const auto result = complete(client, client_request(transcript, 9, "Question"),
        transcript, [](GenerationDelta) {}, cancellation);
    EXPECT_EQ(result.outcome, GenerationOutcome::transport_error);
    EXPECT_TRUE(result.retryable);
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
        EXPECT_EQ(result.retryable, test_case.status == 429 || test_case.status >= 500);
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
        EXPECT_FALSE(result.retryable);
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
    for (const auto api : {ProviderApi::chat_completions, ProviderApi::google}) {
        const std::string body = api == ProviderApi::google
            ? "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"Partial\"}]}}]}\n\n"
            : "data: {\"choices\":[{\"delta\":{\"content\":\"Partial\"}}]}\n\n";
        const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
            "Content-Length: " + std::to_string(body.size() + 20)
            + "\r\nConnection: close\r\n\r\n" + body;
        MockHttpServer mock({response});
        mock.start();
        std::atomic_bool cancellation{false};
        auto definition = network_definition(mock.port());
        definition.provider.config.api = api;
        ProviderClient client(shared_definition(definition));
        Transcript transcript;
        const GenerationRequest request = client_request(transcript, 12, "Question");
        std::string output;

        const GenerationResult result = complete(
            client, request, transcript,
            [&output](GenerationDelta delta) { output += delta.text; }, cancellation);

        EXPECT_EQ(result.outcome, GenerationOutcome::transport_error);
        EXPECT_NE(result.message.find("HTTP request failed"), std::string::npos);
        EXPECT_EQ(output, "Partial");
        EXPECT_FALSE(result.retryable);
        mock.join();
    }
}

TEST(ProviderClient, CancelsAnActiveStreamingTransfer) {
    for (const auto api : {ProviderApi::chat_completions, ProviderApi::google}) {
        const std::string body = api == ProviderApi::google
            ? "data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"Partial\"}]}}]}\n\n"
            : "data: {\"choices\":[{\"delta\":{\"content\":\"Partial\"}}]}\n\n";
        const std::string response = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n"
            "Content-Length: " + std::to_string(body.size() + 20)
            + "\r\nConnection: close\r\n\r\n" + body;
        MockHttpServer mock({response}, true);
        mock.start();
        std::atomic_bool cancellation{false};
        auto definition = network_definition(mock.port());
        definition.provider.config.api = api;
        ProviderClient client(shared_definition(definition));
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
    EXPECT_TRUE(provider_endpoint(shared->provider.config).ends_with("/v1/responses"));
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
    EXPECT_TRUE(body["instructions"].get<std::string>().starts_with("Be concise.\n\n"));
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
    EXPECT_TRUE(provider_endpoint(shared->provider.config).ends_with(
        "/api/v1/chat/completions"));
}

TEST(ProviderClient, HandlesNonStreamingResponsesApiResponse) {
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
    const Json body = Json::parse(request_body(mock.requests().front()));
    EXPECT_FALSE(body.contains("tools"));
    EXPECT_FALSE(body.contains("tool_choice"));
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
    test::TestWorkspace workspace;
    const std::filesystem::path database = test::import_test_database(
        workspace.root(), directory / "workspace.sqlite3");
    auto config = WorkspaceConfigStore::open(database);
    ApiKeyStore keys(*config);
    for (const bool legacy_name : {false, true}) {
        SCOPED_TRACE(legacy_name ? "legacy credential name" : "saved key ID");
        CharacterDefinition definition = network_definition(1, false);
        if (legacy_name) {
            definition.provider.config.api_key_env = "OPEN_ROUTER_API_KEY";
        } else {
            definition.provider.config.api_key_id = "api_key_99";
        }
        try {
            (void)ProviderClient(
                shared_definition(std::move(definition)), nullptr, &keys);
            ADD_FAILURE() << "expected missing API key rejection";
        } catch (const std::runtime_error& error) {
            if (!legacy_name) {
                EXPECT_NE(std::string(error.what()).find("api_key_99"), std::string::npos);
            }
        }
    }
}

TEST(ProviderClient, UsesASavedApiKey) {
    const std::filesystem::path directory = std::filesystem::temp_directory_path()
        / ("cha_provider_saved_key_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::filesystem::remove_all(path); }
    } cleanup{directory};
    test::TestWorkspace workspace;
    const std::filesystem::path database = test::import_test_database(
        workspace.root(), directory / "workspace.sqlite3");
    auto config = WorkspaceConfigStore::open(database);
    ApiKeyStore keys(*config);
    const ApiKeyInfo key = keys.create("Router", "saved-secret");
    (void)keys.create("OPEN_ROUTER_API_KEY", "named-secret");

    for (const bool legacy_name : {false, true}) {
        SCOPED_TRACE(legacy_name ? "legacy credential name" : "saved key ID");
        CharacterDefinition definition = network_definition(443, false);
        if (legacy_name) {
            definition.provider.config.api_key_env = "OPEN_ROUTER_API_KEY";
        } else {
            definition.provider.config.api_key_id = key.id;
        }
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
        const std::string authorization = std::string("Authorization: Bearer ")
            + (legacy_name ? "named-secret" : "saved-secret");
        EXPECT_NE(
            std::ranges::find(requests.front().headers, authorization),
            requests.front().headers.end());
    }
}

TEST(ProviderClient, UnauthenticatedProviderOmitsAuthorization) {
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
        provider_endpoint(shared->provider.config),
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
    EXPECT_EQ(body["service_tier"], "priority");
    EXPECT_TRUE(body["stream"]);
    EXPECT_FALSE(body["store"]);
    EXPECT_TRUE(body["instructions"].get<std::string>().starts_with("You are a helpful assistant.\n\n"));
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
    // A network failure is not proof that the saved login is invalid.
    EXPECT_EQ(oauth.status().state, OpenAiOAuthState::connected);
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

// Exercise the full model -> tool -> model exchange through the HTTP seam.
Json search_call(ProviderApi api, std::string id, std::string arguments = R"({"query":"current release"})",
    std::string name = "web_search") {
    if (api == ProviderApi::responses)
        return {{"type", "function_call"}, {"id", "item_" + id}, {"call_id", id},
            {"name", name}, {"arguments", arguments}};
    return {{"id", id}, {"type", "function"},
        {"function", {{"name", name}, {"arguments", arguments}}}};
}

ProviderHttpResponse tool_reply(ProviderApi api, bool stream, Json calls,
    std::string answer = {}) {
    const auto event = [](const Json& value) { return "data: " + value.dump() + "\n\n"; };
    Json usage = api == ProviderApi::responses
        ? Json{{"input_tokens", 10}, {"output_tokens", 2}}
        : Json{{"prompt_tokens", 10}, {"completion_tokens", 2}};
    if (api == ProviderApi::responses) {
        auto output = Json::array();
        if (!calls.empty()) {
            output.push_back({{"type", "reasoning"}, {"id", "reasoning_1"},
                {"summary", Json::array()}, {"encrypted_content", "opaque reasoning"}});
            for (const auto& call : calls) output.push_back(call);
        }
        if (!answer.empty()) output.push_back({{"type", "message"}, {"role", "assistant"},
            {"content", Json::array({Json{{"type", "output_text"}, {"text", answer}}})}});
        Json body{{"status", "completed"}, {"output", output}, {"usage", usage}};
        if (!stream) return {200, "application/json", body.dump()};
        std::string events;
        if (!answer.empty()) {
            events += event({{"type", "response.output_text.delta"}, {"delta", answer.substr(0, 1)}});
            events += event({{"type", "response.output_text.delta"}, {"delta", answer.substr(1)}});
        }
        for (std::size_t index = 0; index < output.size(); ++index)
            events += event({{"type", "response.output_item.done"}, {"output_index", index}, {"item", output[index]}});
        events += event({{"type", "response.completed"}, {"response", body}});
        return {200, "text/event-stream", events};
    }
    Json message{{"role", "assistant"}, {"content", answer.empty() ? Json(nullptr) : Json(answer)}};
    if (!calls.empty()) {
        message["tool_calls"] = calls;
        message["reasoning_content"] = "Need a source.";
    }
    if (!stream) return {200, "application/json", Json{{"choices", Json::array({Json{{"message", message}}})},
        {"usage", usage}}.dump()};
    std::string events;
    const auto delta = [&event](const Json& data) {
        return event({{"choices", Json::array({Json{{"delta", data}}})}});
    };
    if (!answer.empty()) {
        events += delta({{"content", answer.substr(0, 1)}});
        events += delta({{"content", answer.substr(1)}});
    }
    if (!calls.empty()) events += delta({{"reasoning_content", "Need a source."}});
    for (std::size_t index = 0; index < calls.size(); ++index) {
        auto call = calls[index];
        const auto arguments = call["function"]["arguments"].get<std::string>();
        call["function"]["arguments"] = arguments.substr(0, 5);
        call["index"] = index;
        events += delta({{"tool_calls", Json::array({call})}});
        events += delta({{"tool_calls", Json::array({Json{{"index", index},
            {"function", {{"arguments", arguments.substr(5)}}}}})}});
    }
    events += event({{"choices", Json::array()}, {"usage", usage}});
    return {200, "text/event-stream", events + "data: [DONE]\n\n"};
}

TEST(ProviderClientTools, RecordsFailedModelRoundTimeEvenWhenTransportThrows) {
    for (bool throws : {false, true}) {
        auto definition = network_definition(80, false);
        ProviderClient client(shared_definition(definition), nullptr,
            [&](const ProviderHttpRequest&, const auto&) -> ProviderHttpResponse {
                if (throws) throw std::runtime_error("transport failed");
                return {503, "application/json", R"({"error":{"message":"Unavailable"}})"};
            });
        Transcript transcript;
        GenerationStatistics statistics;
        auto payload = client.prepare(client_request(transcript, 1, "Answer"));
        payload.statistics = &statistics;
        EXPECT_NE(client.perform(std::move(payload), [](auto) {}, std::atomic_bool{false}).outcome,
            GenerationOutcome::completed);
        EXPECT_EQ(statistics.model.calls, 1u);
        EXPECT_EQ(statistics.model.failed, 1u);
        EXPECT_GT(statistics.model.duration.count(), 0);
        EXPECT_EQ(statistics.web_search.calls, 0u);
    }
}

TEST(ProviderClientTools, RunsMixedWebCallsInGroupsOfFourAndPreservesOrderedResults) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        for (bool stream : {false, true}) {
            SCOPED_TRACE(std::string(to_string(api)) + (stream ? " streaming" : " JSON"));
            auto definition = network_definition(80, stream);
            definition.provider.config.api = api;
            int requests = 0;
            ProviderClient client(shared_definition(definition), nullptr,
                [&](const ProviderHttpRequest& request, const auto&) {
                    if (++requests == 1) {
                        auto calls = Json::array();
                        for (int index = 0; index < 8; ++index) {
                            const bool read = index % 2 == 0;
                            calls.push_back(search_call(api, std::to_string(index),
                                Json{{read ? "url" : "query", std::to_string(index)}}.dump(),
                                read ? "web_read" : "web_search"));
                        }
                        return tool_reply(api, stream, calls);
                    }
                    const auto body = Json::parse(request.body);
                    const auto& messages = body[api == ProviderApi::responses ? "input" : "messages"];
                    for (int index = 0; index < 8; ++index) {
                        const auto& output = messages[messages.size() - 8 + index];
                        EXPECT_EQ(output[api == ProviderApi::responses ? "call_id" : "tool_call_id"],
                            std::to_string(index));
                        const auto text = output[api == ProviderApi::responses ? "output" : "content"].get<std::string>();
                        if (index == 1) {
                            EXPECT_EQ(Json::parse(text)["error"], "Web search HTTP 429");
                        } else if (index == 2) {
                            EXPECT_EQ(Json::parse(text)["error"],
                                "Page reading failed. Continue without it or try another URL.");
                            EXPECT_EQ(text.find("private details"), std::string::npos);
                        } else {
                            EXPECT_EQ(text, "result " + std::to_string(index));
                        }
                    }
                    return tool_reply(api, stream, Json::array(), "Done");
                });
            std::mutex mutex;
            std::condition_variable changed;
            int started = 0, completed = 0, active = 0, maximum_active = 0;
            std::vector<int> completion_order;
            const auto execute = [&](std::string_view argument, const auto&) -> std::string {
                const int index = std::stoi(std::string(argument));
                const int group = index / 4 * 4;
                std::unique_lock lock(mutex);
                // The next group must wait even when three earlier calls are done.
                EXPECT_GE(completed, group);
                ++started;
                maximum_active = std::max(maximum_active, ++active);
                changed.notify_all();
                EXPECT_TRUE(changed.wait_for(lock, std::chrono::seconds(2), [&] {
                    return started >= group + 4 && completed == group + 3 - index % 4;
                }));
                completion_order.push_back(index);
                ++completed;
                --active;
                changed.notify_all();
                if (index == 1) throw WebToolError("Web search HTTP 429");
                if (index == 2) throw std::runtime_error("private details");
                return "result " + std::string(argument);
            };
            Transcript transcript;
            auto input = client_request(transcript, 1, "Read and search");
            input.web_search_tool = execute;
            input.web_read_tool = [&](auto argument, const auto& cancelled, const auto&) { return execute(argument, cancelled); };
            GenerationStatistics statistics;
            auto payload = client.prepare(input);
            payload.statistics = &statistics;
            const auto result = client.perform(std::move(payload), [](auto) {}, std::atomic_bool{false});
            EXPECT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
            EXPECT_EQ(statistics.model.calls, 2u);
            EXPECT_EQ(statistics.model.failed, 0u);
            EXPECT_GT(statistics.model.duration.count(), 0);
            EXPECT_EQ(statistics.web_search.calls, 4u);
            EXPECT_EQ(statistics.web_search.failed, 1u);
            EXPECT_EQ(statistics.web_read.calls, 4u);
            EXPECT_EQ(statistics.web_read.failed, 1u);
            EXPECT_EQ(statistics.tool_calls_requested, 8u);
            EXPECT_EQ(statistics.tool_calls_rejected, 0u);
            EXPECT_GT(statistics.tools_wall_time.count(), 0);
            EXPECT_GT(statistics.web_search.duration.count(), 0);
            EXPECT_GT(statistics.web_read.duration.count(), 0);
            EXPECT_EQ(requests, 2);
            EXPECT_EQ(started, 8);
            EXPECT_EQ(completed, 8);
            EXPECT_EQ(active, 0);
            EXPECT_EQ(maximum_active, 4);
            EXPECT_EQ(completion_order, (std::vector<int>{3, 2, 1, 0, 7, 6, 5, 4}));
        }
    }
}

TEST(ProviderClientTools, InvalidUnknownAndUnavailableCallsConsumeTheSearchBudget) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        auto definition = network_definition(80, false);
        definition.provider.config.api = api;
        int requests = 0;
        ProviderClient client(shared_definition(definition), nullptr,
            [&](const ProviderHttpRequest& request, const auto&) {
                if (++requests == 1) return tool_reply(api, false, Json::array({
                    search_call(api, "bad", "{"),
                    search_call(api, "unknown", R"({"query":"x"})", "unknown"),
                    search_call(api, "unavailable", R"({"url":"https://example.org"})", "web_read"),
                    search_call(api, "valid"), search_call(api, "excess"), search_call(api, "excess2")}));
                const auto body = Json::parse(request.body);
                EXPECT_FALSE(body.contains("tools"));
                const auto& messages = body[api == ProviderApi::responses ? "input" : "messages"];
                const std::vector<std::string> ids{"bad", "unknown", "unavailable", "valid", "excess", "excess2"};
                for (std::size_t index = 0; index < ids.size(); ++index) {
                    const auto& output = messages[messages.size() - 7 + index];
                    EXPECT_EQ(output[api == ProviderApi::responses ? "call_id" : "tool_call_id"], ids[index]);
                    const auto text = output[api == ProviderApi::responses ? "output" : "content"].get<std::string>();
                    if (index == 3) EXPECT_EQ(text, "results");
                    else if (index >= 4) EXPECT_NE(text.find("Web tool limit reached"), std::string::npos);
                    else EXPECT_TRUE(Json::parse(text).contains("error"));
                }
                return tool_reply(api, false, Json::array(), "Done");
            });
        Transcript transcript;
        auto input = client_request(transcript, 1, "Search");
        std::atomic_int searches{0};
        input.web_search_tool = [&](auto, const auto&) { ++searches; return "results"; };
        const auto result = client.perform(client.prepare(input), [](auto) {}, std::atomic_bool{false});
        EXPECT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
        EXPECT_EQ(searches, 1);
        EXPECT_EQ(requests, 2);
    }
}

TEST(ProviderClientTools, SearchesThenReadsAndSupportsReadingWithoutSearch) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        for (bool stream : {false, true}) {
            for (bool search_enabled : {false, true}) {
                auto definition = network_definition(80, stream);
                definition.provider.config.api = api;
                std::vector<Json> requests;
                ProviderClient client(shared_definition(definition), nullptr,
                    [&](const ProviderHttpRequest& request, const auto&) {
                        requests.push_back(Json::parse(request.body));
                        const auto round = requests.size();
                        if (search_enabled && round == 1) return tool_reply(api, stream,
                            Json::array({search_call(api, "search")}));
                        if (round == (search_enabled ? 2u : 1u)) return tool_reply(api, stream,
                            Json::array({search_call(api, "read", R"({"url":"https://example.org/page"})", "web_read")}));
                        return tool_reply(api, stream, Json::array(), "Answer from page.");
                    });
                Transcript transcript;
                auto input = client_request(transcript, 1, "Read the page");
                std::atomic_int searches{0}, reads{0};
                if (search_enabled) input.web_search_tool = [&](auto, const auto&) {
                    ++searches;
                    return R"({"url":"https://example.org/page"})";
                };
                input.web_read_tool = [&](std::string_view url, const auto&, const auto&) {
                    EXPECT_EQ(url, "https://example.org/page");
                    ++reads;
                    return R"({"markdown":"# Page\nEvidence"})";
                };
                std::string answer;
                const auto result = client.perform(client.prepare(input), [&](GenerationDelta delta) {
                    if (delta.kind == GenerationDeltaKind::answer) answer += delta.text;
                }, std::atomic_bool{false});
                EXPECT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
                EXPECT_EQ(searches, search_enabled ? 1 : 0);
                EXPECT_EQ(reads, 1);
                EXPECT_EQ(answer, "Answer from page.");
                ASSERT_EQ(requests.size(), search_enabled ? 3u : 2u);
                const auto& tools = requests.front()["tools"];
                ASSERT_EQ(tools.size(), search_enabled ? 2u : 1u);
                const auto& reader = api == ProviderApi::responses ? tools.back() : tools.back()["function"];
                EXPECT_EQ(reader["name"], "web_read");
                EXPECT_EQ(reader["parameters"]["required"], Json::array({"url"}));
                const auto& messages = requests.back()[api == ProviderApi::responses ? "input" : "messages"];
                const auto output = Json::parse(messages.back()[api == ProviderApi::responses ? "output" : "content"].get<std::string>());
                EXPECT_EQ(output["markdown"], "# Page\nEvidence");
                EXPECT_EQ(result.usage.input_tokens, requests.size() * 10u);
            }
        }
    }
}

TEST(ProviderClientTools, ReadingReturnsErrorsAndStopsOnCancellation) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        for (bool cancel : {false, true}) {
            auto definition = network_definition(80, false);
            definition.provider.config.api = api;
            int rounds = 0;
            std::atomic_int reads{0};
            ProviderClient client(shared_definition(definition), nullptr,
                [&](const ProviderHttpRequest& request, const auto&) {
                    if (++rounds == 1) return tool_reply(api, false, Json::array({
                        search_call(api, "bad", R"({"url":42})", "web_read"),
                        search_call(api, "unavailable"),
                        search_call(api, "read", R"({"url":"https://example.org"})", "web_read")}));
                    const auto body = Json::parse(request.body);
                    const auto& messages = body[api == ProviderApi::responses ? "input" : "messages"];
                    for (std::size_t i = messages.size() - 3; i < messages.size(); ++i) {
                        const auto output = Json::parse(messages[i][api == ProviderApi::responses ? "output" : "content"].get<std::string>());
                        EXPECT_TRUE(output.contains("error"));
                        EXPECT_EQ(output.dump().find("private-key"), std::string::npos);
                    }
                    return tool_reply(api, false, Json::array(), "Unavailable.");
                });
            Transcript transcript;
            auto input = client_request(transcript, 1, "Read");
            std::atomic_bool cancelled{false};
            input.web_read_tool = [&](auto, const auto&, const auto&) -> std::string {
                ++reads;
                if (cancel) { cancelled.store(true); return {}; }
                throw std::runtime_error("private-key");
            };
            GenerationStatistics statistics;
            auto payload = client.prepare(input);
            payload.statistics = &statistics;
            const auto result = client.perform(std::move(payload), [](auto) {}, cancelled);
            EXPECT_EQ(statistics.web_read.calls, 1u);
            EXPECT_EQ(statistics.web_read.failed, cancel ? 0u : 1u);
            EXPECT_EQ(statistics.web_read.cancelled, cancel ? 1u : 0u);
            EXPECT_EQ(statistics.tool_calls_rejected, 2u);
            EXPECT_GT(statistics.tools_wall_time.count(), 0);
            EXPECT_EQ(reads, 1);
            EXPECT_EQ(rounds, cancel ? 1 : 2);
            EXPECT_EQ(result.outcome, cancel ? GenerationOutcome::cancelled : GenerationOutcome::completed);
        }
    }
}

TEST(ProviderClientTools, ReadingReportsPaymentFailureWithoutExposingResponseBodies) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        const auto body = Json{{"success", false},
            {"error", "Account balance not enough (uid: private-account-id)"}}.dump();
        MockHttpServer server({"HTTP/1.1 402 Payment Required\r\nContent-Type: application/json\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body});
        server.start();
        const auto endpoint = "http://127.0.0.1:" + std::to_string(server.port());
        auto definition = network_definition(80, false);
        definition.provider.config.api = api;
        int rounds = 0;
        ProviderClient client(shared_definition(definition), nullptr,
            [&](const ProviderHttpRequest& request, const auto&) {
                if (++rounds == 1) return tool_reply(api, false, Json::array({
                    search_call(api, "read", R"({"url":"https://example.org"})", "web_read")}));
                const auto sent = Json::parse(request.body);
                const auto& message = sent[api == ProviderApi::responses ? "input" : "messages"].back();
                const auto output = Json::parse(message[api == ProviderApi::responses ? "output" : "content"].get<std::string>());
                EXPECT_EQ(output["error"], "Page reading HTTP 402");
                EXPECT_EQ(output.dump().find("private-account-id"), std::string::npos);
                EXPECT_EQ(output.dump().find("private-key"), std::string::npos);
                return tool_reply(api, false, Json::array(), "The page reading account needs credit.");
            });
        Transcript transcript;
        auto input = client_request(transcript, 1, "Read");
        input.web_read_tool = [&](std::string_view url, const std::atomic_bool& cancelled, const auto&) {
            return read_firecrawl(url, "private-key", cancelled, endpoint);
        };
        const auto result = client.perform(client.prepare(input), [](auto) {}, std::atomic_bool{false});
        server.join();
        EXPECT_EQ(rounds, 2);
        EXPECT_EQ(result.outcome, GenerationOutcome::completed);
    }
}

TEST(ProviderClientTools, ReportsConcurrentSearchReadFallbackAndFailures) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        auto definition = network_definition(80, false);
        definition.provider.config.api = api;
        int rounds = 0;
        ProviderClient client(shared_definition(definition), nullptr,
            [&](const ProviderHttpRequest&, const auto&) {
                if (++rounds == 1) return tool_reply(api, false, Json::array({
                    search_call(api, "search", R"({"query":"evidence"})"),
                    search_call(api, "read", R"({"url":"https://example.org"})", "web_read")}));
                return tool_reply(api, false, Json::array(), "Answer");
            });
        Transcript transcript;
        auto input = client_request(transcript, 1, "Research");
        input.web_search_tool = [](auto, const auto&) -> std::string { throw WebToolError("Search failed"); };
        input.web_read_tool = [](auto, const auto&, const std::function<void()>& fallback) {
            fallback();
            return "Page content";
        };
        auto payload = client.prepare(input);
        std::mutex mutex;
        std::vector<GenerationToolProgress> events;
        payload.tool_progress = [&](GenerationToolProgress progress) {
            std::lock_guard lock(mutex);
            events.push_back(std::move(progress));
        };
        EXPECT_EQ(client.perform(std::move(payload), [](auto) {}, std::atomic_bool{false}).outcome,
            GenerationOutcome::completed);
        ASSERT_EQ(events.size(), 5u);
        std::vector<ToolProgressAction> search, read;
        for (const auto& event : events) {
            (event.tool == "web_read" ? read : search).push_back(event.action);
            EXPECT_EQ(event.argument, event.tool == "web_read" ? "https://example.org" : "evidence");
            EXPECT_EQ(event.id, event.tool == "web_read" ? 2u : 1u);
        }
        EXPECT_EQ(search, (std::vector{ToolProgressAction::started, ToolProgressAction::failed}));
        EXPECT_EQ(read, (std::vector{ToolProgressAction::started, ToolProgressAction::retrying, ToolProgressAction::completed}));
    }
}

TEST(ProviderClientTools, ReadingStopsAtTheSharedWebToolLimit) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        auto definition = network_definition(80, false);
        definition.provider.config.api = api;
        int rounds = 0;
        std::atomic_int reads{0};
        ProviderClient client(shared_definition(definition), nullptr,
            [&](const ProviderHttpRequest& request, const auto&) {
                if (++rounds == 1) {
                    auto calls = Json::array();
                    for (int i = 0; i < 18; ++i) calls.push_back(search_call(api,
                        "read" + std::to_string(i), R"({"url":"https://example.org"})", "web_read"));
                    return tool_reply(api, false, calls);
                }
                const auto body = Json::parse(request.body);
                EXPECT_FALSE(body.contains("tools"));
                const auto instructions = (api == ProviderApi::responses ? body["instructions"]
                    : body["messages"][0]["content"]).get<std::string>();
                EXPECT_NE(instructions.find("Web page reading is unavailable for this request."),
                    std::string::npos);
                EXPECT_EQ(instructions.find("Web page reading is available for this request."),
                    std::string::npos);
                const auto& messages = body[api == ProviderApi::responses ? "input" : "messages"];
                int outputs = 0;
                for (const auto& message : messages)
                    if (api == ProviderApi::responses ? message.value("type", "") == "function_call_output"
                        : message.value("role", "") == "tool") ++outputs;
                EXPECT_EQ(outputs, 18);
                return tool_reply(api, false, Json::array(), "Done.");
            });
        Transcript transcript;
        auto input = client_request(transcript, 1, "Read");
        input.web_read_tool = [&](auto, const auto&, const auto&) { ++reads; return "{}"; };
        const auto result = client.perform(client.prepare(input), [](auto) {}, std::atomic_bool{false});
        EXPECT_EQ(result.outcome, GenerationOutcome::completed);
        EXPECT_EQ(reads, 16);
        EXPECT_EQ(rounds, 2);
    }
}

TEST(ProviderClientTools, SearchesAndContinuesBothProtocolsWithStreamingAndTokenTotals) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        for (bool stream : {false, true}) {
            SCOPED_TRACE(std::string(to_string(api)) + (stream ? " streaming" : " JSON"));
            auto definition = network_definition(80, stream);
            definition.provider.config.api = api;
            std::vector<Json> requests;
            ProviderClient client(shared_definition(definition), nullptr,
                [&](const ProviderHttpRequest& request, const std::atomic_bool&) {
                    requests.push_back(Json::parse(request.body));
                    if (requests.size() == 1) return tool_reply(api, stream,
                        Json::array({search_call(api, "first"), search_call(api, "second")}));
                    return tool_reply(api, stream, Json::array(), "The release is ready.");
                });
            Transcript transcript;
            auto input = client_request(transcript, 1, "What is current?");
            std::atomic_int searches{0};
            input.web_search_tool = [&](std::string_view query, const std::atomic_bool&) {
                EXPECT_EQ(query, "current release");
                ++searches;
                return R"({"sources":[{"url":"https://example.test/release","title":"Release"}]})";
            };
            std::string answer;
            auto result = client.perform(client.prepare(input), [&](GenerationDelta delta) {
                if (delta.kind == GenerationDeltaKind::answer) answer += delta.text;
            }, std::atomic_bool{false});
            EXPECT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
            EXPECT_EQ(searches, 2);
            EXPECT_EQ(answer, "The release is ready.");
            EXPECT_EQ(result.usage.input_tokens, 20u);
            EXPECT_EQ(result.usage.output_tokens, 4u);
            ASSERT_EQ(requests.size(), 2u);
            EXPECT_EQ(requests[0]["tools"].size(), 1u);
            EXPECT_EQ(requests[1]["tool_choice"], "auto");
            const auto& messages = requests[1][api == ProviderApi::responses ? "input" : "messages"];
            const auto& first = messages[messages.size() - 2];
            const auto& second = messages.back();
            if (api == ProviderApi::responses) {
                EXPECT_EQ(requests[0]["include"], Json::array({"reasoning.encrypted_content"}));
                EXPECT_EQ(first["call_id"], "first");
                EXPECT_EQ(second["call_id"], "second");
                EXPECT_EQ(first["type"], "function_call_output");
                EXPECT_EQ(messages[messages.size() - 5]["encrypted_content"], "opaque reasoning");
            } else {
                EXPECT_EQ(first["role"], "tool");
                EXPECT_EQ(first["tool_call_id"], "first");
                EXPECT_EQ(second["tool_call_id"], "second");
                EXPECT_EQ(messages[messages.size() - 3]["reasoning_content"], "Need a source.");
            }
            EXPECT_NE(first.dump().find("https://example.test/release"), std::string::npos);
        }
    }
}

TEST(ProviderClientTools, SeparatesAnswerTextAcrossToolRounds) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        for (bool stream : {false, true}) {
            for (const auto& rounds : std::vector<std::vector<std::string>>{
                    {"I'll check the latest news.", "According to Reuters..."},
                    {"I'll check the latest news.", "", "According to Reuters..."},
                    {"", "", "According to Reuters..."},
                    {"I'll check the latest news.", "I'll check another source.", "According to Reuters..."}}) {
                SCOPED_TRACE(std::string(to_string(api)) + (stream ? " streaming" : " JSON"));
                SCOPED_TRACE(Json(rounds).dump());
                auto definition = network_definition(80, stream);
                definition.provider.config.api = api;
                std::size_t requests = 0;
                ProviderClient client(shared_definition(definition), nullptr,
                    [&](const ProviderHttpRequest&, const std::atomic_bool&) {
                        const auto& text = rounds.at(requests++);
                        const auto calls = requests < rounds.size()
                            ? Json::array({search_call(api, "call" + std::to_string(requests))})
                            : Json::array();
                        return tool_reply(api, stream, calls, text);
                    });
                Transcript transcript;
                auto input = client_request(transcript, 1, "What is the latest news?");
                std::string answer;
                input.web_search_tool = [&](auto, const auto&) {
                    // The preamble remains visible while the search runs.
                    if (requests == 1) {
                        EXPECT_EQ(answer, rounds.front());
                    }
                    return "[]";
                };
                const auto result = client.perform(client.prepare(input), [&](GenerationDelta delta) {
                    if (delta.kind == GenerationDeltaKind::answer) answer += delta.text;
                }, std::atomic_bool{false});
                EXPECT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
                EXPECT_EQ(requests, rounds.size());
                std::string expected;
                for (const auto& text : rounds) {
                    if (text.empty()) continue;
                    if (!expected.empty()) expected += "\n\n";
                    expected += text;
                }
                EXPECT_EQ(answer, expected);
            }
        }
    }
}

TEST(ProviderClientTools, DoesNotAppendASeparatorWhenContinuationFails) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        for (bool stream : {false, true}) {
            auto definition = network_definition(80, stream);
            definition.provider.config.api = api;
            int requests = 0;
            ProviderClient client(shared_definition(definition), nullptr,
                [&](const ProviderHttpRequest&, const std::atomic_bool&) {
                    if (++requests == 1) return tool_reply(api, stream,
                        Json::array({search_call(api, "call")}), "I'll check the latest news.");
                    return ProviderHttpResponse{500, "application/json", R"({"error":"unavailable"})"};
                });
            Transcript transcript;
            auto input = client_request(transcript, 1, "What is the latest news?");
            input.web_search_tool = [](auto, const auto&) { return "[]"; };
            std::string answer;
            const auto result = client.perform(client.prepare(input), [&](GenerationDelta delta) {
                if (delta.kind == GenerationDeltaKind::answer) answer += delta.text;
            }, std::atomic_bool{false});
            EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
            EXPECT_EQ(answer, "I'll check the latest news.");
        }
    }
}

TEST(ProviderClientTools, ReturnsErrorsForBadArgumentsUnknownToolsAndSearchFailures) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        auto definition = network_definition(80, false);
        definition.provider.config.api = api;
        std::vector<Json> requests;
        ProviderClient client(shared_definition(definition), nullptr,
            [&](const ProviderHttpRequest& request, const std::atomic_bool&) {
                requests.push_back(Json::parse(request.body));
                if (requests.size() == 1) return tool_reply(api, false, Json::array({
                    search_call(api, "bad", R"({"query":42})"),
                    search_call(api, "unknown", R"({"query":"x"})", "other_tool"),
                    search_call(api, "failed")}));
                return tool_reply(api, false, Json::array(), "Search is unavailable.");
            });
        Transcript transcript;
        auto input = client_request(transcript, 1, "Search");
        std::atomic_int searches{0};
        input.web_search_tool = [&](auto, const auto&) -> std::string {
            ++searches;
            throw std::runtime_error("private credential details");
        };
        GenerationStatistics statistics;
        auto payload = client.prepare(input);
        payload.statistics = &statistics;
        const auto result = client.perform(std::move(payload), [](auto) {}, std::atomic_bool{false});
        EXPECT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
        EXPECT_EQ(statistics.web_search.calls, 1u);
        EXPECT_EQ(statistics.web_search.failed, 1u);
        EXPECT_EQ(statistics.tool_calls_requested, 3u);
        EXPECT_EQ(statistics.tool_calls_rejected, 2u);
        EXPECT_EQ(searches, 1);
        ASSERT_EQ(requests.size(), 2u);
        const auto& messages = requests.back()[api == ProviderApi::responses ? "input" : "messages"];
        for (std::size_t i = messages.size() - 3; i < messages.size(); ++i) {
            auto output = Json::parse(messages[i][api == ProviderApi::responses ? "output" : "content"].get<std::string>());
            EXPECT_TRUE(output.contains("error"));
            EXPECT_EQ(output.dump().find("private credential"), std::string::npos);
        }
    }
}

TEST(ProviderClientTools, CapsSearchesThenAllowsAFinalAnswer) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        auto definition = network_definition(80, true);
        definition.provider.config.api = api;
        int requests = 0;
        ProviderClient client(shared_definition(definition), nullptr,
            [&](const ProviderHttpRequest& request, const std::atomic_bool&) {
                ++requests;
                if (requests <= 4) return tool_reply(api, true, Json::array({search_call(api, "call" + std::to_string(requests))}));
                EXPECT_FALSE(Json::parse(request.body).contains("tool_choice"));
                return tool_reply(api, true, Json::array(), "Done.");
            });
        Transcript transcript;
        auto input = client_request(transcript, 1, "Search");
        std::atomic_int searches{0};
        input.web_search_tool = [&](auto, const auto&) { ++searches; return "[]"; };
        const auto result = client.perform(client.prepare(input), [](auto) {}, std::atomic_bool{false});
        EXPECT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
        EXPECT_EQ(searches, 4);
        EXPECT_EQ(requests, 5);
        EXPECT_EQ(result.usage.input_tokens, 50u);
    }
}

TEST(ProviderClientTools, RemovesToolsAndRequestsAnAnswerWhenSearchLimitIsReached) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        for (bool stream : {false, true}) {
            auto definition = network_definition(80, stream);
            definition.provider.config.api = api;
            int requests = 0;
            ProviderClient client(shared_definition(definition), nullptr,
                [&](const ProviderHttpRequest& request, const std::atomic_bool&) {
                    ++requests;
                    const auto body = Json::parse(request.body);
                    if (requests <= 2) {
                        EXPECT_TRUE(body.contains("tools"));
                        return tool_reply(api, stream, Json::array({
                            search_call(api, "first" + std::to_string(requests)),
                            search_call(api, "second" + std::to_string(requests))}));
                    }
                    // Some models keep calling tools while definitions are present,
                    // even when tool_choice is none.
                    EXPECT_FALSE(body.contains("tools"));
                    if (body.contains("tools"))
                        return tool_reply(api, stream, Json::array({search_call(api, "again")}));
                    EXPECT_FALSE(body.contains("tool_choice"));
                    const auto instructions = (api == ProviderApi::responses ? body["instructions"]
                        : body["messages"][0]["content"]).get<std::string>();
                    EXPECT_NE(instructions.find("Web search is unavailable for this request."),
                        std::string::npos);
                    EXPECT_EQ(instructions.find("Web search is available for this request."),
                        std::string::npos);
                    EXPECT_EQ(instructions.find("<tool_availability>"),
                        instructions.rfind("<tool_availability>"));
                    const auto& messages = body[api == ProviderApi::responses ? "input" : "messages"];
                    EXPECT_EQ(messages.back()["role"], "user");
                    EXPECT_NE(messages.back()["content"].get<std::string>().find("Answer"), std::string::npos);
                    const auto& output = messages[messages.size() - 2];
                    EXPECT_EQ(output[api == ProviderApi::responses ? "call_id" : "tool_call_id"], "second2");
                    EXPECT_EQ(output[api == ProviderApi::responses ? "output" : "content"], "Search results");
                    return tool_reply(api, stream, Json::array(), "Here is what I could verify.");
                });
            Transcript transcript;
            auto input = client_request(transcript, 1, "Find five news articles");
            std::atomic_int searches{0};
            input.web_search_tool = [&](auto, const auto&) { ++searches; return "Search results"; };
            std::string answer;
            const auto result = client.perform(client.prepare(input), [&](GenerationDelta delta) {
                if (delta.kind == GenerationDeltaKind::answer) answer += delta.text;
            }, std::atomic_bool{false});
            EXPECT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
            EXPECT_EQ(searches, 4);
            EXPECT_EQ(requests, 3);
            EXPECT_EQ(answer, "Here is what I could verify.");
            EXPECT_EQ(result.usage.input_tokens, 30u);
        }
    }
}

TEST(ProviderClientTools, CancellationDuringSearchStopsTheLoop) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        auto definition = network_definition(80, false);
        definition.provider.config.api = api;
        int requests = 0;
        ProviderClient client(shared_definition(definition), nullptr,
            [&](const auto&, const auto&) {
                ++requests;
                auto calls = Json::array();
                for (int index = 0; index < 8; ++index)
                    calls.push_back(search_call(api, std::to_string(index)));
                return tool_reply(api, false, calls);
            });
        Transcript transcript;
        auto input = client_request(transcript, 1, "Search");
        std::atomic_bool cancelled{false};
        std::mutex mutex;
        std::condition_variable changed;
        int started = 0, finished = 0;
        bool release = false;
        input.web_search_tool = [&](auto, const auto& cancellation) {
            std::unique_lock lock(mutex);
            ++started;
            changed.notify_all();
            EXPECT_TRUE(changed.wait_for(lock, std::chrono::seconds(2), [&] { return release; }));
            EXPECT_TRUE(cancellation.load());
            ++finished;
            return "";
        };
        // Reading raises the shared budget so the response contains a later group.
        input.web_read_tool = [](auto, const auto&, const auto&) { return "unused"; };
        auto worker = std::async(std::launch::async, [&] {
            auto result = client.perform(client.prepare(input), [](auto) {}, cancelled);
            std::lock_guard lock(mutex);
            EXPECT_EQ(finished, 4);
            return result;
        });
        {
            std::unique_lock lock(mutex);
            EXPECT_TRUE(changed.wait_for(lock, std::chrono::seconds(2), [&] { return started == 4; }));
            cancelled.store(true);
            release = true;
        }
        changed.notify_all();
        const auto result = worker.get();
        EXPECT_EQ(result.outcome, GenerationOutcome::cancelled);
        EXPECT_EQ(started, 4);
        EXPECT_EQ(finished, 4);
        EXPECT_EQ(requests, 1);
        EXPECT_EQ(result.usage.input_tokens, 10u);
        EXPECT_EQ(result.usage.output_tokens, 2u);
    }
}

TEST(ProviderClientTools, RejectsUnsolicitedCallsWhenDisabled) {
    auto definition = network_definition(80, false);
    ProviderClient client(shared_definition(definition), nullptr,
        [&](const ProviderHttpRequest& request, const auto&) {
            EXPECT_FALSE(Json::parse(request.body).contains("tools"));
            return tool_reply(ProviderApi::chat_completions, false,
                Json::array({search_call(ProviderApi::chat_completions, "call")}));
        });
    Transcript transcript;
    const auto result = client.perform(client.prepare(client_request(transcript, 1, "Hello")),
        [](auto) {}, std::atomic_bool{false});
    EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
}

TEST(ProviderClientTools, RejectsIncompleteToolStreamsBeforeSearching) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        auto definition = network_definition(80, true);
        definition.provider.config.api = api;
        ProviderClient client(shared_definition(definition), nullptr,
            [&](const auto&, const auto&) {
                auto reply = tool_reply(api, true, Json::array({search_call(api, "call")}));
                const auto terminal = reply.body.rfind("data: ");
                reply.body.erase(terminal);
                return reply;
            });
        Transcript transcript;
        auto input = client_request(transcript, 1, "Search");
        std::atomic_int searches{0};
        input.web_search_tool = [&](auto, const auto&) { ++searches; return "[]"; };
        const auto result = client.perform(client.prepare(input), [](auto) {}, std::atomic_bool{false});
        EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
        EXPECT_EQ(searches, 0);
    }
}

TEST(ProviderClientTools, ReturnsAnOutputForEveryCallWhenBatchExceedsSearchLimit) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        auto definition = network_definition(80, false);
        definition.provider.config.api = api;
        int requests = 0;
        ProviderClient client(shared_definition(definition), nullptr,
            [&](const ProviderHttpRequest& request, const auto&) {
                ++requests;
                if (requests == 1) {
                    auto calls = Json::array();
                    for (int i = 0; i < 6; ++i) calls.push_back(search_call(api, "call" + std::to_string(i)));
                    return tool_reply(api, false, calls);
                }
                const auto body = Json::parse(request.body);
                EXPECT_FALSE(body.contains("tool_choice"));
                const auto& messages = body[api == ProviderApi::responses ? "input" : "messages"];
                EXPECT_FALSE(body.contains("tools"));
                EXPECT_EQ(messages.back()["role"], "user");
                for (int i = 0; i < 6; ++i) {
                    const auto& output = messages[messages.size() - 7 + i];
                    EXPECT_EQ(output[api == ProviderApi::responses ? "call_id" : "tool_call_id"],
                        "call" + std::to_string(i));
                    if (i >= 4) {
                        EXPECT_NE(output.dump().find("Web tool limit reached"), std::string::npos);
                    }
                }
                // A provider that still requests tools must not create an endless loop.
                return tool_reply(api, false, Json::array({search_call(api, "one_more")}));
            });
        Transcript transcript;
        auto input = client_request(transcript, 1, "Search");
        std::atomic_int searches{0};
        input.web_search_tool = [&](auto, const auto&) { ++searches; return "[]"; };
        const auto result = client.perform(client.prepare(input), [](auto) {}, std::atomic_bool{false});
        EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
        EXPECT_EQ(searches, 4);
        EXPECT_EQ(requests, 2);
        EXPECT_EQ(result.message, "Model kept requesting web search after the search limit");
    }
}

TEST(ProviderClientTools, SupportsOrdinaryResponsesWithNullToolCalls) {
    auto definition = network_definition(80, false);
    ProviderClient client(shared_definition(definition), nullptr,
        [](const auto&, const auto&) {
            return ProviderHttpResponse{200, "application/json",
                R"({"choices":[{"message":{"role":"assistant","content":"Hello","tool_calls":null}}]})"};
        });
    Transcript transcript;
    const auto result = client.perform(client.prepare(client_request(transcript, 1, "Hello")),
        [](auto) {}, std::atomic_bool{false});
    EXPECT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
}

TEST(ProviderClientTools, ValidatesToolDataOnlyWhenOnDemandSearchWasOffered) {
    struct Case {
        ProviderApi api;
        bool stream;
        std::string body;
    };
    const std::vector<Case> cases{
        {ProviderApi::chat_completions, true,
            "data: {\"choices\":[{\"delta\":{\"content\":\"Answer\",\"reasoning_details\":[{\"index\":32,\"text\":\"unused\"}]}}]}\n\n"
            "data: [DONE]\n\n"},
        {ProviderApi::chat_completions, true,
            "data: {\"choices\":[{\"delta\":{\"content\":\"Answer\",\"tool_calls\":[{\"function\":{\"name\":\"web_search\"}}]}}]}\n\n"
            "data: [DONE]\n\n"},
        {ProviderApi::responses, true,
            "data: {\"type\":\"response.output_item.done\",\"item\":{\"type\":\"function_call\"}}\n\n"
            "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Answer\"}\n\n"
            "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n\n"},
        {ProviderApi::chat_completions, false,
            R"({"choices":[{"message":{"content":"Answer","tool_calls":"unused"}}]})"},
        {ProviderApi::responses, false,
            R"({"status":"completed","output":[{"type":"function_call"},{"type":"message","role":"assistant","content":[{"type":"output_text","text":"Answer"}]}]})"},
    };
    for (const auto& item : cases) {
        for (bool enabled : {false, true}) {
            SCOPED_TRACE(item.body);
            SCOPED_TRACE(enabled);
            auto definition = network_definition(80, item.stream);
            definition.provider.config.api = item.api;
            // Provider-hosted search alone must not enable application tool parsing.
            if (item.api == ProviderApi::responses)
                definition.provider.config.web_search = WebSearchMode::automatic;
            int requests = 0;
            ProviderClient client(shared_definition(definition), nullptr,
                [&](const ProviderHttpRequest&, const std::atomic_bool&) {
                    ++requests;
                    return ProviderHttpResponse{200,
                        item.stream ? "text/event-stream" : "application/json", item.body};
                });
            Transcript transcript;
            auto input = client_request(transcript, 1, "Hello");
            std::atomic_int searches{0};
            if (enabled) input.web_search_tool = [&](auto, const auto&) { ++searches; return "[]"; };
            std::string answer;
            const auto result = client.perform(client.prepare(input), [&](GenerationDelta delta) {
                if (delta.kind == GenerationDeltaKind::answer) answer += delta.text;
            }, std::atomic_bool{false});
            EXPECT_EQ(result.outcome, enabled ? GenerationOutcome::protocol_error : GenerationOutcome::completed)
                << result.message;
            if (!enabled) {
                EXPECT_EQ(answer, "Answer");
            }
            EXPECT_EQ(searches, 0);
            EXPECT_EQ(requests, 1);
        }
    }
}

TEST(ProviderClientTools, PreparedPayloadOwnsItsSearchFunction) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        auto definition = network_definition(80, false);
        definition.provider.config.api = api;
        int requests = 0;
        ProviderClient client(shared_definition(definition), nullptr,
            [&](const ProviderHttpRequest&, const std::atomic_bool&) {
                ++requests;
                if (requests == 1 || requests == 3)
                    return tool_reply(api, false, Json::array({search_call(api, "call")}));
                return tool_reply(api, false, Json::array(), "Done");
            });
        Transcript transcript;
        auto input = client_request(transcript, 1, "Search");
        std::atomic_int first_searches{0};
        std::atomic_int second_searches{0};
        input.web_search_tool = [&](auto, const auto&) { ++first_searches; return "[]"; };
        auto first = client.prepare(input);
        input.web_search_tool = [&](auto, const auto&) { ++second_searches; return "[]"; };
        auto second = client.prepare(input);
        input.web_search_tool = {};
        auto plain = client.prepare(input);
        for (auto* payload : {&first, &second, &plain}) {
            const auto result = client.perform(std::move(*payload), [](auto) {}, std::atomic_bool{false});
            EXPECT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
        }
        EXPECT_EQ(first_searches, 1);
        EXPECT_EQ(second_searches, 1);
        EXPECT_EQ(requests, 5);
    }
}

TEST(ProviderClientTools, PreservesGeminiToolCallsAndSignaturesWhenContinuingAfterSearch) {
    for (bool stream : {false, true}) {
        for (bool indexed : {false, true}) {
            for (bool separate_events : {false, true}) {
                SCOPED_TRACE(stream);
                SCOPED_TRACE(indexed);
                SCOPED_TRACE(separate_events);
                auto calls = Json::array({search_call(ProviderApi::chat_completions, "first"),
                    search_call(ProviderApi::chat_completions, "second")});
                calls[0]["extra_content"] = {{"google", {{"thought_signature", "opaque signature"}}}};
                int requests = 0;
                ProviderClient client(shared_definition(network_definition(80, stream)), nullptr,
                    [&](const ProviderHttpRequest& request, const std::atomic_bool&) {
                        if (++requests == 1) {
                            if (!stream) return tool_reply(ProviderApi::chat_completions, false, calls);
                            auto parts = calls;
                            if (indexed) {
                                for (std::size_t i = 0; i < parts.size(); ++i) parts[i]["index"] = i;
                            }
                            const auto event = [](const Json& parts) {
                                return "data: " + Json{{"choices", Json::array({Json{{"delta",
                                    {{"role", "assistant"}, {"tool_calls", parts}}}}})}}.dump() + "\n\n";
                            };
                            std::string body;
                            if (separate_events) {
                                for (const auto& part : parts) body += event(Json::array({part}));
                            } else body = event(parts);
                            return ProviderHttpResponse{200, "text/event-stream", body + "data: [DONE]\n\n"};
                        }
                        const auto messages = Json::parse(request.body)["messages"];
                        EXPECT_EQ(messages[messages.size() - 3]["tool_calls"], calls);
                        EXPECT_EQ(messages[messages.size() - 2]["tool_call_id"], "first");
                        EXPECT_EQ(messages[messages.size() - 1]["tool_call_id"], "second");
                        return tool_reply(ProviderApi::chat_completions, stream, Json::array(), "Done");
                    });
                Transcript transcript;
                auto input = client_request(transcript, 1, "Search");
                std::atomic_int searches{0};
                input.web_search_tool = [&](std::string_view query, const auto&) {
                    EXPECT_EQ(query, "current release");
                    ++searches;
                    return "[]";
                };
                std::string answer;
                const auto result = client.perform(client.prepare(input), [&](GenerationDelta delta) {
                    if (delta.kind == GenerationDeltaKind::answer) answer += delta.text;
                }, std::atomic_bool{false});
                EXPECT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
                EXPECT_EQ(requests, 2);
                EXPECT_EQ(searches, 2);
                EXPECT_EQ(answer, "Done");
            }
        }
    }
}

TEST(ProviderClientTools, PreservesStreamedReasoningSummaryInContinuation) {
    int requests = 0;
    ProviderClient client(shared_definition(network_definition(80, true)), nullptr,
        [&](const ProviderHttpRequest& request, const std::atomic_bool&) {
            if (++requests == 1) {
                auto reply = tool_reply(ProviderApi::chat_completions, true,
                    Json::array({search_call(ProviderApi::chat_completions, "call")}));
                reply.body =
                    "data: {\"choices\":[{\"delta\":{\"reasoning_details\":[{\"index\":0,\"type\":\"reasoning.summary\",\"summary\":\"First \"}]}}]}\n\n"
                    "data: {\"choices\":[{\"delta\":{\"reasoning_details\":[{\"index\":0,\"type\":\"reasoning.summary\",\"summary\":\"second\"}]}}]}\n\n"
                    + reply.body;
                return reply;
            }
            const auto messages = Json::parse(request.body)["messages"];
            EXPECT_EQ(messages[messages.size() - 2]["reasoning_details"][0]["summary"], "First second");
            return tool_reply(ProviderApi::chat_completions, true, Json::array(), "Done");
        });
    Transcript transcript;
    auto input = client_request(transcript, 1, "Search");
    input.web_search_tool = [](auto, const auto&) { return "[]"; };
    const auto result = client.perform(client.prepare(input), [](auto) {}, std::atomic_bool{false});
    EXPECT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
    EXPECT_EQ(requests, 2);
}

TEST(ProviderClientTools, RejectsIncompleteChatToolCallsInBothResponseForms) {
    for (bool stream : {false, true}) {
        for (const auto* reason : {"length", "content_filter"}) {
            ProviderClient client(shared_definition(network_definition(80, stream)), nullptr,
                [&](const ProviderHttpRequest&, const std::atomic_bool&) {
                    auto reply = tool_reply(ProviderApi::chat_completions, stream,
                        Json::array({search_call(ProviderApi::chat_completions, "call")}));
                    if (stream) {
                        const auto event = "data: " + Json{{"choices", Json::array({Json{
                            {"delta", Json::object()}, {"finish_reason", reason}}})}}.dump() + "\n\n";
                        reply.body.insert(reply.body.find("data: [DONE]"), event);
                    } else {
                        auto body = Json::parse(reply.body);
                        body["choices"][0]["finish_reason"] = reason;
                        reply.body = body.dump();
                    }
                    return reply;
                });
            Transcript transcript;
            auto input = client_request(transcript, 1, "Search");
            std::atomic_int searches{0};
            input.web_search_tool = [&](auto, const auto&) { ++searches; return "[]"; };
            const auto result = client.perform(client.prepare(input), [](auto) {}, std::atomic_bool{false});
            EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
            EXPECT_EQ(result.message, "Tool response ended incomplete");
            EXPECT_EQ(searches, 0);
        }
    }
}

Json maintenance_arguments() {
    return Json{{"prefix", nullptr}};
}

void expect_maintenance_tools(const Json& body) {
    ASSERT_TRUE(body.contains("tools"));
    std::vector<std::string> names;
    for (const Json& tool : body["tools"]) {
        EXPECT_EQ(tool["type"], "function");
        const Json& function = tool.contains("function") ? tool["function"] : tool;
        names.push_back(function["name"].get<std::string>());
        EXPECT_EQ(function["strict"], true);
        const auto& schema = function["parameters"];
        EXPECT_EQ(schema["additionalProperties"], false);
        if (function["name"] == "vault_config_apply") {
            const auto& item = schema["properties"]["changes"]["items"];
            EXPECT_EQ(item["additionalProperties"], false);
            EXPECT_EQ(item["required"], Json::array({"path", "operation", "content", "key", "value"}));
            EXPECT_EQ(item["properties"]["operation"]["enum"], Json::array({"create", "replace", "set"}));
            EXPECT_EQ(item["properties"]["content"]["type"], Json::array({"string", "null"}));
            EXPECT_EQ(item["properties"]["key"]["type"], Json::array({"string", "null"}));
            EXPECT_EQ(item["properties"]["value"]["type"], Json::array({"string", "number", "boolean", "null"}));
        } else if (function["name"] == "assistant_events") {
            EXPECT_TRUE(schema["properties"].empty());
            EXPECT_EQ(schema["required"], Json::array());
        } else if (function["name"] == "add_character") {
            EXPECT_EQ(schema["required"], Json::array({"version", "name", "description", "provider_id", "profile", "forum_id"}));
            EXPECT_EQ(schema["properties"].size(), 6U);
            for (const auto field : {"version", "name", "description", "provider_id", "profile"}) {
                EXPECT_EQ(schema["properties"][field]["type"], "string");
            }
            EXPECT_EQ(schema["properties"]["forum_id"]["type"], Json::array({"string", "null"}));
        }

        EXPECT_NE(tool.value("type", ""), "web_search");
        EXPECT_NE(tool.value("type", ""), "openrouter:web_search");
    }
    EXPECT_EQ(names, (std::vector<std::string>{
        "vault_config_list", "vault_config_read", "vault_config_apply", "add_character",
        "assistant_logs", "assistant_events", "assistant_logging",
        "host_config_list", "host_config_read", "host_config_write",
        "assistant_openai_login"}));
}

ProviderHttpResponse limited_tool_reply(ProviderApi api, bool stream, Json calls) {
    auto reply = tool_reply(api, stream, std::move(calls));
    if (!stream) {
        auto body = Json::parse(reply.body);
        if (api == ProviderApi::responses) {
            body["status"] = "incomplete";
            body["incomplete_details"] = {{"reason", "max_output_tokens"}};
        } else {
            body["choices"][0]["finish_reason"] = "length";
        }
        reply.body = body.dump();
        return reply;
    }
    if (api == ProviderApi::responses) {
        const auto marker = reply.body.rfind("data: ");
        reply.body.erase(marker);
        reply.body += "data: " + Json{
            {"type", "response.incomplete"},
            {"response", {
                {"status", "incomplete"},
                {"incomplete_details", {{"reason", "max_output_tokens"}}},
            }},
        }.dump() + "\n\n";
        return reply;
    }
    const auto event = "data: " + Json{{"choices", Json::array({Json{
        {"delta", Json::object()}, {"finish_reason", "length"}}})}}.dump() + "\n\n";
    reply.body.insert(reply.body.find("data: [DONE]"), event);
    return reply;
}

TEST(ProviderClientTools, MaintenanceTruncationIsNotExecuted) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        for (bool stream : {false, true}) {
            int calls = 0;
            ProviderClient client(shared_definition([&] {
                auto definition = network_definition(80, stream);
                definition.provider.config.api = api;
                return definition;
            }()), nullptr, [&](const ProviderHttpRequest&, const std::atomic_bool&) {
                return limited_tool_reply(api, stream, Json::array({
                    search_call(api, "cut", "{oops", "vault_config_list")}));
            });
            Transcript transcript;
            auto input = client_request(transcript, 1, "Repair");
            input.maintenance_tool = [&](auto, auto, const auto&) {
                ++calls;
                return "{}";
            };
            const auto result = client.perform(
                client.prepare(input), [](auto) {}, std::atomic_bool{false});
            EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error) << result.message;
            EXPECT_EQ(result.message, std::string(maintenance_output_limit_message));
            EXPECT_EQ(calls, 0);
        }
    }
}

TEST(ProviderClientTools, MaintenanceRejectsOversizedAndMalformedCalls) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        int calls = 0;
        ProviderClient client(shared_definition([&] {
            auto definition = network_definition(80, false);
            definition.provider.config.api = api;
            return definition;
        }()), nullptr, [&](const ProviderHttpRequest&, const std::atomic_bool&) {
            return tool_reply(api, false, Json::array({search_call(
                api, "huge", std::string(maintenance_argument_limit + 1, 'x'),
                "vault_config_list")}));
        });
        Transcript transcript;
        auto input = client_request(transcript, 1, "Repair");
        input.maintenance_tool = [&](auto, auto, const auto&) {
            ++calls;
            return "{}";
        };
        const auto result = client.perform(
            client.prepare(input), [](auto) {}, std::atomic_bool{false});
        EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
        EXPECT_EQ(result.message, "Tool arguments too large");
        EXPECT_EQ(calls, 0);
    }
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        for (bool stream : {false, true}) {
            int calls = 0;
            std::vector<Json> requests;
            ProviderClient client(shared_definition([&] {
                auto definition = network_definition(80, stream);
                definition.provider.config.api = api;
                return definition;
            }()), nullptr, [&](const ProviderHttpRequest& request, const std::atomic_bool&) {
                requests.push_back(Json::parse(request.body));
                if (requests.size() == 1) {
                    return tool_reply(api, stream, Json::array({
                        search_call(api, "bad", "not-json", "vault_config_read")}));
                }
                return tool_reply(api, stream, Json::array(), "Corrected.");
            });
            Transcript transcript;
            auto input = client_request(transcript, 2, "Repair");
            input.maintenance_tool = [&](auto, auto, const auto&) {
                ++calls;
                return "{}";
            };
            std::string answer;
            const auto result = client.perform(client.prepare(input), [&](GenerationDelta delta) {
                if (delta.kind == GenerationDeltaKind::answer) answer += delta.text;
            }, std::atomic_bool{false});
            EXPECT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
            EXPECT_EQ(calls, 0);
            EXPECT_EQ(answer, "Corrected.");
            const auto& messages = requests.back()[api == ProviderApi::responses ? "input" : "messages"];
            const auto output = messages.back()[api == ProviderApi::responses ? "output" : "content"].get<std::string>();
            EXPECT_NE(output.find("invalid_argument"), std::string::npos);
        }
    }
}

TEST(ProviderClientTools, MaintenanceDisablesToolsAfterTwentyFourCalls) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        int calls = 0;
        std::vector<Json> requests;
        ProviderClient client(shared_definition([&] {
            auto definition = network_definition(80, false);
            definition.provider.config.api = api;
            return definition;
        }()), nullptr, [&](const ProviderHttpRequest& request, const std::atomic_bool&) {
            requests.push_back(Json::parse(request.body));
            if (requests.size() == 1) {
                Json batch = Json::array();
                for (int index = 0; index < maintenance_call_limit; ++index) {
                    batch.push_back(search_call(
                        api, "call" + std::to_string(index),
                        maintenance_arguments().dump(), "vault_config_list"));
                }
                return tool_reply(api, false, batch);
            }
            return tool_reply(api, false, Json::array(), "Finished from the results.");
        });
        Transcript transcript;
        auto input = client_request(transcript, 3, "Repair");
        input.maintenance_tool = [&](auto, auto, const auto&) {
            ++calls;
            return R"({"version":"1","entries":[]})";
        };
        const auto result = client.perform(
            client.prepare(input), [](auto) {}, std::atomic_bool{false});
        EXPECT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
        EXPECT_EQ(calls, maintenance_call_limit);
        ASSERT_EQ(requests.size(), 2u);
        EXPECT_FALSE(requests.back().contains("tools"));
        EXPECT_NE(requests.back().dump().find("Earlier saved changes stay saved."), std::string::npos);
    }
}

TEST(ProviderClientTools, MaintenanceAnswerBudgetKeepsSavedResultsAndStopsLaterCalls) {
    struct Case {
        std::vector<std::pair<std::string, std::size_t>> calls;
        std::vector<std::string> expected_run;
    };
    const std::string big_name = "assistant_logs";
    const std::size_t big = 262000;
    const std::vector<Case> cases{
        // The apply goes over the budget. Its saved result reaches the model.
        {{{big_name, big}, {big_name, big}, {"vault_config_apply", 300}, {"vault_config_list", 10}},
            {big_name, big_name, "vault_config_apply"}},
        {{{big_name, big}, {big_name, big}, {"host_config_write", 300}, {"host_config_list", 10}},
            {big_name, big_name, "host_config_write"}},
        {{{big_name, big}, {big_name, big}, {"add_character", 300}, {"vault_config_list", 10}},
            {big_name, big_name, "add_character"}},
        {{{big_name, big}, {big_name, big}, {"add_character", maintenance_call_result_limit + 1}, {"vault_config_list", 10}},
            {big_name, big_name, "add_character"}},
        // A read goes over the budget. It is replaced, and the apply after it does not run.
        {{{big_name, big}, {big_name, big}, {big_name, 1000}, {"vault_config_apply", 300}},
            {big_name, big_name, big_name}},
    };
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        for (const Case& test : cases) {
            std::vector<Json> requests;
            ProviderClient client(shared_definition([&] {
                auto definition = network_definition(80, false);
                definition.provider.config.api = api;
                return definition;
            }()), nullptr, [&](const ProviderHttpRequest& request, const std::atomic_bool&) {
                requests.push_back(Json::parse(request.body));
                if (requests.size() == 1) {
                    Json batch = Json::array();
                    for (std::size_t index = 0; index < test.calls.size(); ++index) {
                        batch.push_back(search_call(
                            api, "call" + std::to_string(index), "{}", test.calls[index].first));
                    }
                    return tool_reply(api, false, batch);
                }
                return tool_reply(api, false, Json::array(), "Done.");
            });
            Transcript transcript;
            auto input = client_request(transcript, 6, "Repair");
            std::vector<std::string> run;
            input.maintenance_tool = [&](std::string_view name, auto, const auto&) {
                run.emplace_back(name);
                const std::size_t size = test.calls[run.size() - 1].second;
                if (name == "vault_config_apply" || name == "host_config_write" || name == "add_character") {
                    return R"({"committed":true,"padding":")" + std::string(size, 'a') + "\"}";
                }
                return R"({"text":")" + std::string(size, 'x') + "\"}";
            };
            const auto result = client.perform(
                client.prepare(input), [](auto) {}, std::atomic_bool{false});
            EXPECT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
            EXPECT_EQ(run, test.expected_run);
            ASSERT_EQ(requests.size(), 2u);
            EXPECT_FALSE(requests.back().contains("tools"));
            const auto& messages = requests.back()[api == ProviderApi::responses ? "input" : "messages"];
            std::vector<std::string> outputs;
            for (const Json& message : messages) {
                const auto field = api == ProviderApi::responses ? "output" : "content";
                const bool tool = api == ProviderApi::responses
                    ? message.value("type", "") == "function_call_output"
                    : message.value("role", "") == "tool";
                if (tool) outputs.push_back(message[field].get<std::string>());
            }
            ASSERT_EQ(outputs.size(), test.calls.size());
            EXPECT_NE(outputs[3].find("This call was not run."), std::string::npos);
            if (test.expected_run.back() == "vault_config_apply"
                || test.expected_run.back() == "host_config_write"
                || test.expected_run.back() == "add_character") {
                EXPECT_NE(outputs[2].find(R"("committed":true)"), std::string::npos);
            } else {
                EXPECT_NE(outputs[2].find("Tool results for this answer are too large."),
                    std::string::npos);
            }
        }
    }
}

TEST(ProviderClientTools, MaintenanceReplacesOneOversizedResultAndContinues) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        std::vector<Json> requests;
        ProviderClient client(shared_definition([&] {
            auto definition = network_definition(80, false);
            definition.provider.config.api = api;
            return definition;
        }()), nullptr, [&](const ProviderHttpRequest& request, const std::atomic_bool&) {
            requests.push_back(Json::parse(request.body));
            if (requests.size() == 1) {
                return tool_reply(api, false, Json::array({
                    search_call(api, "huge", "{}", "assistant_logs"),
                    search_call(api, "small", "{}", "vault_config_list"),
                }));
            }
            return tool_reply(api, false, Json::array(), "Done.");
        });
        Transcript transcript;
        auto input = client_request(transcript, 7, "Repair");
        std::vector<std::string> run;
        input.maintenance_tool = [&](std::string_view name, auto, const auto&) {
            run.emplace_back(name);
            if (name == "assistant_logs") {
                return R"({"text":")" + std::string(maintenance_call_result_limit, 'x') + "\"}";
            }
            return std::string(R"({"entries":[]})");
        };
        const auto result = client.perform(
            client.prepare(input), [](auto) {}, std::atomic_bool{false});
        EXPECT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
        EXPECT_EQ(run, (std::vector<std::string>{"assistant_logs", "vault_config_list"}));
        ASSERT_EQ(requests.size(), 2u);
        EXPECT_TRUE(requests.back().contains("tools"));
        const std::string sent = requests.back().dump();
        EXPECT_NE(sent.find("Tool result is too large."), std::string::npos);
        EXPECT_NE(sent.find("entries"), std::string::npos);
    }
}

TEST(ProviderClientTools, MaintenanceAnswerBudgetStopsCallsInLaterRounds) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        std::vector<Json> requests;
        ProviderClient client(shared_definition([&] {
            auto definition = network_definition(80, false);
            definition.provider.config.api = api;
            return definition;
        }()), nullptr, [&](const ProviderHttpRequest& request, const std::atomic_bool&) {
            requests.push_back(Json::parse(request.body));
            if (requests.size() == 1) {
                Json batch = Json::array();
                for (int index = 0; index < 3; ++index) {
                    batch.push_back(search_call(
                        api, "big" + std::to_string(index), "{}", "assistant_logs"));
                }
                return tool_reply(api, false, batch);
            }
            // The final request has no tools, but this provider still asks for one.
            return tool_reply(api, false, Json::array({
                search_call(api, "late", "{}", "vault_config_apply"),
            }));
        });
        Transcript transcript;
        auto input = client_request(transcript, 8, "Repair");
        std::vector<std::string> run;
        input.maintenance_tool = [&](std::string_view name, auto, const auto&) {
            run.emplace_back(name);
            return R"({"text":")" + std::string(262000, 'x') + "\"}";
        };
        const auto result = client.perform(
            client.prepare(input), [](auto) {}, std::atomic_bool{false});
        EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
        EXPECT_EQ(run, (std::vector<std::string>(3, "assistant_logs")));
        ASSERT_EQ(requests.size(), 2u);
        EXPECT_FALSE(requests.back().contains("tools"));
        EXPECT_FALSE(requests.back().contains("tool_choice"));
    }
}

TEST(ProviderClientTools, MaintenanceOmitsRequiredWebSearch) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        int web_calls = 0;
        int maintenance_calls = 0;
        std::vector<Json> requests;
        ProviderClient client(shared_definition([&] {
            auto definition = network_definition(80, false);
            definition.provider.config.api = api;
            definition.provider.config.host = "openrouter.ai";
            definition.provider.config.web_search = WebSearchMode::required;
            return definition;
        }()), nullptr, [&](const ProviderHttpRequest& request, const std::atomic_bool&) {
            requests.push_back(Json::parse(request.body));
            if (requests.size() == 1) {
                return tool_reply(api, false, Json::array({
                    search_call(api, "search"),
                    search_call(api, "read", R"({"url":"https://example.org"})", "web_read"),
                    search_call(api, "list", maintenance_arguments().dump(), "vault_config_list"),
                }));
            }
            return tool_reply(api, false, Json::array(), "Local evidence only.");
        });
        Transcript transcript;
        auto input = client_request(transcript, 4, "Repair");
        input.web_search_tool = [&](auto, const auto&) -> std::string {
            ++web_calls;
            throw std::runtime_error("web search must not run");
        };
        input.web_read_tool = [&](auto, const auto&, const auto&) -> std::string {
            ++web_calls;
            throw std::runtime_error("web read must not run");
        };
        input.maintenance_tool = [&](std::string_view name, auto, const auto&) {
            ++maintenance_calls;
            EXPECT_EQ(name, "vault_config_list");
            return R"({"version":"1","entries":[]})";
        };
        GenerationStatistics statistics;
        auto payload = client.prepare(input);
        payload.statistics = &statistics;
        const auto result = client.perform(std::move(payload), [](auto) {}, std::atomic_bool{false});
        EXPECT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
        EXPECT_EQ(statistics.maintenance.calls, 1u);
        EXPECT_EQ(statistics.maintenance.failed, 0u);
        EXPECT_EQ(statistics.tool_calls_rejected, 2u);
        EXPECT_GT(statistics.maintenance.duration.count(), 0);
        EXPECT_EQ(web_calls, 0);
        EXPECT_EQ(maintenance_calls, 1);
        expect_maintenance_tools(requests.front());
        const auto& messages = requests.back()[api == ProviderApi::responses ? "input" : "messages"];
        int unavailable = 0;
        for (const Json& message : messages) {
            const auto field = api == ProviderApi::responses ? "output" : "content";
            if (!message.contains(field) || !message[field].is_string()) continue;
            if (message[field].get<std::string>().find("Web tools are not available") != std::string::npos) {
                ++unavailable;
            }
        }
        EXPECT_EQ(unavailable, 2);
    }
}

TEST(ProviderClientTools, SubscriptionMaintenanceDoesNotAddAnOutputTokenCap) {
    SubscriptionOwner owner;
    OpenAiOAuth oauth = owner.make_connected();
    CharacterDefinition definition = subscription_definition();
    definition.provider.config.max_tokens = 32;
    definition.provider.config.web_search = WebSearchMode::required;
    ProviderClient client(shared_definition(std::move(definition)), &oauth);
    Transcript transcript;
    auto input = client_request(transcript, 5, "Repair");
    input.maintenance_tool = [](auto, auto, const auto&) { return "{}"; };
    const Json body = Json::parse(client.prepare(input).bytes);
    EXPECT_FALSE(body.contains("max_output_tokens"));
    EXPECT_FALSE(body.contains("max_tokens"));
    expect_maintenance_tools(body);
}


Json google_response(Json parts, std::string reason = "STOP") {
    return {{"candidates", Json::array({{
        {"content", {{"role", "model"}, {"parts", std::move(parts)}}},
        {"finishReason", std::move(reason)}}})}};
}

TEST(ProviderClient, GoogleUsesNativeUrlsAndOptionalSavedKeyHeaders) {
    test::TestWorkspace workspace;
    auto config = WorkspaceConfigStore::open(test::import_test_database(workspace.root()));
    ApiKeyStore keys(*config);
    const auto key = keys.create("Google", "google-test-secret");
    for (const bool stream : {false, true}) {
        for (const int credential : {0, 1, 2}) {
            auto definition = network_definition(443, stream);
            auto& provider = definition.provider.config;
            provider.api = ProviderApi::google;
            provider.base_path = "/v1beta";
            provider.model = "models/gemini-test";
            if (credential == 1) provider.api_key_id = key.id;
            if (credential == 2) provider.api_key_env = "Google";
            int calls = 0;
            ProviderClient client(shared_definition(definition), nullptr, &keys,
                [&](const ProviderHttpRequest& request, const auto&) {
                    ++calls;
                    EXPECT_EQ(request.url, std::string("http://127.0.0.1:443/v1beta/models/gemini-test")
                        + (stream ? ":streamGenerateContent?alt=sse" : ":generateContent"));
                    EXPECT_EQ(std::ranges::count(request.headers, "x-goog-api-key: google-test-secret"), credential ? 1 : 0);
                    for (const auto& header : request.headers) EXPECT_FALSE(header.starts_with("Authorization:"));
                    EXPECT_FALSE(Json::parse(request.body).contains("messages"));
                    auto response = google_response(Json::array({{{"text", "OK"}}})).dump();
                    return ProviderHttpResponse{200, stream ? "text/event-stream" : "application/json",
                        stream ? "data: " + response + "\n\n" : response};
                });
            Transcript transcript;
            auto request = client_request(transcript, 1, "Question");
            std::atomic_bool cancel{false};
            std::string answer;
            auto result = client.perform(client.prepare(request), [&](GenerationDelta d) { answer += d.text; }, cancel);
            EXPECT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
            EXPECT_EQ(answer, "OK");
            EXPECT_EQ(calls, 1);
        }
    }
    auto definition = network_definition(443);
    definition.provider.config.api = ProviderApi::google;
    definition.provider.config.api_key_id = "missing";
    EXPECT_THROW((ProviderClient(shared_definition(definition), nullptr, &keys)), std::exception);
}

TEST(ProviderClient, GoogleKeepsParallelAndSequentialToolPartsInOrderThroughLimit) {
    for (const bool stream : {false, true}) {
        for (const bool keep_calling : {false, true}) {
          for (const bool with_stores : {false, true}) {
            auto definition = network_definition(8080, stream);
            definition.provider.config.api = ProviderApi::google;
            int requests = 0;
            std::vector<Json> sent;
            std::vector<Json> model_contents;
            ProviderClient client(shared_definition(definition), nullptr,
                [&](const ProviderHttpRequest& request, const auto&) {
                    auto body = Json::parse(request.body);
                    for (const auto* field : {"tool_choice", "messages", "input", "model", "stream"}) EXPECT_FALSE(body.contains(field));
                    for (const auto& content : body.at("contents")) EXPECT_FALSE(content.contains("content"));
                    if (requests) {
                        const auto& contents = body.at("contents");
                        EXPECT_EQ(contents.size(), 1u + 2u * static_cast<unsigned>(requests));
                        for (int previous = 0; previous < requests; ++previous) {
                            EXPECT_EQ(contents[1 + previous * 2], model_contents[previous]);
                        }
                        const auto& responses = contents.back().at("parts");
                        EXPECT_EQ(contents.back().at("role"), "user");
                        EXPECT_EQ(responses.size(), requests == 1 ? 2u : 1u);
                        if (requests == 1) {
                            EXPECT_EQ(responses[0]["functionResponse"]["name"], "web_search");
                            EXPECT_EQ(responses[0]["functionResponse"]["id"], "provider-id");
                            EXPECT_EQ(responses[0]["functionResponse"]["response"], (Json{{"answer", "first"}}));
                            EXPECT_FALSE(responses[1]["functionResponse"].contains("id"));
                            EXPECT_EQ(responses[1]["functionResponse"]["response"], (Json{{"result", Json::array({1, 2})}}));
                        } else EXPECT_EQ(responses[0]["functionResponse"]["response"], (Json{{"result", "plain result"}}));
                    }
                    if (with_stores) {
                        EXPECT_EQ(body["tools"].back(), (Json{{"fileSearch", {{"fileSearchStoreNames", {"fileSearchStores/one"}}}}}));
                        EXPECT_EQ(body["toolConfig"]["includeServerSideToolInvocations"], true);
                        EXPECT_EQ(body["toolConfig"]["functionCallingConfig"]["mode"], requests < 3 ? "VALIDATED" : "NONE");
                    }
                    sent.push_back(body);
                    ++requests;
                    // Two parallel calls then two sequential calls exhaust the search limit of four.
                    auto parts = Json::array({{{"text", requests == 1 ? "First" : requests == 4 ? "Final" : "Next"}}});
                    if (requests < 4 || keep_calling) {
                        Json call{{"name", "web_search"}, {"args", {{"query", requests == 1 ? "first" : "later"}}}};
                        if (requests == 1) call["id"] = "provider-id";
                        parts.push_back({{"functionCall", call}, {"thoughtSignature", "signed-" + std::to_string(requests)}});
                        if (requests == 1) parts.push_back({{"functionCall", {{"name", "web_search"}, {"args", {{"query", "second"}}}}}});
                        parts.push_back({{"thoughtSignature", "tail-" + std::to_string(requests)}});
                    }
                    if (with_stores) {
                        parts.insert(parts.begin(), Json{{"toolCall", {{"id", "hosted"}, {"toolType", "FILE_SEARCH"}, {"args", {{"query", "q"}}}}}, {"thoughtSignature", "hosted-signature"}});
                        parts.push_back({{"toolResponse", {{"id", "hosted"}, {"response", {{"text", "retrieved"}}}}}});
                    }
                    auto response = google_response(parts);
                    model_contents.push_back(response["candidates"][0]["content"]);
                    response["usageMetadata"] = {{"promptTokenCount", 3}, {"candidatesTokenCount", 2}, {"thoughtsTokenCount", 1}};
                    const auto encoded = response.dump();
                    return ProviderHttpResponse{200, stream ? "text/event-stream" : "application/json",
                        stream ? "data: " + encoded + "\n\n" : encoded};
                });
            Transcript transcript;
            auto request = client_request(transcript, 1, "Question");
            if (with_stores) request.folder_search = FolderSearchAccess{FolderStoreService::google, {"fileSearchStores/one"}};
            std::atomic_int executions{0};
            request.web_search_tool = [&](std::string_view query, const auto&) {
                ++executions;
                if (query == "first") return std::string(R"({"answer":"first"})");
                if (query == "second") return std::string("[1,2]");
                return std::string("plain result");
            };
            std::atomic_bool cancel{false};
            std::string answer;
            const auto result = client.perform(client.prepare(request), [&](GenerationDelta d) { answer += d.text; }, cancel);
            EXPECT_EQ(result.outcome, keep_calling ? GenerationOutcome::protocol_error : GenerationOutcome::completed) << result.message;
            EXPECT_EQ(executions.load(), 4);
            ASSERT_EQ(sent.size(), 4u);
            EXPECT_EQ(sent.back()["toolConfig"]["functionCallingConfig"]["mode"], "NONE");
            EXPECT_TRUE(sent.back().contains("tools"));
            EXPECT_NE(sent.back()["systemInstruction"].dump().find("Web search is unavailable"), std::string::npos);
            EXPECT_NE(sent.back()["systemInstruction"].dump().find("tool limit was reached"), std::string::npos);
            if (with_stores) {
                EXPECT_NE(sent.back()["systemInstruction"].dump().find("Folder search is available"), std::string::npos);
                EXPECT_EQ(sent.back()["systemInstruction"].dump().find("Do not invent missing facts or request more tools"), std::string::npos);
            }
            EXPECT_TRUE(sent.back()["contents"].back()["parts"][0].contains("functionResponse"));
            EXPECT_EQ(result.usage.input_tokens, 12u);
            EXPECT_EQ(result.usage.output_tokens, 12u);
            EXPECT_EQ(answer, "First\n\nNext\n\nNext\n\nFinal");
          }
        }
    }
}

TEST(ProviderClient, GoogleRejectsIncompleteCallsAndHonorsCancellationAndHttpErrors) {
    for (const auto* reason : {"MAX_TOKENS", "SAFETY", "MALFORMED_FUNCTION_CALL", ""}) {
        auto definition = network_definition(8080, false);
        definition.provider.config.api = ProviderApi::google;
        ProviderClient client(shared_definition(definition), nullptr, [&](const auto&, const auto&) {
            return ProviderHttpResponse{200, "application/json", google_response(Json::array({
                {{"text", "Partial"}}, {{"functionCall", {{"name", "assistant_events"}, {"args", Json::object()}}}}
            }), reason).dump()};
        });
        Transcript transcript;
        auto request = client_request(transcript, 1, "Question");
        int executed = 0;
        request.maintenance_tool = [&](auto, auto, const auto&) { ++executed; return "{}"; };
        std::atomic_bool cancel{false};
        std::string answer;
        const auto result = client.perform(client.prepare(request), [&](GenerationDelta d) { answer += d.text; }, cancel);
        EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
        EXPECT_EQ(executed, 0);
        EXPECT_EQ(answer, "Partial");
    }
    for (const bool stream : {false, true}) {
        auto definition = network_definition(8080, stream);
        definition.provider.config.api = ProviderApi::google;
        std::atomic_bool cancel{false};
        int calls = 0;
        ProviderClient client(shared_definition(definition), nullptr, [&](const auto&, const auto&) {
            ++calls;
            return ProviderHttpResponse{400, "application/json", R"({"error":{"message":"Google error detail"}})"};
        });
        Transcript transcript;
        auto request = client_request(transcript, 1, "Question");
        const GenerationDeltaSink sink = [](auto) {};
        auto result = client.perform(client.prepare(request), sink, cancel);
        EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
        EXPECT_NE(result.message.find("Google error detail"), std::string::npos);
        cancel = true;
        EXPECT_EQ(client.perform(client.prepare(request), sink, cancel).outcome, GenerationOutcome::cancelled);
        EXPECT_EQ(calls, 1);
    }
}

TEST(ProviderClient, GoogleCurlReadsSseThroughHttpEnd) {
    const std::string stream = "data: " + google_response(Json::array({{{"text", "Answer"}}})).dump()
        + "\n\ndata: {\"usageMetadata\":{\"candidatesTokenCount\":4}}\n\n";
    MockHttpServer server({http_response("text/event-stream", stream)});
    server.start();
    auto definition = network_definition(server.port());
    definition.provider.config.api = ProviderApi::google;
    definition.provider.config.base_path = "/v1beta";
    ProviderClient client(shared_definition(definition));
    Transcript transcript;
    const auto request = client_request(transcript, 1, "Question");
    std::atomic_bool cancel{false};
    std::string answer;
    const auto result = client.perform(client.prepare(request), [&](GenerationDelta d) { answer += d.text; }, cancel);
    server.join();
    EXPECT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
    EXPECT_EQ(result.usage.output_tokens, 4u);
    EXPECT_EQ(answer, "Answer");
    ASSERT_EQ(server.requests().size(), 1u);
    EXPECT_TRUE(server.requests()[0].starts_with("POST /v1beta/models/configured-model:streamGenerateContent?alt=sse HTTP/1.1"));
}

Json anthropic_response(Json content, std::string reason = "end_turn") {
    return {{"type", "message"}, {"role", "assistant"}, {"content", content}, {"stop_reason", reason},
        {"usage", {{"input_tokens", 3}, {"output_tokens", 2}, {"cache_read_input_tokens", 1}}}};
}
std::string anthropic_stream(const Json& message) {
    const auto event = [](Json value) { return "data: " + value.dump() + "\n\n"; };
    auto start = message;
    start["content"] = Json::array(); start["stop_reason"] = nullptr;
    auto stream = event({{"type", "message_start"}, {"message", start}});
    int index = 0;
    for (const auto& block : message["content"]) {
        stream += event({{"type", "content_block_start"}, {"index", index}, {"content_block", block}});
        stream += event({{"type", "content_block_stop"}, {"index", index++}});
    }
    return stream + event({{"type", "message_delta"}, {"delta", {{"stop_reason", message["stop_reason"]}}}})
        + event({{"type", "message_stop"}});
}

TEST(ProviderClient, AnthropicUsesNativeEndpointVersionAndOptionalSavedBearerKey) {
    test::TestWorkspace workspace;
    auto config = WorkspaceConfigStore::open(test::import_test_database(workspace.root()));
    ApiKeyStore keys(*config);
    const auto key = keys.create("Anthropic", "anthropic-test-secret");
    for (const bool stream : {false, true}) {
        for (const int credential : {0, 1, 2}) {
            auto definition = network_definition(8080, stream);
            auto& provider = definition.provider.config;
            provider.api = ProviderApi::anthropic;
            provider.base_path = "/proxy";
            if (credential == 1) provider.api_key_id = key.id;
            if (credential == 2) provider.api_key_env = "Anthropic";
            ProviderClient client(shared_definition(definition), nullptr, &keys,
                [&](const ProviderHttpRequest& request, const auto&) {
                    EXPECT_EQ(request.url, "http://127.0.0.1:8080/proxy/v1/messages");
                    EXPECT_EQ(std::ranges::count(request.headers, "Authorization: Bearer anthropic-test-secret"), credential ? 1 : 0);
                    EXPECT_EQ(std::ranges::count(request.headers, "anthropic-version: 2026-10-21"), 1);
                    const auto body = Json::parse(request.body);
                    EXPECT_EQ(body["max_tokens"], 64000);
                    auto response = anthropic_response(Json::array({{{"type", "text"}, {"text", "OK"}}}));
                    return ProviderHttpResponse{200, stream ? "text/event-stream" : "application/json",
                        stream ? anthropic_stream(response) : response.dump()};
                });
            Transcript transcript;
            auto request = client_request(transcript, 1, "Question");
            std::atomic_bool cancel{false};
            std::string answer;
            const auto result = client.perform(client.prepare(request), [&](GenerationDelta d) { answer += d.text; }, cancel);
            EXPECT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
            EXPECT_EQ(answer, "OK");
        }
    }
    auto definition = network_definition(8080);
    definition.provider.config.api = ProviderApi::anthropic;
    definition.provider.config.api_key_id = "missing";
    EXPECT_THROW((ProviderClient(shared_definition(definition), nullptr, &keys)), std::exception);
}

TEST(ProviderClient, AnthropicPreservesSignedPrefixAndParallelResultOrderThroughToolLimit) {
    for (const bool stream : {false, true}) {
        for (const bool keep_calling : {false, true}) {
            auto definition = network_definition(8080, stream);
            definition.provider.config.api = ProviderApi::anthropic;
            std::vector<Json> sent, assistant_messages;
            ProviderClient client(shared_definition(definition), nullptr,
                [&](const ProviderHttpRequest& request, const auto&) {
                    const auto body = Json::parse(request.body);
                    for (const auto* field : {"input", "instructions", "temperature", "reasoning_effort", "reasoning", "provider"}) EXPECT_FALSE(body.contains(field));
                    if (!sent.empty()) {
                        EXPECT_EQ(body["system"], sent.front()["system"]);
                        EXPECT_EQ(body["tools"], sent.front()["tools"]);
                        const auto& messages = body["messages"];
                        EXPECT_EQ(messages.size(), 1u + sent.size() * 2);
                        for (std::size_t i = 0; i < sent.back()["messages"].size(); ++i) {
                            EXPECT_EQ(messages[i], sent.back()["messages"][i]);
                        }
                        EXPECT_EQ(messages[messages.size() - 2], assistant_messages.back());
                        EXPECT_EQ(messages.back()["role"], "user");
                        const auto& results = messages.back()["content"];
                        EXPECT_EQ(results.size(), sent.size() == 1 ? 2u : 1u);
                        EXPECT_EQ(results[0], (Json{{"type", "tool_result"}, {"tool_use_id", "id-1"}, {"content", "first result"}}));
                        if (sent.size() == 1) EXPECT_EQ(results[1], (Json{{"type", "tool_result"}, {"tool_use_id", "id-2"}, {"content", "second result"}}));
                    }
                    if (sent.size() == 3) EXPECT_EQ(body["tool_choice"], (Json{{"type", "none"}}));
                    else EXPECT_FALSE(body.contains("tool_choice"));
                    sent.push_back(body);
                    auto content = Json::array({{{"type", "thinking"}, {"thinking", ""}, {"signature", "signed-" + std::to_string(sent.size())}},
                        {{"type", "redacted_thinking"}, {"data", "opaque"}},
                        {{"type", "text"}, {"text", sent.size() == 4 ? "Final" : "Checking"}}});
                    const bool calls = sent.size() < 4 || keep_calling;
                    if (calls) {
                        content.push_back({{"type", "tool_use"}, {"id", "id-1"}, {"name", "web_search"}, {"input", {{"query", "first"}}}});
                        if (sent.size() == 1) content.push_back({{"type", "tool_use"}, {"id", "id-2"}, {"name", "web_search"}, {"input", {{"query", "second"}}}});
                    }
                    assistant_messages.push_back({{"role", "assistant"}, {"content", content}});
                    auto response = anthropic_response(content, calls ? "tool_use" : "end_turn");
                    return ProviderHttpResponse{200, stream ? "text/event-stream" : "application/json",
                        stream ? anthropic_stream(response) : response.dump()};
                });
            Transcript transcript;
            auto request = client_request(transcript, 1, "Question");
            std::atomic_int executions{0};
            request.web_search_tool = [&](std::string_view query, const auto&) {
                ++executions;
                return query == "first" ? "first result" : "second result";
            };
            std::atomic_bool cancel{false};
            std::string answer;
            const auto result = client.perform(client.prepare(request), [&](GenerationDelta d) { answer += d.text; }, cancel);
            EXPECT_EQ(result.outcome, keep_calling ? GenerationOutcome::protocol_error : GenerationOutcome::completed) << result.message;
            EXPECT_EQ(executions.load(), 4);
            ASSERT_EQ(sent.size(), 4u);
            EXPECT_EQ(result.usage.input_tokens, 16u);
            EXPECT_EQ(result.usage.output_tokens, 8u);
            EXPECT_EQ(answer, "Checking\n\nChecking\n\nChecking\n\nFinal");
        }
    }
}

TEST(ProviderClient, AnthropicRejectsTruncatedMaintenanceCallsAndSupportsCurlAndCancellation) {
    for (const bool stream : {false, true}) {
        for (const auto* reason : {"max_tokens", "model_context_window_exceeded"}) {
            auto definition = network_definition(8080, stream);
            definition.provider.config.api = ProviderApi::anthropic;
            auto response = anthropic_response(Json::array({{{"type", "tool_use"}, {"id", "id"},
                {"name", "vault_config_apply"}, {"input", "{incomplete"}}}), reason);
            // Streaming truncation may leave both JSON and the content block incomplete.
            const auto truncated = std::string("data: {\"type\":\"message_start\",\"message\":{\"type\":\"message\",\"role\":\"assistant\",\"content\":[]}}\n\n")
                + "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"tool_use\",\"id\":\"id\",\"name\":\"vault_config_apply\",\"input\":{}}}\n\n"
                + "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{bad\"}}\n\n"
                + "data: " + Json{{"type", "message_delta"}, {"delta", {{"stop_reason", reason}}}}.dump()
                + "\n\ndata: {\"type\":\"message_stop\"}\n\n";
            ProviderClient client(shared_definition(definition), nullptr, [&](const auto&, const auto&) {
                return ProviderHttpResponse{200, stream ? "text/event-stream" : "application/json", stream ? truncated : response.dump()};
            });
            Transcript transcript;
            auto request = client_request(transcript, 1, "Question");
            int executions = 0;
            request.maintenance_tool = [&](auto, auto, const auto&) { ++executions; return "{}"; };
            std::atomic_bool cancel{false};
            const auto result = client.perform(client.prepare(request), [](auto) {}, cancel);
            EXPECT_EQ(result.message, maintenance_output_limit_message);
            EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
            EXPECT_EQ(executions, 0);
        }
    }
    const auto message = anthropic_response(Json::array({{{"type", "text"}, {"text", "Answer"}}}));
    MockHttpServer server({http_response("text/event-stream", anthropic_stream(message))});
    server.start();
    auto definition = network_definition(server.port());
    definition.provider.config.api = ProviderApi::anthropic;
    ProviderClient client(shared_definition(definition));
    Transcript transcript;
    const auto request = client_request(transcript, 1, "Question");
    std::atomic_bool cancel{false};
    std::string answer;
    auto result = client.perform(client.prepare(request), [&](GenerationDelta d) { answer += d.text; }, cancel);
    server.join();
    EXPECT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
    EXPECT_EQ(answer, "Answer");
    ASSERT_EQ(server.requests().size(), 1u);
    EXPECT_TRUE(server.requests()[0].starts_with("POST /v1/messages HTTP/1.1"));
    cancel = true;
    EXPECT_EQ(client.perform(client.prepare(request), [](auto) {}, cancel).outcome, GenerationOutcome::cancelled);
}

TEST(ProviderClient, AnthropicRedactsSavedKeyFromHttpAndSseErrors) {
    test::TestWorkspace workspace;
    auto config = WorkspaceConfigStore::open(test::import_test_database(workspace.root()));
    ApiKeyStore keys(*config);
    const auto key = keys.create("Anthropic", "anthropic-secret");
    for (const bool stream : {false, true}) {
        auto definition = network_definition(8080, stream);
        definition.provider.config.api = ProviderApi::anthropic;
        definition.provider.config.api_key_id = key.id;
        ProviderClient client(shared_definition(definition), nullptr, &keys, [&](const auto&, const auto&) {
            const auto body = Json{{"type", "error"}, {"error", {{"message", "Rejected anthropic-secret"}}}}.dump();
            return ProviderHttpResponse{stream ? 200 : 400, stream ? "text/event-stream" : "application/json", stream ? "data: " + body + "\n\n" : body};
        });
        Transcript transcript;
        const auto request = client_request(transcript, 1, "Question");
        std::atomic_bool cancel{false};
        const auto result = client.perform(client.prepare(request), [](auto) {}, cancel);
        EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
        EXPECT_NE(result.message.find("Rejected [REDACTED]"), std::string::npos);
        EXPECT_EQ(result.message.find("anthropic-secret"), std::string::npos);
    }
}

TEST(ProviderClient, AnthropicReturnsEveryToolResultIncludingRejectedCalls) {
    auto definition = network_definition(8080, false);
    definition.provider.config.api = ProviderApi::anthropic;
    int rounds = 0;
    ProviderClient client(shared_definition(definition), nullptr, [&](const ProviderHttpRequest& wire, const auto&) {
        const auto body = Json::parse(wire.body);
        if (++rounds == 1) {
            auto calls = Json::array();
            for (int i = 0; i < 5; ++i) calls.push_back({{"type", "tool_use"}, {"id", std::to_string(i)},
                {"name", "web_search"}, {"input", {{"query", "query"}}}});
            return ProviderHttpResponse{200, "application/json", anthropic_response(calls, "tool_use").dump()};
        }
        EXPECT_EQ(body["tool_choice"], (Json{{"type", "none"}}));
        const auto& results = body["messages"].back()["content"];
        EXPECT_EQ(results.size(), 5u);
        for (std::size_t i = 0; i < results.size(); ++i) {
            EXPECT_EQ(results[i]["tool_use_id"], std::to_string(i));
            EXPECT_EQ(results[i]["type"], "tool_result");
            if (i < 4) EXPECT_EQ(results[i]["content"], "result");
            else EXPECT_NE(results[i]["content"].get<std::string>().find("limit reached"), std::string::npos);
        }
        return ProviderHttpResponse{200, "application/json",
            anthropic_response(Json::array({{{"type", "text"}, {"text", "Final"}}})).dump()};
    });
    Transcript transcript;
    auto request = client_request(transcript, 1, "Question");
    std::atomic_int executions{0};
    request.web_search_tool = [&](auto, const auto&) { ++executions; return "result"; };
    std::atomic_bool cancel{false};
    EXPECT_EQ(client.perform(client.prepare(request), [](auto) {}, cancel).outcome, GenerationOutcome::completed);
    EXPECT_EQ(executions.load(), 4);
    EXPECT_EQ(rounds, 2);
}

TEST(ProviderClient, AnthropicCancellationAfterTransportDoesNotExecuteCalls) {
    auto definition = network_definition(8080, false);
    definition.provider.config.api = ProviderApi::anthropic;
    std::atomic_bool cancel{false};
    ProviderClient client(shared_definition(definition), nullptr, [&](const auto&, const auto&) {
        cancel = true;
        return ProviderHttpResponse{200, "application/json", anthropic_response(Json::array({
            {{"type", "tool_use"}, {"id", "id"}, {"name", "assistant_events"}, {"input", Json::object()}}
        }), "tool_use").dump()};
    });
    Transcript transcript;
    auto request = client_request(transcript, 1, "Question");
    int executions = 0;
    request.maintenance_tool = [&](auto, auto, const auto&) { ++executions; return "{}"; };
    EXPECT_EQ(client.perform(client.prepare(request), [](auto) {}, cancel).outcome, GenerationOutcome::cancelled);
    EXPECT_EQ(executions, 0);
}


TEST(ProviderClientTools, ResponsesKeepsHostedSearchAndReplayThroughFinalBudgetRound) {
    for (const bool stream : {false, true}) {
        auto definition = network_definition(443, stream);
        definition.provider.config.api = ProviderApi::responses;
        definition.provider.config.host = "api.openai.com";
        definition.provider.config.web_search = WebSearchMode::required;
        int requests = 0;
        Json first_output;
        ProviderClient client(shared_definition(definition), nullptr,
            [&](const ProviderHttpRequest& request, const auto&) {
                const auto body = Json::parse(request.body);
                EXPECT_EQ(body["store"], false);
                EXPECT_EQ(body["include"], Json::array({"reasoning.encrypted_content", "file_search_call.results"}));
                if (++requests == 1) {
                    EXPECT_EQ(body["tool_choice"], "required");
                    auto calls = Json::array({{{"type", "file_search_call"}, {"id", "hosted"}, {"status", "completed"}, {"queries", {"fact"}},
                        {"results", Json::array({{{"file_id", "file_1"}, {"filename", "note.md"}, {"score", 0.9}, {"text", "retrieved fact"}}})}}});
                    for (int i = 0; i < 4; ++i) calls.push_back(search_call(ProviderApi::responses, "call" + std::to_string(i)));
                    first_output = calls;
                    return tool_reply(ProviderApi::responses, stream, calls);
                }
                EXPECT_EQ(body["tools"], (Json::array({{{"type", "file_search"}, {"vector_store_ids", {"vs_one"}}}})));
                EXPECT_EQ(body["tool_choice"], "auto");
                EXPECT_EQ(body["input"][2], first_output[0]);
                EXPECT_EQ(body["input"][2]["results"][0]["text"], "retrieved fact");
                EXPECT_EQ(body["input"][1]["encrypted_content"], "opaque reasoning");
                EXPECT_NE(body["instructions"].dump().find("Folder search is available"), std::string::npos);
                EXPECT_NE(body["instructions"].dump().find("Web search is unavailable"), std::string::npos);
                const Json done{{"status", "completed"}, {"output", Json::array({{{"type", "message"}, {"role", "assistant"},
                    {"content", Json::array({{{"type", "output_text"}, {"text", "Answer"}}})}}})}};
                return ProviderHttpResponse{200, stream ? "text/event-stream" : "application/json", stream
                    ? "data: " + Json{{"type", "response.output_text.delta"}, {"delta", "Answer"}}.dump() + "\n\n"
                        + "data: " + Json{{"type", "response.completed"}, {"response", done}}.dump() + "\n\n"
                    : done.dump()};
            });
        Transcript transcript;
        auto request = client_request(transcript, 1, "Question");
        request.folder_search = FolderSearchAccess{FolderStoreService::openai, {"vs_one"}};
        std::atomic_int executions{0};
        request.web_search_tool = [&](auto, const auto&) { ++executions; return "[]"; };
        std::string answer;
        const auto result = client.perform(client.prepare(request), [&](GenerationDelta d) { answer += d.text; }, std::atomic_bool{false});
        EXPECT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
        EXPECT_EQ(executions.load(), 4);
        EXPECT_EQ(requests, 2);
        EXPECT_EQ(answer, "Answer");
    }
}


TEST(ProviderClient, UnknownGoogleMixedModelFailureDoesNotRetryOrDropStoresOrFunctions) {
    auto definition = network_definition(443, false);
    definition.provider.config.api = ProviderApi::google;
    definition.provider.config.model = "unknown-model";
    int attempts = 0;
    ProviderClient client(shared_definition(definition), nullptr, [&](const auto& request, const auto&) {
        ++attempts;
        const auto body = Json::parse(request.body);
        EXPECT_EQ(body["tools"].size(), 2u);
        EXPECT_EQ(body["tools"][0]["functionDeclarations"][0]["name"], "web_search");
        EXPECT_EQ(body["tools"][1]["fileSearch"]["fileSearchStoreNames"], Json::array({"fileSearchStores/one"}));
        EXPECT_EQ(body["toolConfig"]["functionCallingConfig"]["mode"], "VALIDATED");
        return ProviderHttpResponse{400, "application/json", R"({"error":{"message":"This model rejects the tool combination"}})"};
    });
    Transcript transcript;
    auto request = client_request(transcript, 1, "Question");
    request.folder_search = FolderSearchAccess{FolderStoreService::google, {"fileSearchStores/one"}};
    request.web_search_tool = [](auto, const auto&) { ADD_FAILURE() << "No local function should run"; return "[]"; };
    const auto result = client.perform(client.prepare(request), [](auto) {}, std::atomic_bool{false});
    EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
    EXPECT_NE(result.message.find("This model rejects"), std::string::npos);
    EXPECT_EQ(attempts, 1);
    EXPECT_EQ(request.folder_search->remote_store_ids, (std::vector<std::string>{"fileSearchStores/one"}));
}

Json summary_mock_call(ProviderApi api, std::string id, std::string name, Json args) {
    if (api == ProviderApi::google) return {{"functionCall", {{"name", name}, {"id", id}, {"args", args}}}, {"thoughtSignature", "signed"}};
    if (api == ProviderApi::anthropic) return {{"type", "tool_use"}, {"id", id}, {"name", name}, {"input", args}};
    return search_call(api, id, args.dump(), name);
}
ProviderHttpResponse summary_mock_reply(ProviderApi api, bool stream, Json calls, std::string answer = {}) {
    if (api == ProviderApi::chat_completions || api == ProviderApi::responses) return tool_reply(api, stream, calls, answer);
    if (api == ProviderApi::google) {
        if (!answer.empty()) calls.push_back({{"text", answer}});
        auto response = google_response(calls);
        response["usageMetadata"] = {{"promptTokenCount", 10}, {"candidatesTokenCount", 2}};
        auto text = response.dump();
        return {200, stream ? "text/event-stream" : "application/json", stream ? "data: " + text + "\n\n" : text};
    }
    if (!answer.empty()) calls.push_back({{"type", "text"}, {"text", answer}});
    auto response = anthropic_response(calls, answer.empty() ? "tool_use" : "end_turn");
    return {200, stream ? "text/event-stream" : "application/json", stream ? anthropic_stream(response) : response.dump()};
}

std::string summary_system_text(const Json& body, ProviderApi api) {
    if (api == ProviderApi::responses) return body.at("instructions").get<std::string>();
    if (api == ProviderApi::anthropic) return body.at("system").get<std::string>();
    if (api == ProviderApi::google) return body.at("systemInstruction").at("parts").back().at("text").get<std::string>();
    return body.at("messages").front().at("content").get<std::string>();
}

TEST(ProviderClientSummary, CatalogueUsesEffectiveToolsAndCountsFullSystemTextOnce) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses, ProviderApi::anthropic, ProviderApi::google}) {
        auto definition = network_definition(80, false);
        definition.provider.config.api = api;
        ProviderClient client(shared_definition(definition));
        Transcript transcript;
        auto input = client_request(transcript, 1, "Recall");
        input.summary_context = "Saved conversation summaries for this forum\nSummary catalogue JSON data:\n{\"items\":[{\"note_id\":42}]}";
        input.summary_tool = [](auto, auto, const auto&) { return "{}"; };
        const auto payload = client.prepare(input);
        const auto text = summary_system_text(Json::parse(payload.bytes), api);
        const auto at = text.find(input.summary_context);
        ASSERT_NE(at, std::string::npos);
        EXPECT_EQ(text.find(input.summary_context, at + 1), std::string::npos);
        EXPECT_LT(at, text.find("<tool_availability>"));
        ASSERT_TRUE(payload.text_sizes);
        EXPECT_EQ(payload.text_sizes->system_prompt_bytes, text.size());
        input.summary_context.clear();
        const auto empty = client.prepare(input);
        EXPECT_EQ(summary_system_text(Json::parse(empty.bytes), api).find("Summary catalogue JSON data"), std::string::npos);
        input.summary_context = "CATALOGUE-MARKER";
        input.include_tool_instructions = false;
        EXPECT_EQ(client.prepare(input).bytes.find(input.summary_context), std::string::npos);
        input.include_tool_instructions = true;
        input.summary_tool = {};
        EXPECT_EQ(client.prepare(input).bytes.find(input.summary_context), std::string::npos);
    }
}

TEST(ProviderClientSummary, SummaryOnlySearchReadAnswerAcrossAllApisAndStreamingModes) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses, ProviderApi::anthropic, ProviderApi::google}) {
        for (bool stream : {false, true}) {
            SCOPED_TRACE(std::to_string(static_cast<int>(api)) + (stream ? " stream" : " plain"));
            auto definition = network_definition(80, stream);
            definition.provider.config.api = api;
            std::vector<Json> requests;
            ProviderClient client(shared_definition(definition), nullptr, [&](const ProviderHttpRequest& request, const auto&) {
                requests.push_back(Json::parse(request.body));
                const auto round = requests.size();
                if (round == 1) return summary_mock_reply(api, stream, Json::array({summary_mock_call(api, "search-id", "summary_search", {{"query", "preference"}, {"before_note_id", nullptr}})}));
                if (round == 2) return summary_mock_reply(api, stream, Json::array({summary_mock_call(api, "read-id", "summary_read", {{"note_id", 42}})}));
                return summary_mock_reply(api, stream, Json::array(), "Historical preference.");
            });
            Transcript transcript;
            auto input = client_request(transcript, 1, "What did we decide?");
            input.summary_context = "CATALOGUE-SNAPSHOT-42";
            std::vector<std::string> calls;
            input.summary_tool = [&](std::string_view name, std::string_view arguments, const auto&) {
                calls.emplace_back(name);
                const auto args = Json::parse(arguments);
                if (name == "summary_search") {
                    EXPECT_EQ(args["before_note_id"], nullptr);
                    return std::string(R"({"items":[{"note_id":42,"name":"Preference"}],"next_before_note_id":null,"skipped_invalid_count":0})");
                }
                EXPECT_EQ(args["note_id"], 42);
                return std::string(R"({"note_id":42,"name":"Preference","text":"Saved preference","truncated":false})");
            };
            auto payload = client.prepare(input);
            ASSERT_TRUE(payload.summary_tool);
            const auto first = Json::parse(payload.bytes);
            EXPECT_NE(first.dump().find("Do not retrieve unrelated material"), std::string::npos);
            EXPECT_NE(first.dump().find("Summary search and reading are available"), std::string::npos);
            GenerationStatistics stats;
            payload.statistics = &stats;
            payload.tool_progress = [](auto) { ADD_FAILURE() << "Summary calls must not emit web progress"; };
            std::string answer;
            const auto result = client.perform(std::move(payload), [&](GenerationDelta delta) {
                if (delta.kind == GenerationDeltaKind::answer) answer += delta.text;
            }, std::atomic_bool{false});
            EXPECT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
            EXPECT_EQ(answer, "Historical preference.");
            EXPECT_EQ(calls, (std::vector<std::string>{"summary_search", "summary_read"}));
            ASSERT_EQ(requests.size(), 3u);
            for (const auto& request : requests) {
                const auto text = summary_system_text(request, api);
                const auto at = text.find(input.summary_context);
                ASSERT_NE(at, std::string::npos);
                EXPECT_EQ(text.find(input.summary_context, at + 1), std::string::npos);
            }
            EXPECT_NE(requests.back().dump().find("Saved preference"), std::string::npos);
            EXPECT_NE(requests.back().dump().find("read-id"), std::string::npos);
            EXPECT_EQ(stats.summary.calls, 2u);
            EXPECT_EQ(stats.summary.failed, 0u);
            EXPECT_GT(stats.summary.duration.count(), 0);
            EXPECT_GT(stats.summary.result_bytes, 0u);
            EXPECT_EQ(stats.web_search.calls, 0u);
            EXPECT_EQ(stats.web_read.calls, 0u);
            EXPECT_EQ(stats.tool_calls_requested, 2u);
            EXPECT_EQ(result.usage.input_tokens, api == ProviderApi::anthropic ? 12u : 30u);
        }
    }
}

TEST(ProviderClientSummary, SeparateWebAllowanceAndOverallLimitPreserveMixedCallIdsAndSignedPrefixes) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses, ProviderApi::anthropic, ProviderApi::google}) {
        auto definition = network_definition(80, false);
        definition.provider.config.api = api;
        std::vector<Json> sent;
        int searches = 0, summaries = 0;
        ProviderClient client(shared_definition(definition), nullptr, [&](const ProviderHttpRequest& request, const auto&) {
            sent.push_back(Json::parse(request.body));
            if (sent.size() == 1) {
                Json calls = Json::array();
                calls.push_back(summary_mock_call(api, "summary-first", "summary_read", {{"note_id", 1}}));
                for (int i = 0; i < 5; ++i) calls.push_back(summary_mock_call(api, "web" + std::to_string(i), "web_search", {{"query", "paid"}}));
                calls.push_back(summary_mock_call(api, "summary-last", "summary_read", {{"note_id", 2}}));
                return summary_mock_reply(api, false, calls);
            }
            if (sent.size() == 2) {
                const auto text = sent.back().dump();
                EXPECT_NE(text.find("web-call limit was reached"), std::string::npos);
                EXPECT_NE(text.find("summary-first"), std::string::npos);
                EXPECT_NE(text.find("summary-last"), std::string::npos);
                Json calls = Json::array();
                for (int i = 0; i < 10; ++i) calls.push_back(summary_mock_call(api, "more" + std::to_string(i), "summary_read", {{"note_id", 3}}));
                return summary_mock_reply(api, false, calls);
            }
            const auto& final = sent.back();
            if (api == ProviderApi::anthropic) {
                EXPECT_EQ(final["tool_choice"]["type"], "none");
                EXPECT_EQ(final["system"], sent.front()["system"]);
                EXPECT_EQ(final["tools"], sent.front()["tools"]);
            } else if (api == ProviderApi::google) {
                EXPECT_EQ(final["toolConfig"]["functionCallingConfig"]["mode"], "NONE");
                EXPECT_NE(final["systemInstruction"].dump().find("Summary search and reading are unavailable"), std::string::npos);
            } else EXPECT_FALSE(final.contains("tools"));
            return summary_mock_reply(api, false, Json::array(), "Done.");
        });
        Transcript transcript;
        auto input = client_request(transcript, 1, "Recall and verify");
        input.summary_context = "CATALOGUE-SNAPSHOT-FINAL";
        input.web_search_tool = [&](auto, const auto&) { ++searches; return "{}"; };
        input.summary_tool = [&](auto, auto, const auto&) { ++summaries; return R"({"error":"invalid_summary","message":"invalid"})"; };
        GenerationStatistics stats;
        auto payload = client.prepare(input); payload.statistics = &stats;
        const auto result = client.perform(std::move(payload), [](auto) {}, std::atomic_bool{false});
        EXPECT_EQ(result.outcome, GenerationOutcome::completed) << result.message;
        EXPECT_EQ(searches, 4);
        EXPECT_EQ(summaries, 11); // 16 total attempts minus 5 web attempts, including the denied fifth.
        EXPECT_EQ(stats.summary.failed, 11u);
        EXPECT_EQ(stats.web_search.calls, 4u);
        EXPECT_EQ(stats.tool_calls_rejected, 2u);
        EXPECT_EQ(sent.size(), 3u);
        for (const auto& request : sent) {
            const auto text = summary_system_text(request, api);
            const auto at = text.find(input.summary_context);
            ASSERT_NE(at, std::string::npos);
            EXPECT_EQ(text.find(input.summary_context, at + 1), std::string::npos);
        }
    }
}

TEST(ProviderClientSummary, CallbackCancellationPreventsModelContinuation) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses, ProviderApi::anthropic, ProviderApi::google}) {
        auto definition = network_definition(80, false); definition.provider.config.api = api;
        int requests = 0;
        ProviderClient client(shared_definition(definition), nullptr, [&](const auto&, const auto&) {
            ++requests;
            return summary_mock_reply(api, false, Json::array({summary_mock_call(api, "cancel", "summary_read", {{"note_id", 1}})}));
        });
        Transcript transcript;
        auto input = client_request(transcript, 1, "Recall");
        std::atomic_bool cancellation{false};
        input.summary_tool = [&](auto, auto, const auto&) {
            cancellation.store(true);
            return R"({"error":"cancelled","message":"cancelled"})";
        };
        GenerationStatistics stats;
        auto payload = client.prepare(input); payload.statistics = &stats;
        EXPECT_EQ(client.perform(std::move(payload), [](auto) {}, cancellation).outcome, GenerationOutcome::cancelled);
        EXPECT_EQ(requests, 1);
        EXPECT_EQ(stats.summary.cancelled, 1u);
    }
}

TEST(ProviderClientSummary, RequiredHostedSearchExcludesSummariesForTheWholeGeneration) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses}) {
        auto definition = network_definition(443, false);
        definition.provider.config.api = api;
        definition.provider.config.host = "openrouter.ai";
        definition.provider.config.web_search = WebSearchMode::required;
        std::vector<Json> requests;
        ProviderClient client(shared_definition(definition), nullptr, [&](const ProviderHttpRequest& request, const auto&) {
            const auto body = Json::parse(request.body);
            requests.push_back(body);
            EXPECT_EQ(body.dump().find("\"name\":\"summary_search\""), std::string::npos);
            EXPECT_NE(body.dump().find("Summary search and reading are unavailable"), std::string::npos);
            if (requests.size() <= 2) return summary_mock_reply(api, false, Json::array({
                summary_mock_call(api, "forged" + std::to_string(requests.size()), "summary_read", {{"note_id", 42}})}));
            return summary_mock_reply(api, false, Json::array(), "Done.");
        });
        Transcript transcript;
        auto input = client_request(transcript, 1, "Research");
        input.web_search_tool = [](auto, const auto&) { return "{}"; };
        input.summary_context = "CATALOGUE-MARKER";
        input.summary_tool = [](auto, auto, const auto&) -> std::string { ADD_FAILURE(); return "{}"; };
        auto payload = client.prepare(input);
        EXPECT_FALSE(payload.summary_tool);
        EXPECT_EQ(payload.bytes.find("CATALOGUE-MARKER"), std::string::npos);
        EXPECT_EQ(client.perform(std::move(payload), [](auto) {}, std::atomic_bool{false}).outcome, GenerationOutcome::completed);
        ASSERT_EQ(requests.size(), 3u);
        EXPECT_NE(requests[1].dump().find("Summary tools are unavailable"), std::string::npos);
        EXPECT_EQ(requests.front()["tool_choice"], "required");
        EXPECT_EQ(requests.back()["tool_choice"], "auto");
    }
}

TEST(ProviderClientSummary, GoogleFileSearchCompatibilityAndRequestsWithoutSummaryAccessOmitCallbacks) {
    for (auto api : {ProviderApi::chat_completions, ProviderApi::responses, ProviderApi::anthropic, ProviderApi::google}) {
        auto definition = network_definition(80, false); definition.provider.config.api = api;
        ProviderClient client(shared_definition(definition));
        Transcript transcript;
        const auto payload = client.prepare(client_request(transcript, 1, "Self-contained"));
        EXPECT_FALSE(payload.summary_tool);
        EXPECT_EQ(payload.bytes.find("CATALOGUE-MARKER"), std::string::npos);
        EXPECT_FALSE(Json::parse(payload.bytes).contains("tools"));
    }
    for (const auto* model : {"gemini-2.5-flash", "gemini-3-flash"}) {
        auto definition = network_definition(80, false);
        definition.provider.config.api = ProviderApi::google; definition.provider.config.model = model;
        ProviderClient client(shared_definition(definition));
        Transcript transcript;
        auto input = client_request(transcript, 1, "Recall");
        input.folder_search = FolderSearchAccess{FolderStoreService::google, {"fileSearchStores/one"}};
        input.summary_context = "CATALOGUE-MARKER";
        input.summary_tool = [](auto, auto, const auto&) { return "{}"; };
        const auto payload = client.prepare(input);
        const auto body = Json::parse(payload.bytes);
        const bool available = std::string_view(model) != "gemini-2.5-flash";
        EXPECT_EQ(bool(payload.summary_tool), available);
        EXPECT_EQ(payload.bytes.find("CATALOGUE-MARKER") != std::string::npos, available);
        EXPECT_EQ(body["tools"].size(), available ? 2u : 1u);
        if (available) EXPECT_EQ(body["toolConfig"]["functionCallingConfig"]["mode"], "VALIDATED");
    }
    // Anthropic ignores the obsolete hosted-search setting; it must not hide effective summary tools.
    auto definition = network_definition(80, false);
    definition.provider.config.api = ProviderApi::anthropic;
    definition.provider.config.web_search = WebSearchMode::required;
    ProviderClient client(shared_definition(definition));
    Transcript transcript;
    auto input = client_request(transcript, 1, "Recall");
    input.summary_tool = [](auto, auto, const auto&) { return "{}"; };
    EXPECT_TRUE(client.prepare(input).summary_tool);
}


TEST(ProviderClientSummary, IgnoredSubscriptionHostedSearchSettingDoesNotSuppressSummaryFunctions) {
    SubscriptionOwner owner;
    OpenAiOAuth oauth = owner.make_connected();
    auto definition = subscription_definition();
    definition.provider.config.web_search = WebSearchMode::required;
    ProviderClient client(shared_definition(definition), &oauth);
    Transcript transcript;
    auto input = client_request(transcript, 1, "Recall");
    input.summary_tool = [](auto, auto, const auto&) { return "{}"; };
    const auto payload = client.prepare(input);
    EXPECT_TRUE(payload.summary_tool);
    EXPECT_NE(payload.bytes.find("summary_search"), std::string::npos);
    EXPECT_EQ(payload.bytes.find("\"type\":\"web_search\""), std::string::npos);
}

} // namespace
} // namespace cha
