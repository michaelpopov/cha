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
        EXPECT_EQ(body["instructions"], "System prompt");
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
    EXPECT_EQ(body["instructions"], "You are a helpful assistant.");
    EXPECT_EQ(
        text_sizes.system_prompt_bytes,
        std::string("You are a helpful assistant.").size());
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

TEST(ResponsesApi, OmitsEmptyInstructionsAndReasoningAndSearchFields) {
    Transcript transcript;
    const GenerationRequest request = make_request(transcript, "Hi");
    ModelBackendConfig config = responses_config();
    config.temperature.reset();
    const Json body = Json::parse(build_responses_request_body(
        request, config, ""));

    EXPECT_FALSE(body.contains("instructions"));
    EXPECT_FALSE(body.contains("reasoning"));
    EXPECT_FALSE(body.contains("temperature"));
    EXPECT_FALSE(body.contains("max_output_tokens"));
    EXPECT_FALSE(body.contains("tools"));
    EXPECT_FALSE(body.contains("tool_choice"));
    ASSERT_EQ(body["input"].size(), 1U);
    EXPECT_EQ(body["input"][0]["role"], "user");
    EXPECT_EQ(body["input"][0]["content"], "from You:\nHi");
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
