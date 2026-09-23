#include "media/xai_transcript.h"
#include "media/xai_socket.h"
#include "media/xai_voice_session.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <string>
#include <vector>

namespace cha::media {
namespace {

using namespace std::chrono_literals;

nlohmann::json load_json(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Missing " + path);
    nlohmann::json value;
    input >> value;
    return value;
}

std::vector<nlohmann::json> load_events(const std::string& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Missing " + path);
    std::vector<nlohmann::json> events;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        events.push_back(nlohmann::json::parse(line).at("event"));
    }
    return events;
}

TEST(XaiTranscript, ReplaysEveryFixture) {
    const std::string directory = CHA_XAI_FIXTURE_DIRECTORY;
    const auto expected = load_json(directory + "/expected.json");
    for (const auto& [name, want] : expected.items()) {
        SCOPED_TRACE(name);
        XaiTranscriptNormalizer normalizer;
        std::string text;
        const auto events = load_events(directory + "/" + name + ".jsonl");
        ASSERT_EQ(events.size(), want.at("event_deltas").size());
        for (std::size_t index = 0; index < events.size(); ++index) {
            const auto update = normalizer.apply(events[index]);
            EXPECT_EQ(update.addition, want.at("event_deltas")[index].get<std::string>());
            text += update.addition;
        }
        EXPECT_EQ(text, want.at("text").get<std::string>());
        EXPECT_NEAR(
            normalizer.last_committed_end(),
            want.at("last_committed_end").get<double>(),
            1e-9);
    }
}

TEST(XaiTranscript, IgnoresInterimAndKeepsEqualEndTimes) {
    XaiTranscriptNormalizer normalizer;
    const auto interim = nlohmann::json::parse(
        R"({"type":"transcript.partial","is_final":false,"speech_final":false,"text":"maybe","words":[]})");
    EXPECT_EQ(normalizer.apply(interim).addition, "");
    EXPECT_DOUBLE_EQ(normalizer.last_committed_end(), -1);

    const auto same_end = nlohmann::json::parse(
        R"({"type":"transcript.partial","is_final":true,"speech_final":false,"text":"Hi,","words":[{"text":"Hi","start":0.1,"end":0.2},{"text":",","start":0.2,"end":0.2}]})");
    EXPECT_EQ(normalizer.apply(same_end).addition, "Hi,");
    const auto repeated = nlohmann::json::parse(
        R"({"type":"transcript.partial","is_final":true,"speech_final":true,"text":"Hi,","words":[{"text":"Hi","start":0.1,"end":0.2},{"text":",","start":0.2,"end":0.2}]})");
    EXPECT_EQ(normalizer.apply(repeated).addition, "");
    const auto again = nlohmann::json::parse(
        R"({"type":"transcript.partial","is_final":true,"speech_final":false,"text":"Hi","words":[{"text":"Hi","start":1.0,"end":1.2}]})");
    EXPECT_EQ(normalizer.apply(again).addition, " Hi");
    EXPECT_NEAR(normalizer.last_committed_end(), 1.2, 1e-9);
}

TEST(XaiTranscript, RejectsMissingAndInvalidTimings) {
    XaiTranscriptNormalizer normalizer;
    const auto missing = nlohmann::json::parse(
        R"({"type":"transcript.partial","is_final":true,"speech_final":true,"text":"Hello"})");
    EXPECT_THROW(
        (void)normalizer.apply(missing),
        XaiTranscriptError);
    try {
        (void)normalizer.apply(missing);
    } catch (const XaiTranscriptError& error) {
        EXPECT_EQ(error.what(), std::string(xai_missing_word_timings));
    }
    const auto blank = nlohmann::json::parse(
        R"({"type":"transcript.partial","is_final":true,"speech_final":false,"text":"  ","words":[]})");
    EXPECT_EQ(normalizer.apply(blank).addition, "");
    const auto bad_time = nlohmann::json::parse(
        R"({"type":"transcript.done","text":"A","words":[{"text":"A","start":0.4,"end":0.2}]})");
    EXPECT_THROW((void)normalizer.apply(bad_time), XaiTranscriptError);
    const auto backwards = nlohmann::json::parse(
        R"({"type":"transcript.done","text":"AB","words":[{"text":"A","start":0.0,"end":0.4},{"text":"B","start":0.1,"end":0.2}]})");
    EXPECT_THROW((void)normalizer.apply(backwards), XaiTranscriptError);
    const auto provider = nlohmann::json::parse(R"({"type":"error","message":"nope"})");
    try {
        (void)normalizer.apply(provider);
        FAIL() << "provider error must fail";
    } catch (const XaiTranscriptError& error) {
        EXPECT_EQ(error.what(), std::string(xai_provider_error));
    }
    EXPECT_EQ(normalizer.apply(nlohmann::json{{"type", "unknown"}}).addition, "");
    const auto done = nlohmann::json::parse(R"({"type":"transcript.done","text":"","words":[]})");
    const auto update = normalizer.apply(done);
    EXPECT_TRUE(update.done);
    EXPECT_EQ(update.addition, "");
}

// Constructed events. They are not rows from the recorded fixtures.
TEST(XaiTranscript, SyntheticEdges) {
    XaiTranscriptNormalizer equal_ends;
    const auto same_end = nlohmann::json::parse(
        R"({"type":"transcript.partial","is_final":true,"speech_final":false,"text":"Hi,","words":[{"text":"Hi","start":0.1,"end":0.2},{"text":",","start":0.2,"end":0.2}]})");
    EXPECT_EQ(equal_ends.apply(same_end).addition, "Hi,");
    EXPECT_EQ(equal_ends.apply(same_end).addition, "");
    EXPECT_NEAR(equal_ends.last_committed_end(), 0.2, 1e-9);

    XaiTranscriptNormalizer missing_times;
    const auto untimed = nlohmann::json::parse(
        R"({"type":"transcript.partial","is_final":true,"speech_final":true,"text":"Hello"})");
    try {
        (void)missing_times.apply(untimed);
        FAIL() << "a nonempty final without words must fail";
    } catch (const XaiTranscriptError& error) {
        EXPECT_EQ(error.what(), std::string(xai_missing_word_timings));
    }
    EXPECT_DOUBLE_EQ(missing_times.last_committed_end(), -1);
    const auto untimed_done = nlohmann::json::parse(
        R"({"type":"transcript.done","text":"Hello","words":[]})");
    EXPECT_THROW((void)missing_times.apply(untimed_done), XaiTranscriptError);
    const auto bad_time = nlohmann::json::parse(
        R"({"type":"transcript.partial","is_final":true,"speech_final":true,"text":"A","words":[{"text":"A","start":0.4,"end":0.2}]})");
    EXPECT_THROW((void)missing_times.apply(bad_time), XaiTranscriptError);
    EXPECT_DOUBLE_EQ(missing_times.last_committed_end(), -1);

    XaiTranscriptNormalizer done_words;
    const auto earlier = nlohmann::json::parse(
        R"({"type":"transcript.partial","is_final":true,"speech_final":false,"text":"Hi","words":[{"text":"Hi","start":0.0,"end":0.2}]})");
    EXPECT_EQ(done_words.apply(earlier).addition, "Hi");
    const auto done = nlohmann::json::parse(
        R"({"type":"transcript.done","text":"Hi there","words":[{"text":"Hi","start":0.0,"end":0.2},{"text":"there","start":0.2,"end":0.4}]})");
    const auto update = done_words.apply(done);
    EXPECT_TRUE(update.done);
    EXPECT_EQ(update.addition, " there");
    EXPECT_NEAR(done_words.last_committed_end(), 0.4, 1e-9);

    XaiTranscriptNormalizer done_only;
    const auto first_done = done_only.apply(nlohmann::json::parse(
        R"({"type":"transcript.done","text":"Hi there","words":[{"text":"Hi","start":0.0,"end":0.2},{"text":"there","start":0.2,"end":0.4}]})"));
    EXPECT_TRUE(first_done.done);
    EXPECT_EQ(first_done.addition, "Hi there");
}

TEST(XaiRequest, BuildsTheFixedQueryAndOmitsAnEmptyLanguage) {
    const std::string base = "ws://127.0.0.1:9/v1/stt";
    const std::string english = build_xai_stt_url(
        base, "grok-voice-transcribe-2.0", {"en"});
    EXPECT_EQ(
        english,
        base + "?model=grok-voice-transcribe-2.0&encoding=pcm&sample_rate=16000"
            "&channels=1&interim_results=false&endpointing=400&language=en");
    EXPECT_EQ(
        build_xai_stt_url(base, "grok-voice-transcribe-2.0", {"ru"}),
        base + "?model=grok-voice-transcribe-2.0&encoding=pcm&sample_rate=16000"
            "&channels=1&interim_results=false&endpointing=400&language=ru");
    EXPECT_EQ(
        build_xai_stt_url(base, "grok-voice-transcribe-2.0", {}),
        base + "?model=grok-voice-transcribe-2.0&encoding=pcm&sample_rate=16000"
            "&channels=1&interim_results=false&endpointing=400");
    EXPECT_EQ(
        build_xai_stt_url(base + "?dropped=1", "a b", {"", "ru"}),
        base + "?model=a%20b&encoding=pcm&sample_rate=16000&channels=1"
            "&interim_results=false&endpointing=400&language=ru");
    EXPECT_EQ(english.find("secret"), std::string::npos);
}

TEST(XaiRequest, DecodesPcmAndRejectsBadPayloads) {
    const auto pcm = decode_pcm_base64("AAE=");
    ASSERT_EQ(pcm.size(), 2U);
    EXPECT_EQ(pcm[0], 0);
    EXPECT_EQ(pcm[1], 1);
    EXPECT_THROW((void)decode_pcm_base64(""), std::invalid_argument);
    EXPECT_THROW((void)decode_pcm_base64("@@@"), std::invalid_argument);
    EXPECT_THROW((void)decode_pcm_base64("AA=="), std::invalid_argument);
    EXPECT_THROW((void)decode_pcm_base64("AA==AAAA"), std::invalid_argument);
    std::string huge(65540, 'A');
    EXPECT_THROW((void)decode_pcm_base64(huge), std::invalid_argument);
}

TEST(XaiRequest, ReportsCurlWebSocketSchemes) {
    EXPECT_TRUE(curl_supports_websocket_scheme("ws"));
#ifdef __APPLE__
    EXPECT_TRUE(curl_supports_websocket_scheme("wss"));
#endif
    EXPECT_FALSE(curl_supports_websocket_scheme("not-a-scheme"));
}

TEST(XaiRequest, ScalesDeadlinesFromTheBridgeBudget) {
    const auto normal = xai_deadline_budget(30000ms);
    EXPECT_EQ(normal.startup, 15000ms);
    EXPECT_EQ(normal.stop_budget, 20000ms);
    EXPECT_EQ(normal.audio_send, 2000ms);
    const auto shortened = xai_deadline_budget(900ms);
    EXPECT_EQ(shortened.startup, 600ms);
    EXPECT_EQ(shortened.stop_budget, 600ms);
    EXPECT_EQ(shortened.audio_send, 600ms);
    EXPECT_EQ(xai_stop_limit(20000ms, 9000ms), 9000ms);
    EXPECT_EQ(xai_stop_limit(100ms, 30000ms), 100ms);
}

} // namespace
} // namespace cha::media
