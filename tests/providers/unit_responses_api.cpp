#include "providers/responses_api.h"
#include "chat/transcript.h"
#include "support/test_transcript.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

namespace cha {
namespace {

using Json = nlohmann::json;

class Output {
public:
    Output() = default;
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    const GenerationDeltaSink& sink() const { return sink_; }

    const std::vector<GenerationDelta>& deltas() const { return deltas_; }

    std::string text(GenerationDeltaKind kind) const {
        std::string result;
        for (const GenerationDelta& delta : deltas_) {
            if (delta.kind == kind) {
                result += delta.text;
            }
        }
        return result;
    }

    std::string answer() const { return text(GenerationDeltaKind::answer); }

private:
    std::vector<GenerationDelta> deltas_;
    GenerationDeltaSink sink_ = [this](GenerationDelta delta) {
        deltas_.push_back(std::move(delta));
    };
};

GenerationRequest make_request(
    Transcript& transcript,
    std::string prompt,
    std::vector<TranscriptEntry> history = {}) {
    for (TranscriptEntry& entry : history) {
        transcript.add_entry(std::move(entry));
    }
    GenerationRequest input{
        .history = std::make_shared<const ModelHistory>(transcript.model_history()),
        .run = {
            .request_id = 1,
            .target = {"assistant", "Assistant"},
            .author = {"human", "You"},
            .prompt_text = std::move(prompt),
        },
    };
    transcript.add_entry(test::human_entry(
        1001, {"human", "You"}, {"assistant", "Assistant"},
        input.run.prompt_text, 1));
    return input;
}

ModelBackendConfig responses_config(
    WebSearchMode search = WebSearchMode::off,
    bool stream = true) {
    ModelBackendConfig config;
    config.model = "test-model";
    config.stream = stream;
    config.temperature = 0.5;
    config.api = ProviderApi::responses;
    config.web_search = search;
    return config;
}

TEST(ResponsesApi, BuildsRequestFieldsAndMapsRoles) {
    Transcript transcript;
    const GenerationRequest request = make_request(
        transcript, "Current question", {
            test::human_entry(
                1, {"human", "You"}, {"assistant", "Assistant"},
                "Earlier question", 6),
            make_character_entry(
                2, "assistant", "Assistant", "Earlier answer",
                EntryStatus::complete, 6),
        });
    struct Case {
        WebSearchMode search;
        const char* host;
        const char* tool;
        const char* choice;
    };
    const Case cases[]{
        {WebSearchMode::automatic, "example.test", "web_search", "auto"},
        {WebSearchMode::required, "example.test", "web_search", "required"},
        {WebSearchMode::automatic, "OPENROUTER.AI.", "openrouter:web_search", "auto"},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.host);
        SCOPED_TRACE(item.choice);
        ModelBackendConfig config = responses_config(item.search);
        config.host = item.host;
        config.reasoning_effort = "none";
        config.max_tokens = 8;
        const Json body = Json::parse(build_responses_request_body(
            request, config, "System prompt"));

        EXPECT_EQ(body["model"], "test-model");
        EXPECT_TRUE(body["stream"]);
        EXPECT_FALSE(body["store"]);
        EXPECT_DOUBLE_EQ(body["temperature"].get<double>(), 0.5);
        EXPECT_EQ(body["max_output_tokens"], 16);
        EXPECT_TRUE(body["instructions"].get<std::string>().starts_with("System prompt\n\n"));
        EXPECT_NE(body["instructions"].get<std::string>().find(
            "Web search is available for this request."), std::string::npos);
        EXPECT_EQ(body["reasoning"]["effort"], "none");
        EXPECT_FALSE(body.contains("reasoning_effort"));
        EXPECT_EQ(body["tools"], Json::array({Json{{"type", item.tool}}}));
        EXPECT_EQ(body["tool_choice"], item.choice);
        EXPECT_FALSE(body.contains("include"));
        EXPECT_FALSE(body.contains("previous_response_id"));
        EXPECT_FALSE(body.contains("conversation"));
        EXPECT_EQ(body["input"], Json::array({
            {{"role", "user"}, {"content", "from You:\nEarlier question"}},
            {{"role", "assistant"}, {"content", "Earlier answer"}},
            {{"role", "user"}, {"content", "from You:\nCurrent question"}},
        }));
    }
}

TEST(ResponsesApi, SubscriptionBodyUsesFallbackInstructionsAndOmitsExtras) {
    Transcript transcript;
    GenerationRequest request = make_request(
        transcript, "Current question", {
            test::human_entry(
                1, {"human", "You"}, {"assistant", "Assistant"},
                "Earlier question", 6),
            make_character_entry(
                2, "assistant", "Assistant", "Earlier answer",
                EntryStatus::complete, 6),
        });
    ModelBackendConfig config = responses_config(WebSearchMode::automatic);
    config.auth = ProviderAuth::openai_subscription;
    config.host = "api.openai.com";
    config.temperature = 0.5;
    config.max_tokens = 64;
    config.cache_retention = CacheRetention::short_;
    request.run.prompt_cache_key = "cache-key";

    RequestTextSizes text_sizes;
    const Json body = Json::parse(build_responses_request_body(
        request, config, "", &text_sizes));

    EXPECT_EQ(body["model"], "test-model");
    EXPECT_TRUE(body["stream"]);
    EXPECT_FALSE(body["store"]);
    const auto instructions = body["instructions"].get<std::string>();
    EXPECT_TRUE(instructions.starts_with("You are a helpful assistant.\n\n"));
    EXPECT_NE(instructions.find("Web search is unavailable for this request."), std::string::npos);
    EXPECT_EQ(
        text_sizes.system_prompt_bytes,
        instructions.size());
    EXPECT_FALSE(body.contains("temperature"));
    EXPECT_FALSE(body.contains("max_output_tokens"));
    EXPECT_FALSE(body.contains("tools"));
    EXPECT_FALSE(body.contains("tool_choice"));
    EXPECT_FALSE(body.contains("prompt_cache_key"));
    EXPECT_FALSE(body.contains("session_id"));
    EXPECT_EQ(body["input"], Json::array({
        {{"role", "user"}, {"content", "from You:\nEarlier question"}},
        {{"role", "assistant"}, {"content", "Earlier answer"}},
        {{"role", "user"}, {"content", "from You:\nCurrent question"}},
    }));
}

TEST(ResponsesApi, DefaultsToNoneReasoningAndAddsToolInstructionsWithoutSystemPrompt) {
    Transcript transcript;
    const GenerationRequest request = make_request(transcript, "Hi");
    ModelBackendConfig config = responses_config();
    config.temperature.reset();
    const Json body = Json::parse(build_responses_request_body(
        request, config, ""));

    EXPECT_TRUE(body["instructions"].get<std::string>().starts_with("<tool_availability>\n"));
    EXPECT_EQ(body["reasoning"]["effort"], "none");
    EXPECT_FALSE(body.contains("temperature"));
    EXPECT_FALSE(body.contains("max_output_tokens"));
    EXPECT_FALSE(body.contains("tools"));
    EXPECT_FALSE(body.contains("tool_choice"));
    ASSERT_EQ(body["input"].size(), 1U);
    EXPECT_EQ(body["input"][0]["role"], "user");
    EXPECT_EQ(body["input"][0]["content"], "from You:\nHi");
}

TEST(ResponsesApi, OmitsUnsupportedReasoningForDirectOpenAiGpt4o) {
    Transcript transcript;
    const GenerationRequest request = make_request(transcript, "Hi");
    ModelBackendConfig config = responses_config();
    config.host = "API.OPENAI.COM.";
    for (const char* model : {"gpt-4o", "gpt-4o-2024-08-06", "gpt-4o-mini"}) {
        SCOPED_TRACE(model);
        config.model = model;
        for (const char* effort : {"none", "high"}) {
            SCOPED_TRACE(effort);
            config.reasoning_effort = effort;
            const Json body = Json::parse(build_responses_request_body(
                request, config, ""));
            EXPECT_EQ(body["model"], model);
            EXPECT_FALSE(body.contains("reasoning"));
        }
    }

    config.model = "gpt-6-astra";
    const Json body = Json::parse(build_responses_request_body(
        request, config, ""));
    EXPECT_EQ(body["reasoning"]["effort"], "high");
}

TEST(ResponsesApi, RejectsInvalidUtf8InRequestBody) {
    Transcript transcript;
    const GenerationRequest request = make_request(
        transcript, std::string("\xc0\x80", 2));
    EXPECT_THROW(
        (void)build_responses_request_body(
            request, responses_config(), "ok"),
        std::runtime_error);
    try {
        (void)build_responses_request_body(
            request, responses_config(), "ok");
    } catch (const std::runtime_error& error) {
        EXPECT_EQ(std::string(error.what()), "Model request contains invalid UTF-8");
    }
}

TEST(ResponsesApi, DecodesTwoTextDeltasThenCompletion) {
    Output output;
    ResponsesStreamDecoder decoder(output.sink());
    decoder.consume(
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Hello\"}\n\n");
    decoder.consume(
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\" world\"}\n\n");
    decoder.consume(
        "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\",\"usage\":{\"input_tokens\":12,\"output_tokens\":5,\"input_tokens_details\":{\"cached_tokens\":9,\"cache_write_tokens\":7}}}}\n\n");
    const StreamDecodeResult result = decoder.finish();
    EXPECT_EQ(result.result.outcome, GenerationOutcome::completed);
    EXPECT_FALSE(result.describe_response);
    EXPECT_EQ(output.answer(), "Hello world");
    EXPECT_EQ(output.deltas().size(), 2U);
    ASSERT_TRUE(result.result.usage.input_tokens);
    ASSERT_TRUE(result.result.usage.output_tokens);
    EXPECT_EQ(*result.result.usage.input_tokens, 12U);
    EXPECT_EQ(*result.result.usage.output_tokens, 5U);
    ASSERT_TRUE(result.result.usage.cache_read_tokens);
    EXPECT_EQ(*result.result.usage.cache_read_tokens, 9U);
    ASSERT_TRUE(result.result.usage.cache_write_tokens);
    EXPECT_EQ(*result.result.usage.cache_write_tokens, 7U);
}

TEST(ResponsesApi, KeepsSearchAnnotationsAndReasoningEventsPrivate) {
    Output output;
    ResponsesStreamDecoder decoder(output.sink());
    decoder.consume(
        "data: {\"type\":\"response.web_search_call.in_progress\"}\n\n"
        "data: {\"type\":\"response.web_search_call.searching\"}\n\n"
        "data: {\"type\":\"response.web_search_call.completed\"}\n\n"
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Answer[source]\"}\n\n"
        "data: {\"type\":\"response.output_text.annotation.added\","
        "\"annotation\":{\"type\":\"url_citation\",\"start_index\":6,"
        "\"end_index\":14,\"title\":\"Primary source\","
        "\"url\":\"https://example.com/source\"}}\n\n"
        "data: {\"type\":\"response.reasoning_summary_text.delta\",\"delta\":\"secret\"}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n\n");
    const StreamDecodeResult result = decoder.finish();
    EXPECT_EQ(result.result.outcome, GenerationOutcome::completed);
    EXPECT_EQ(output.answer(), "Answer[source]");
    EXPECT_EQ(output.deltas().size(), 1U);
}

TEST(ResponsesApi, EmitsRefusalDeltaAsAnswer) {
    Output output;
    ResponsesStreamDecoder decoder(output.sink());
    decoder.consume(
        "data: {\"type\":\"response.refusal.delta\",\"delta\":\"I cannot help\"}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n\n");
    const StreamDecodeResult result = decoder.finish();
    EXPECT_EQ(result.result.outcome, GenerationOutcome::completed);
    EXPECT_EQ(output.answer(), "I cannot help");
}

TEST(ResponsesApi, ReportsInvalidAndFailedEvents) {
    struct Case {
        const char* stream;
        const char* error;
        bool describe_response;
        const char* answer;
    };
    const Case cases[]{
        {"data: not-json\n\n"
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"Partial\"}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n\n",
         "malformed JSON", true, "Partial"},
        {"data: {\"type\":\"response.output_text.delta\",\"delta\":1}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n\n",
         "string delta", true, ""},
        {"data: {\"type\":\"error\",\"message\":\"quota exceeded\"}\n\n",
         "quota exceeded", false, ""},
        {"data: {\"type\":\"response.failed\",\"response\":"
            "{\"error\":{\"message\":\"backend down\"}}}\n\n",
         "backend down", false, ""},
        {"data: {\"type\":\"response.incomplete\",\"response\":"
            "{\"incomplete_details\":{\"reason\":\"max_output_tokens\"}}}\n\n",
         "max_output_tokens", false, ""},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.error);
        Output output;
        ResponsesStreamDecoder decoder(output.sink());
        decoder.consume(item.stream);
        const StreamDecodeResult result = decoder.finish();
        EXPECT_EQ(result.result.outcome, GenerationOutcome::protocol_error);
        EXPECT_NE(result.result.message.find(item.error), std::string::npos);
        EXPECT_EQ(result.describe_response, item.describe_response);
        EXPECT_EQ(output.answer(), item.answer);
    }
}

std::string sse_event(const Json& value) {
    return "data: " + value.dump() + "\n\n";
}

Json function_call_item(std::string name, std::string arguments) {
    return {
        {"type", "function_call"},
        {"call_id", "call_1"},
        {"name", std::move(name)},
        {"arguments", std::move(arguments)},
    };
}

TEST(ResponsesApi, BoundsStoredFunctionCallText) {
    constexpr std::size_t limit = 8;
    const Json call = function_call_item(std::string(40, 'n'), "{}");
    const auto expect_bounded = [&](bool include_output) {
        Output output;
        ResponsesStreamDecoder decoder(output.sink(), true, limit);
        decoder.consume(sse_event({
            {"type", "response.output_item.done"},
            {"output_index", 0},
            {"item", call},
        }));
        Json response{{"status", "completed"}};
        if (include_output) response["output"] = Json::array({call});
        decoder.consume(sse_event({
            {"type", "response.completed"},
            {"response", std::move(response)},
        }));
        const StreamDecodeResult result = decoder.finish();
        EXPECT_EQ(result.result.outcome, GenerationOutcome::completed) << result.result.message;
        ASSERT_EQ(result.result.tool_calls.size(), 1u);
        EXPECT_EQ(result.result.tool_calls[0].name.size(), limit + 1);
        EXPECT_EQ(result.result.tool_calls[0].arguments, "{}");
        const Json continuation = Json::parse(result.result.continuation);
        ASSERT_TRUE(continuation.is_array());
        ASSERT_FALSE(continuation.empty());
        EXPECT_EQ(continuation[0]["name"].get<std::string>().size(), limit + 1);
        EXPECT_EQ(continuation[0]["arguments"].get<std::string>(), "{}");
    };
    expect_bounded(false);
    expect_bounded(true);

    const Json oversized = function_call_item(
        "vault_config_list", std::string(limit + 20, 'x'));
    Output output;
    ResponsesStreamDecoder decoder(output.sink(), true, limit);
    decoder.consume(sse_event({
        {"type", "response.function_call_arguments.delta"},
        {"delta", std::string(limit + 20, 'x')},
    }));
    decoder.consume(sse_event({
        {"type", "response.output_item.done"},
        {"output_index", 0},
        {"item", oversized},
    }));
    decoder.consume(sse_event({
        {"type", "response.completed"},
        {"response", {{"status", "completed"}, {"output", Json::array({oversized})}}},
    }));
    const StreamDecodeResult result = decoder.finish();
    EXPECT_EQ(result.result.outcome, GenerationOutcome::protocol_error);
    EXPECT_EQ(result.result.message, "Tool arguments too large");
    EXPECT_TRUE(result.result.tool_calls.empty());
}

TEST(ResponsesApi, LimitsStreamedArgumentsPerCall) {
    constexpr std::size_t limit = 8;
    const auto incomplete = [](std::vector<std::pair<int, std::string>> deltas) {
        Output output;
        ResponsesStreamDecoder decoder(output.sink(), true, limit);
        for (const auto& [index, delta] : deltas) {
            decoder.consume(sse_event({
                {"type", "response.function_call_arguments.delta"},
                {"output_index", index},
                {"delta", delta},
            }));
        }
        decoder.consume(sse_event({
            {"type", "response.incomplete"},
            {"response", {{"incomplete_details", {{"reason", "content_filter"}}}}},
        }));
        return decoder.finish();
    };

    const StreamDecodeResult separate = incomplete({
        {0, std::string(limit, 'a')},
        {1, std::string(limit, 'b')},
    });
    EXPECT_EQ(separate.result.outcome, GenerationOutcome::protocol_error);
    EXPECT_EQ(separate.result.message, "Responses stream ended incomplete: content_filter");

    const StreamDecodeResult one_call = incomplete({
        {0, std::string(limit, 'a')},
        {0, "x"},
    });
    EXPECT_EQ(one_call.result.message, "Tool arguments too large");
}

TEST(ResponsesApi, RequiresCompletionAndAnswerText) {
    struct Case {
        const char* stream;
        const char* error;
        bool describe_response;
        const char* answer;
    };
    const Case cases[]{
        {"data: {\"type\":\"response.output_text.delta\",\"delta\":\"Partial\"}\n\n",
         "response.completed", true, "Partial"},
        {"data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n\n",
         "without answer content", false, ""},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.error);
        Output output;
        ResponsesStreamDecoder decoder(output.sink());
        decoder.consume(item.stream);
        const StreamDecodeResult result = decoder.finish();
        EXPECT_EQ(result.result.outcome, GenerationOutcome::protocol_error);
        EXPECT_NE(result.result.message.find(item.error), std::string::npos);
        EXPECT_EQ(result.describe_response, item.describe_response);
        EXPECT_EQ(output.answer(), item.answer);
    }
}

TEST(ResponsesApi, IgnoresNonStreamingSearchAndAnnotationMetadata) {
    Output output;
    const std::string body = R"({
        "status": "completed",
        "usage": {"input_tokens": 12, "output_tokens": 5,
                  "input_tokens_details": {
                      "cached_tokens": 9,
                      "cache_write_tokens": 7
                  }},
        "output": [
            {"type": "reasoning", "summary": []},
            {
                "type": "web_search_call",
                "status": "completed",
                "action": {"type": "search", "query": "private query"}
            },
            {
                "type": "message",
                "role": "assistant",
                "content": [
                    {
                        "type": "output_text",
                        "text": "Public answer[source]",
                        "annotations": [
                            {
                                "type": "url_citation",
                                "start_index": 13,
                                "end_index": 21,
                                "title": "Example source",
                                "url": "https://example.com/source"
                            }
                        ]
                    }
                ]
            }
        ]
    })";
    const GenerationResult result = decode_responses_response(body, output.sink());
    EXPECT_EQ(result.outcome, GenerationOutcome::completed);
    EXPECT_EQ(output.answer(), "Public answer[source]");
    EXPECT_EQ(output.deltas().size(), 1U);
    ASSERT_TRUE(result.usage.input_tokens);
    ASSERT_TRUE(result.usage.output_tokens);
    EXPECT_EQ(*result.usage.input_tokens, 12U);
    EXPECT_EQ(*result.usage.output_tokens, 5U);
    ASSERT_TRUE(result.usage.cache_read_tokens);
    EXPECT_EQ(*result.usage.cache_read_tokens, 9U);
    ASSERT_TRUE(result.usage.cache_write_tokens);
    EXPECT_EQ(*result.usage.cache_write_tokens, 7U);
}

TEST(ResponsesApi, DecodesNonStreamingRefusalAsAnswer) {
    Output output;
    const std::string body = R"({
        "status": "completed",
        "output": [{
            "type": "message",
            "role": "assistant",
            "content": [{"type": "refusal", "refusal": "No"}]
        }]
    })";
    const GenerationResult result = decode_responses_response(body, output.sink());
    EXPECT_EQ(result.outcome, GenerationOutcome::completed);
    EXPECT_EQ(output.answer(), "No");
}

TEST(ResponsesApi, ReportsMalformedOrFailedNonStreamingBodies) {
    Output output;
    EXPECT_EQ(
        decode_responses_response("{}", output.sink()).outcome,
        GenerationOutcome::protocol_error);
    EXPECT_EQ(
        decode_responses_response(R"({"status":"completed","output":null})", output.sink())
            .outcome,
        GenerationOutcome::protocol_error);
    EXPECT_EQ(
        decode_responses_response(
            R"({"status":"failed","error":{"message":"boom"}})",
            output.sink()).outcome,
        GenerationOutcome::protocol_error);
    EXPECT_EQ(
        decode_responses_response(
            R"({"status":"incomplete","incomplete_details":{"reason":"max_output_tokens"}})",
            output.sink()).outcome,
        GenerationOutcome::protocol_error);
    const GenerationResult failed = decode_responses_response(
        R"({"status":"failed","error":{"message":"boom"}})",
        output.sink());
    EXPECT_NE(failed.message.find("boom"), std::string::npos);
}

} // namespace
} // namespace cha
