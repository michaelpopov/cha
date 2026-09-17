#include "providers/chat_completions_api.h"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cha {
namespace {

// Collects what one decoded response reported, so a test can assert the deltas
// and their order without running a transfer.
class Output {
public:
    Output() = default;
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;

    // The recorder owns the sink because a decoder borrows it, so every test
    // declares its Output before the decoder that reports through it.
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
    std::string reasoning() const { return text(GenerationDeltaKind::reasoning); }

private:
    std::vector<GenerationDelta> deltas_;
    GenerationDeltaSink sink_ = [this](GenerationDelta delta) {
        deltas_.push_back(std::move(delta));
    };
};

constexpr std::string_view two_part_stream =
    "data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n"
    "data: {\"choices\":[{\"delta\":{\"content\":\" world\"}}]}\n\n"
    "data: [DONE]\n\n";

TEST(ChatCompletionsApi, IgnoresOpenRouterProcessingHeartbeats) {
    Output output;
    ChatCompletionsStreamDecoder decoder(ReasoningFormat::automatic, output.sink());

    decoder.consume(
        ": OPENROUTER PROCESSING\n\n"
        ": OPENROUTER PROCESSING\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"Answer\"}}]}\n\n"
        "data: [DONE]\n\n");
    const StreamDecodeResult result = decoder.finish();

    EXPECT_EQ(result.result.outcome, GenerationOutcome::completed);
    EXPECT_EQ(output.answer(), "Answer");
    EXPECT_EQ(output.deltas().size(), 1U);
}

TEST(ChatCompletionsApi, ReadsUsageFromTheFinalStreamingChunk) {
    Output output;
    ChatCompletionsStreamDecoder decoder(ReasoningFormat::automatic, output.sink());

    decoder.consume(
        "data: {\"choices\":[{\"delta\":{\"content\":\"Answer\"}}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":12,\"completion_tokens\":5,\"prompt_tokens_details\":{\"cached_tokens\":9,\"cache_write_tokens\":7}}}\n\n"
        "data: [DONE]\n\n");
    const StreamDecodeResult result = decoder.finish();

    EXPECT_EQ(result.result.outcome, GenerationOutcome::completed);
    ASSERT_TRUE(result.result.usage.input_tokens);
    ASSERT_TRUE(result.result.usage.output_tokens);
    EXPECT_EQ(*result.result.usage.input_tokens, 12U);
    EXPECT_EQ(*result.result.usage.output_tokens, 5U);
    ASSERT_TRUE(result.result.usage.cache_read_tokens);
    EXPECT_EQ(*result.result.usage.cache_read_tokens, 9U);
    ASSERT_TRUE(result.result.usage.cache_write_tokens);
    EXPECT_EQ(*result.result.usage.cache_write_tokens, 7U);
}

TEST(ChatCompletionsApi, ResolvesNonStreamingCacheUsage) {
    struct Case {
        const char* body;
        std::optional<std::uint64_t> cache_read;
        std::optional<std::uint64_t> cache_write;
    };
    const Case cases[]{
        {R"({"usage":{"prompt_tokens":12,"completion_tokens":5,"prompt_cache_hit_tokens":4},"choices":[{"message":{"content":"Answer"}}]})", 4, std::nullopt},
        {R"({"usage":{"prompt_tokens":12,"completion_tokens":5,"prompt_cache_hit_tokens":4,"prompt_tokens_details":{"cached_tokens":9,"cache_write_tokens":7}},"choices":[{"message":{"content":"Answer"}}]})", 9, 7},
        {R"({"usage":{"prompt_tokens":12,"completion_tokens":5},"choices":[{"message":{"content":"Answer"}}]})", std::nullopt, std::nullopt},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.body);
        Output output;
        const GenerationResult result = decode_chat_completions_response(
            item.body, ReasoningFormat::automatic, output.sink());
        EXPECT_EQ(result.outcome, GenerationOutcome::completed);
        EXPECT_EQ(result.usage.input_tokens, 12U);
        EXPECT_EQ(result.usage.output_tokens, 5U);
        EXPECT_EQ(result.usage.cache_read_tokens, item.cache_read);
        EXPECT_EQ(result.usage.cache_write_tokens, item.cache_write);
    }
}

TEST(ChatCompletionsApi, DecodesTheSameStreamOneByteAtATime) {
    Output output;
    ChatCompletionsStreamDecoder decoder(ReasoningFormat::automatic, output.sink());

    // Leave DONE unterminated so finish() must flush the trailing event.
    for (const char character : two_part_stream.substr(0, two_part_stream.size() - 2)) {
        decoder.consume(std::string_view(&character, 1));
    }
    const StreamDecodeResult result = decoder.finish();

    EXPECT_EQ(result.result.outcome, GenerationOutcome::completed);
    EXPECT_FALSE(result.describe_response);
    EXPECT_EQ(output.answer(), "Hello world");
    EXPECT_EQ(output.deltas().size(), 2U);
}

TEST(ChatCompletionsApi, ReportsMalformedStreamingEvents) {
    struct Case {
        const char* stream;
        const char* error;
        const char* answer;
    };
    const Case cases[]{
        {"data: not-json\n\n"
         "data: {\"choices\":[{\"delta\":{\"content\":\"Partial\"}}]}\n\n"
         "data: [DONE]\n\n", "malformed JSON", "Partial"},
        {"data: {\"object\":\"chunk\"}\n\ndata: [DONE]\n\n",
         "did not contain a choices array", ""},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.error);
        Output output;
        ChatCompletionsStreamDecoder decoder(ReasoningFormat::automatic, output.sink());
        decoder.consume(item.stream);
        const StreamDecodeResult result = decoder.finish();
        EXPECT_EQ(result.result.outcome, GenerationOutcome::protocol_error);
        EXPECT_NE(result.result.message.find(item.error), std::string::npos);
        EXPECT_TRUE(result.describe_response);
        EXPECT_EQ(output.answer(), item.answer);
    }
}

TEST(ChatCompletionsApi, ReportsAStreamThatEndedBeforeTheEndMarker) {
    Output output;
    ChatCompletionsStreamDecoder decoder(ReasoningFormat::automatic, output.sink());

    decoder.consume("data: {\"choices\":[{\"delta\":{\"content\":\"Partial\"}}]}\n\n");
    const StreamDecodeResult result = decoder.finish();

    EXPECT_EQ(result.result.outcome, GenerationOutcome::protocol_error);
    EXPECT_NE(
        result.result.message.find("ended before [DONE]"),
        std::string::npos);
    EXPECT_TRUE(result.describe_response);
    EXPECT_EQ(output.answer(), "Partial");
}

TEST(ChatCompletionsApi, ReportsResponseBytesThatWereNotAnEventStream) {
    Output output;
    ChatCompletionsStreamDecoder decoder(ReasoningFormat::automatic, output.sink());

    decoder.consume(R"({"error":{"message":"model unavailable"}})");
    const StreamDecodeResult result = decoder.finish();

    EXPECT_EQ(result.result.outcome, GenerationOutcome::protocol_error);
    EXPECT_NE(
        result.result.message.find("was not valid SSE"),
        std::string::npos);
    EXPECT_EQ(result.result.message.find("model unavailable"), std::string::npos);
    EXPECT_TRUE(result.describe_response);
    EXPECT_TRUE(output.deltas().empty());
}

TEST(ChatCompletionsApi, IgnoresDataAfterTheEndMarker) {
    Output output;
    ChatCompletionsStreamDecoder decoder(ReasoningFormat::automatic, output.sink());

    decoder.consume(
        "data: {\"choices\":[{\"delta\":{\"content\":\"Complete\"}}]}\n\n"
        "data: [DONE]\n"
        "data: not-json\n\n");
    decoder.consume("data: {\"choices\":[{\"delta\":{\"content\":\" ignored\"}}]}\n\n");
    const StreamDecodeResult result = decoder.finish();

    EXPECT_EQ(result.result.outcome, GenerationOutcome::completed);
    EXPECT_EQ(output.answer(), "Complete");
}

TEST(ChatCompletionsApi, RejectsACompletedStreamWithoutAnswerContent) {
    Output output;
    ChatCompletionsStreamDecoder decoder(ReasoningFormat::automatic, output.sink());

    decoder.consume(
        "data: {\"choices\":[{\"delta\":{"
        "\"reasoning_content\":\"PRIVATE_ONLY_REASONING\"}}]}\n\n"
        "data: [DONE]\n\n");
    const StreamDecodeResult result = decoder.finish();

    EXPECT_EQ(result.result.outcome, GenerationOutcome::protocol_error);
    EXPECT_NE(
        result.result.message.find("without answer content"),
        std::string::npos);
    EXPECT_EQ(
        result.result.message.find("PRIVATE_ONLY_REASONING"),
        std::string::npos);
    // A complete stream explains itself; the caller adds no response metadata.
    EXPECT_FALSE(result.describe_response);
    EXPECT_EQ(output.reasoning(), "PRIVATE_ONLY_REASONING");
}

TEST(ChatCompletionsApi, StreamsReasoningBeforeAnswerWithAutomaticPrecedence) {
    Output output;
    ChatCompletionsStreamDecoder decoder(ReasoningFormat::automatic, output.sink());

    decoder.consume(
        "data: {\"choices\":[{\"delta\":{"
        "\"reasoning_content\":\"Primary\","
        "\"reasoning\":\"Ignored\","
        "\"content\":\"Answer\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"reasoning\":\" late\"}}]}\n\n"
        "data: [DONE]\n\n");
    const StreamDecodeResult result = decoder.finish();

    EXPECT_EQ(result.result.outcome, GenerationOutcome::completed);
    ASSERT_EQ(output.deltas().size(), 3U);
    EXPECT_EQ(output.deltas()[0].kind, GenerationDeltaKind::reasoning);
    EXPECT_EQ(output.deltas()[0].text, "Primary");
    EXPECT_EQ(output.deltas()[1].kind, GenerationDeltaKind::answer);
    EXPECT_EQ(output.deltas()[1].text, "Answer");
    EXPECT_EQ(output.deltas()[2].kind, GenerationDeltaKind::reasoning);
    EXPECT_EQ(output.deltas()[2].text, " late");
}

TEST(ChatCompletionsApi, UsesReasoningTextAsTheLastAutomaticFallback) {
    Output output;
    const GenerationResult result = decode_chat_completions_response(
        R"({"choices":[{"message":{"reasoning_content":"","reasoning":"","reasoning_text":"Fallback","content":"Answer"}}]})",
        ReasoningFormat::automatic,
        output.sink());

    EXPECT_EQ(result.outcome, GenerationOutcome::completed);
    EXPECT_EQ(output.reasoning(), "Fallback");
    EXPECT_EQ(output.answer(), "Answer");
}

TEST(ChatCompletionsApi, ReportsAnExplicitReasoningFieldOfTheWrongType) {
    Output output;
    ChatCompletionsStreamDecoder decoder(
        ReasoningFormat::reasoning_content, output.sink());

    decoder.consume(
        "data: {\"choices\":[{\"delta\":{"
        "\"reasoning_content\":{\"bad\":true},"
        "\"content\":\"Answer\"}}]}\n\n"
        "data: [DONE]\n\n");
    const StreamDecodeResult result = decoder.finish();

    EXPECT_EQ(result.result.outcome, GenerationOutcome::protocol_error);
    EXPECT_NE(
        result.result.message.find(
            "Reasoning field 'reasoning_content' was not a string or null"),
        std::string::npos);
    EXPECT_EQ(result.result.message.find("{\"bad\""), std::string::npos);
    EXPECT_TRUE(result.describe_response);
    EXPECT_EQ(output.answer(), "Answer");
}

TEST(ChatCompletionsApi, AcceptsNullAndEmptyReasoningUnderAnExplicitFormat) {
    Output output;
    ChatCompletionsStreamDecoder decoder(ReasoningFormat::reasoning, output.sink());

    decoder.consume(
        "data: {\"choices\":[{\"delta\":{\"reasoning\":null}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{"
        "\"reasoning\":\"\",\"reasoning_content\":\"Unselected\","
        "\"content\":\"Answer\"}}]}\n\n"
        "data: [DONE]\n\n");
    const StreamDecodeResult result = decoder.finish();

    EXPECT_EQ(result.result.outcome, GenerationOutcome::completed);
    EXPECT_EQ(output.reasoning(), "");
    EXPECT_EQ(output.answer(), "Answer");
}

TEST(ChatCompletionsApi, DropsReasoningWhenTheFormatIsNone) {
    Output output;
    ChatCompletionsStreamDecoder decoder(ReasoningFormat::none, output.sink());

    decoder.consume(
        "data: {\"choices\":[{\"delta\":{"
        "\"reasoning_content\":\"Hidden\",\"reasoning\":\"Hidden\","
        "\"content\":\"Answer\"}}]}\n\n"
        "data: [DONE]\n\n");
    const StreamDecodeResult result = decoder.finish();

    EXPECT_EQ(result.result.outcome, GenerationOutcome::completed);
    ASSERT_EQ(output.deltas().size(), 1U);
    EXPECT_EQ(output.deltas().front().kind, GenerationDeltaKind::answer);
}

TEST(ChatCompletionsApi, DecodesANonStreamingResponse) {
    Output output;

    const GenerationResult result = decode_chat_completions_response(
        R"({"choices":[{"message":{"reasoning":"Think","content":"Answer"}}]})",
        ReasoningFormat::reasoning,
        output.sink());

    EXPECT_EQ(result.outcome, GenerationOutcome::completed);
    ASSERT_EQ(output.deltas().size(), 2U);
    EXPECT_EQ(output.deltas()[0].kind, GenerationDeltaKind::reasoning);
    EXPECT_EQ(output.deltas()[0].text, "Think");
    EXPECT_EQ(output.deltas()[1].kind, GenerationDeltaKind::answer);
    EXPECT_EQ(output.deltas()[1].text, "Answer");
}

TEST(ChatCompletionsApi, ReportsInvalidNonStreamingBodies) {
    struct Case {
        const char* body;
        ReasoningFormat format;
        const char* error;
        const char* answer;
        const char* reasoning;
    };
    const Case cases[]{
        {"not-json", ReasoningFormat::automatic, "invalid JSON", "", ""},
        {R"({"choices":[]})", ReasoningFormat::automatic,
         "did not contain choices[0].message", "", ""},
        {R"({"choices":[{"message":{"reasoning":"Only","content":""}}]})",
         ReasoningFormat::reasoning, "Response completed without answer content", "", "Only"},
        {R"({"choices":[{"message":{"reasoning":7,"content":"Answer"}}]})",
         ReasoningFormat::reasoning, "Reasoning field 'reasoning' was not a string or null", "Answer", ""},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.error);
        Output output;
        const GenerationResult result = decode_chat_completions_response(
            item.body, item.format, output.sink());
        EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
        EXPECT_NE(result.message.find(item.error), std::string::npos);
        EXPECT_EQ(output.answer(), item.answer);
        EXPECT_EQ(output.reasoning(), item.reasoning);
        if (std::string_view(item.body) == "not-json") {
            EXPECT_TRUE(output.deltas().empty());
        }
    }
}

} // namespace
} // namespace cha
