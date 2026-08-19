#include "agents/provider_response.h"

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

TEST(ProviderResponse, DecodesOneEventPerChunk) {
    Output output;
    ProviderStreamDecoder decoder(ReasoningFormat::automatic, output.sink());

    decoder.consume("data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\n\n");
    decoder.consume("data: {\"choices\":[{\"delta\":{\"content\":\" world\"}}]}\n\n");
    decoder.consume("data: [DONE]\n\n");
    const StreamDecodeResult result = decoder.finish();

    EXPECT_EQ(result.result.outcome, GenerationOutcome::completed);
    EXPECT_FALSE(result.describe_response);
    EXPECT_EQ(output.answer(), "Hello world");
    EXPECT_EQ(output.deltas().size(), 2U);
}

TEST(ProviderResponse, DecodesSeveralEventsInOneChunk) {
    Output output;
    ProviderStreamDecoder decoder(ReasoningFormat::automatic, output.sink());

    decoder.consume(two_part_stream);
    const StreamDecodeResult result = decoder.finish();

    EXPECT_EQ(result.result.outcome, GenerationOutcome::completed);
    EXPECT_EQ(output.answer(), "Hello world");
}

TEST(ProviderResponse, ReadsUsageFromTheFinalStreamingChunk) {
    Output output;
    ProviderStreamDecoder decoder(ReasoningFormat::automatic, output.sink());

    decoder.consume(
        "data: {\"choices\":[{\"delta\":{\"content\":\"Answer\"}}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":12,\"completion_tokens\":5,\"prompt_tokens_details\":{\"cached_tokens\":9}}}\n\n"
        "data: [DONE]\n\n");
    const StreamDecodeResult result = decoder.finish();

    EXPECT_EQ(result.result.outcome, GenerationOutcome::completed);
    ASSERT_TRUE(result.result.usage.input_tokens);
    ASSERT_TRUE(result.result.usage.output_tokens);
    EXPECT_EQ(*result.result.usage.input_tokens, 12U);
    EXPECT_EQ(*result.result.usage.output_tokens, 5U);
    ASSERT_TRUE(result.result.usage.cache_read_tokens);
    EXPECT_EQ(*result.result.usage.cache_read_tokens, 9U);
}

TEST(ProviderResponse, UsesTheLegacyCacheCountOnlyWhenPrimaryDetailsAreAbsent) {
    Output output;
    const GenerationResult fallback = decode_provider_response(
        R"({"usage":{"prompt_tokens":12,"completion_tokens":5,"prompt_cache_hit_tokens":4},"choices":[{"message":{"content":"Answer"}}]})",
        ReasoningFormat::automatic,
        output.sink());
    ASSERT_TRUE(fallback.usage.cache_read_tokens);
    EXPECT_EQ(*fallback.usage.cache_read_tokens, 4U);

    const GenerationResult primary = decode_provider_response(
        R"({"usage":{"prompt_tokens":12,"completion_tokens":5,"prompt_cache_hit_tokens":4,"prompt_tokens_details":{"cached_tokens":9}},"choices":[{"message":{"content":"Answer"}}]})",
        ReasoningFormat::automatic,
        output.sink());
    ASSERT_TRUE(primary.usage.cache_read_tokens);
    EXPECT_EQ(*primary.usage.cache_read_tokens, 9U);
}

TEST(ProviderResponse, LeavesCacheUsageUnsetWhenProviderOmitsIt) {
    Output output;
    const GenerationResult result = decode_provider_response(
        R"({"usage":{"prompt_tokens":12,"completion_tokens":5},"choices":[{"message":{"content":"Answer"}}]})",
        ReasoningFormat::automatic,
        output.sink());

    EXPECT_EQ(result.outcome, GenerationOutcome::completed);
    ASSERT_TRUE(result.usage.input_tokens);
    ASSERT_TRUE(result.usage.output_tokens);
    EXPECT_EQ(*result.usage.input_tokens, 12U);
    EXPECT_EQ(*result.usage.output_tokens, 5U);
    EXPECT_FALSE(result.usage.cache_read_tokens);
}

TEST(ProviderResponse, DecodesTheSameStreamOneByteAtATime) {
    Output output;
    ProviderStreamDecoder decoder(ReasoningFormat::automatic, output.sink());

    for (const char character : two_part_stream) {
        decoder.consume(std::string_view(&character, 1));
    }
    const StreamDecodeResult result = decoder.finish();

    EXPECT_EQ(result.result.outcome, GenerationOutcome::completed);
    EXPECT_EQ(output.answer(), "Hello world");
    EXPECT_EQ(output.deltas().size(), 2U);
}

TEST(ProviderResponse, DecodesCarriageReturnsSplitAcrossChunks) {
    Output output;
    ProviderStreamDecoder decoder(ReasoningFormat::automatic, output.sink());

    decoder.consume("data: {\"choices\":[{\"delta\":{\"content\":\"Hello\"}}]}\r");
    decoder.consume("\n\r");
    decoder.consume("\ndata: [DONE]\r\n\r\n");
    const StreamDecodeResult result = decoder.finish();

    EXPECT_EQ(result.result.outcome, GenerationOutcome::completed);
    EXPECT_EQ(output.answer(), "Hello");
}

TEST(ProviderResponse, DecodesATrailingEventTheStreamNeverTerminated) {
    Output output;
    ProviderStreamDecoder decoder(ReasoningFormat::automatic, output.sink());

    decoder.consume("data: {\"choices\":[{\"delta\":{\"content\":\"Tail\"}}]}\n");
    decoder.consume("data: [DONE]");
    const StreamDecodeResult result = decoder.finish();

    EXPECT_EQ(result.result.outcome, GenerationOutcome::completed);
    EXPECT_EQ(output.answer(), "Tail");
}

TEST(ProviderResponse, ReportsMalformedEventJsonAfterEmittingValidEvents) {
    Output output;
    ProviderStreamDecoder decoder(ReasoningFormat::automatic, output.sink());

    decoder.consume(
        "data: not-json\n\n"
        "data: {\"choices\":[{\"delta\":{\"content\":\"Partial\"}}]}\n\n"
        "data: [DONE]\n\n");
    const StreamDecodeResult result = decoder.finish();

    EXPECT_EQ(result.result.outcome, GenerationOutcome::protocol_error);
    EXPECT_NE(result.result.message.find("malformed JSON"), std::string::npos);
    EXPECT_TRUE(result.describe_response);
    EXPECT_EQ(output.answer(), "Partial");
}

TEST(ProviderResponse, ReportsAnEventWithoutAChoicesArray) {
    Output output;
    ProviderStreamDecoder decoder(ReasoningFormat::automatic, output.sink());

    decoder.consume("data: {\"object\":\"chunk\"}\n\ndata: [DONE]\n\n");
    const StreamDecodeResult result = decoder.finish();

    EXPECT_EQ(result.result.outcome, GenerationOutcome::protocol_error);
    EXPECT_NE(
        result.result.message.find("did not contain a choices array"),
        std::string::npos);
    EXPECT_TRUE(result.describe_response);
}

TEST(ProviderResponse, ReportsAStreamThatEndedBeforeTheEndMarker) {
    Output output;
    ProviderStreamDecoder decoder(ReasoningFormat::automatic, output.sink());

    decoder.consume("data: {\"choices\":[{\"delta\":{\"content\":\"Partial\"}}]}\n\n");
    const StreamDecodeResult result = decoder.finish();

    EXPECT_EQ(result.result.outcome, GenerationOutcome::protocol_error);
    EXPECT_NE(
        result.result.message.find("ended before [DONE]"),
        std::string::npos);
    EXPECT_TRUE(result.describe_response);
    EXPECT_EQ(output.answer(), "Partial");
}

TEST(ProviderResponse, ReportsResponseBytesThatWereNotAnEventStream) {
    Output output;
    ProviderStreamDecoder decoder(ReasoningFormat::automatic, output.sink());

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

TEST(ProviderResponse, IgnoresDataAfterTheEndMarker) {
    Output output;
    ProviderStreamDecoder decoder(ReasoningFormat::automatic, output.sink());

    decoder.consume(
        "data: {\"choices\":[{\"delta\":{\"content\":\"Complete\"}}]}\n\n"
        "data: [DONE]\n"
        "data: not-json\n\n");
    decoder.consume("data: {\"choices\":[{\"delta\":{\"content\":\" ignored\"}}]}\n\n");
    const StreamDecodeResult result = decoder.finish();

    EXPECT_EQ(result.result.outcome, GenerationOutcome::completed);
    EXPECT_EQ(output.answer(), "Complete");
}

TEST(ProviderResponse, RejectsACompletedStreamWithoutAnswerContent) {
    Output output;
    ProviderStreamDecoder decoder(ReasoningFormat::automatic, output.sink());

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

TEST(ProviderResponse, StreamsReasoningBeforeAnswerWithAutomaticPrecedence) {
    Output output;
    ProviderStreamDecoder decoder(ReasoningFormat::automatic, output.sink());

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

TEST(ProviderResponse, ReportsAnExplicitReasoningFieldOfTheWrongType) {
    Output output;
    ProviderStreamDecoder decoder(
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

TEST(ProviderResponse, AcceptsNullAndEmptyReasoningUnderAnExplicitFormat) {
    Output output;
    ProviderStreamDecoder decoder(ReasoningFormat::reasoning, output.sink());

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

TEST(ProviderResponse, DropsReasoningWhenTheFormatIsNone) {
    Output output;
    ProviderStreamDecoder decoder(ReasoningFormat::none, output.sink());

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

TEST(ProviderResponse, DecodesANonStreamingResponse) {
    Output output;

    const GenerationResult result = decode_provider_response(
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

TEST(ProviderResponse, ReportsAMalformedNonStreamingBody) {
    Output output;

    const GenerationResult result = decode_provider_response(
        "not-json", ReasoningFormat::automatic, output.sink());

    EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
    EXPECT_NE(result.message.find("invalid JSON"), std::string::npos);
    EXPECT_TRUE(output.deltas().empty());
}

TEST(ProviderResponse, ReportsANonStreamingBodyWithoutAMessageObject) {
    Output output;

    const GenerationResult result = decode_provider_response(
        R"({"choices":[]})", ReasoningFormat::automatic, output.sink());

    EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
    EXPECT_NE(
        result.message.find("did not contain choices[0].message"),
        std::string::npos);
}

TEST(ProviderResponse, RejectsANonStreamingResponseWithoutAnswerContent) {
    Output output;

    const GenerationResult result = decode_provider_response(
        R"({"choices":[{"message":{"reasoning":"Only","content":""}}]})",
        ReasoningFormat::reasoning,
        output.sink());

    EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
    EXPECT_NE(
        result.message.find("Response completed without answer content"),
        std::string::npos);
    EXPECT_EQ(output.reasoning(), "Only");
}

TEST(ProviderResponse, ReportsANonStreamingReasoningFieldOfTheWrongType) {
    Output output;

    const GenerationResult result = decode_provider_response(
        R"({"choices":[{"message":{"reasoning":7,"content":"Answer"}}]})",
        ReasoningFormat::reasoning,
        output.sink());

    EXPECT_EQ(result.outcome, GenerationOutcome::protocol_error);
    EXPECT_NE(
        result.message.find(
            "Reasoning field 'reasoning' was not a string or null"),
        std::string::npos);
    EXPECT_EQ(output.answer(), "Answer");
}

} // namespace
} // namespace cha
