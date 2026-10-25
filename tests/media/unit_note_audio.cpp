#include "media/note_audio.h"
#include "support/mock_http_server.h"
#include "support/test_workspace.h"
#include "util/base64.h"
#include <gtest/gtest.h>
#include <fstream>
#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#endif

namespace cha {
namespace {
using Json = nlohmann::json;
const std::string file_name = "files/" + std::string(32, 'a');
const std::string batch_name = "batches/" + std::string(32, 'b');
std::string json_response(const Json& value) { return http_response("application/json", value.dump()); }
std::string failure_response(int code) { return "HTTP/1.1 " + std::to_string(code) + " Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; }
Json operation(bool done = false, std::string state = "BATCH_STATE_RUNNING", Json stats = Json::object()) {
    Json result{{"name", batch_name}, {"done", done}, {"metadata", {{"state", state}, {"batchStats", stats}}}};
    if (done) result["response"] = {{"responsesFile", file_name}};
    return result;
}
std::string wav(std::size_t frames = 2400) {
    std::string value;
    const auto number = [&](std::uint32_t n, int bytes) { for (int i = 0; i < bytes; ++i) value += static_cast<char>((n >> (8 * i)) & 255); };
    value += "RIFF"; number(static_cast<std::uint32_t>(36 + frames * 2), 4); value += "WAVEfmt ";
    number(16, 4); number(1, 2); number(1, 2); number(24000, 4); number(48000, 4); number(2, 2); number(16, 2);
    value += "data"; number(static_cast<std::uint32_t>(frames * 2), 4);
    for (std::size_t i = 0; i < frames; ++i) number(i % 100 < 50 ? 1000 : 64536, 2);
    return value;
}
Json item(std::string key, std::string bytes = wav()) {
    return {{"key", key}, {"response", {{"candidates", Json::array({{{"content", {{"parts", Json::array({
        {{"inlineData", {{"mimeType", "audio/wav"}, {"data", encode_base64(bytes)}}}}
    })}}}}})}}}};
}
class NoteAudioServiceTest : public testing::Test {
protected:
    std::vector<std::filesystem::path> run(std::vector<std::string> responses, std::size_t count = 1,
        std::string finalization = json_response({{"file", {{"name", file_name}}}})) {
        MockHttpServer upload({std::move(finalization)});
        responses.insert(responses.begin(), "HTTP/1.1 200 OK\r\nx-goog-upload-url: http://127.0.0.1:"
            + std::to_string(upload.port()) + "/secret\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        MockHttpServer server(std::move(responses));
        NoteAudioService service{"http://127.0.0.1:" + std::to_string(server.port()), "private-key", "grant"};
        const auto jsonl = prepare_note_audio(service, std::vector<std::string>(count, "Paragraph"));
        upload.start(); server.start();
        std::vector<std::filesystem::path> clips;
        std::exception_ptr error;
        try {
            clips = generate_note_audio(service, jsonl, count, workspace.root(), cancel,
                [&](auto phase, auto completed, auto total) {
                    progress.push_back({{"phase", phase}});
                    if (completed) progress.back()["completed"] = *completed;
                    if (total) progress.back()["total"] = *total;
                }, [&](int seconds) { waits.push_back(seconds); });
        } catch (...) { error = std::current_exception(); }
        upload.join(); server.join();
        requests = server.requests(); upload_requests = upload.requests();
        EXPECT_EQ(upload_requests.size(), 1u);
        EXPECT_EQ(upload_requests[0].find("private-key"), std::string::npos);
        EXPECT_EQ(upload_requests[0].find("x-goog-api-key"), std::string::npos);
        EXPECT_NE(requests[0].find("x-goog-api-key: private-key"), std::string::npos);
        if (error) std::rethrow_exception(error);
        return clips;
    }
    test::TestWorkspace workspace;
    std::atomic_bool cancel{};
    std::vector<int> waits;
    std::vector<Json> progress;
    std::vector<std::string> requests, upload_requests;
};

TEST(NoteAudioInput, ValidatesLimitsAndSerializesExactVoiceAndUnicodeText) {
    NoteAudioService service{"http://127.0.0.1:18080/", "secret", "gerdt"};
    const auto text = std::string(" \n\"Visible link\" ") + "😀";
    const auto data = prepare_note_audio(service, {text, text});
    EXPECT_EQ(service.url, "http://127.0.0.1:18080");
    const auto first = Json::parse(data.substr(0, data.find('\n')));
    EXPECT_EQ(first["key"], "0");
    EXPECT_EQ(first["request"]["contents"][0]["parts"][0]["text"], text);
    EXPECT_EQ(first["request"]["generationConfig"]["speechConfig"]["voiceConfig"]["voice"], "gerdt");
    EXPECT_EQ(std::count(data.begin(), data.end(), '\n'), 2);
    EXPECT_THROW(prepare_note_audio(service, {}), std::runtime_error);
    EXPECT_THROW(prepare_note_audio(service, {""}), std::runtime_error);
    EXPECT_THROW(prepare_note_audio(service, std::vector<std::string>(1001, "x")), std::runtime_error);
    std::string unicode;
    for (int i = 0; i < 5000; ++i) unicode += "😀";
    EXPECT_NO_THROW(prepare_note_audio(service, {unicode}));
    EXPECT_THROW(prepare_note_audio(service, {unicode + "x"}), std::runtime_error);
    EXPECT_THROW(prepare_note_audio(service, std::vector<std::string>(1000, std::string(5000, '\1'))), std::runtime_error);
    // Escaped control characters increase JSONL bytes without increasing the
    // code-point count. Fill precisely to the byte boundary, then exceed it.
    std::vector<std::string> boundary(1000, std::string(4999, 'x'));
    auto remaining = 8388608 - prepare_note_audio(service, boundary).size();
    std::size_t index{};
    while (remaining >= 5) { boundary[index / 4999][index % 4999] = '\1'; ++index; remaining -= 5; }
    while (remaining) { boundary[index / 4999][index % 4999] = '\t'; ++index; --remaining; }
    EXPECT_EQ(prepare_note_audio(service, boundary).size(), 8388608u);
    boundary[index / 4999][index % 4999] = '\t';
    EXPECT_THROW(prepare_note_audio(service, boundary), std::runtime_error);
    service.voice = "obsolete";
    EXPECT_THROW(prepare_note_audio(service, {"x"}), std::runtime_error);
    service.voice = "Kore"; service.url = "https://example.com/v1beta";
    EXPECT_THROW(prepare_note_audio(service, {"x"}), std::runtime_error);
}

#ifdef __APPLE__
TEST_F(NoteAudioServiceTest, ReadRetriesResetAfterSuccessAndUseBatchState) {
    auto stats = Json{{"successfulRequestCount", "1"}, {"failedRequestCount", "1"}, {"requestCount", "3"}, {"pendingRequestCount", "1"}};
    const auto results = (item("0").dump() + '\n');
    const auto clips = run({json_response(operation(false, "BATCH_STATE_PENDING")), failure_response(500), "",
        json_response(operation(false, "BATCH_STATE_RUNNING", stats)), failure_response(500),
        json_response(operation(true)), http_response("application/jsonl", results)});
    EXPECT_EQ(waits, (std::vector<int>{10, 10, 20, 10, 10}));
    ASSERT_EQ(progress.size(), 5u);
    EXPECT_EQ(progress[1]["phase"], "queued");
    EXPECT_FALSE(progress[1].contains("total"));
    EXPECT_EQ(progress[2], (Json{{"phase", "generating"}, {"completed", 2}, {"total", 3}}));
    EXPECT_EQ(requests.size(), 8u);
    EXPECT_EQ(clips.size(), 1u);
}

TEST_F(NoteAudioServiceTest, StopsAfterExactlyThreeReadRetries) {
    EXPECT_THROW(run({json_response(operation()), failure_response(500), failure_response(500), failure_response(500), failure_response(500)}), std::runtime_error);
    EXPECT_EQ(waits, (std::vector<int>{10, 10, 20, 40}));
    EXPECT_EQ(requests.size(), 6u);
}
TEST_F(NoteAudioServiceTest, OtherHttpErrorsAndPostsAreNotRetried) {
    EXPECT_THROW(run({json_response(operation()), failure_response(503)}), std::runtime_error);
    EXPECT_EQ(waits, (std::vector<int>{10}));
    waits.clear();
    EXPECT_THROW(run({failure_response(500)}), std::runtime_error);
    EXPECT_TRUE(waits.empty());
    EXPECT_EQ(requests.size(), 2u);
    EXPECT_THROW(run({}, 1, failure_response(500)), std::runtime_error);
    EXPECT_TRUE(waits.empty());
    EXPECT_EQ(requests.size(), 1u);
}
TEST_F(NoteAudioServiceTest, RestartsPartialDownloadsAndAcceptsOutOfOrderResults) {
    const auto results = item("1").dump() + '\n' + item("0").dump() + '\n';
    const auto partial = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(results.size())
        + "\r\nConnection: close\r\n\r\n" + results.substr(0, 31);
    auto clips = run({json_response(operation(true)), partial, http_response("application/jsonl", results)}, 2);
    EXPECT_EQ(waits, (std::vector<int>{10}));
    ASSERT_EQ(clips.size(), 2u);
    EXPECT_EQ(clips[0].filename(), "0.m4a");
    EXPECT_EQ(clips[1].filename(), "1.m4a");
    EXPECT_FALSE(std::filesystem::exists(workspace.root() / "clip.wav"));
    EXPECT_EQ(requests[2].substr(0, requests[2].find('\r')), requests[3].substr(0, requests[3].find('\r')));
}
TEST_F(NoteAudioServiceTest, MalformedCountsAreUnavailableRatherThanZero) {
    const auto stats = Json{{"successfulRequestCount", 1}, {"failedRequestCount", "bad"}, {"requestCount", "oops"}};
    (void)run({json_response(operation(false, "BATCH_STATE_RUNNING", stats)), json_response(operation(true)),
        http_response("application/jsonl", item("0").dump() + '\n')});
    EXPECT_EQ(progress[1], (Json{{"phase", "generating"}}));
}
TEST_F(NoteAudioServiceTest, RejectsIncompleteDuplicateUnknownAndFailedItems) {
    const std::vector<std::string> invalid = {
        "", item("0").dump() + '\n' + item("0").dump() + '\n', item("1").dump() + '\n', item("00").dump() + '\n',
        Json{{"key", "0"}, {"error", {{"message", "private service details"}}}}.dump() + '\n',
        "not JSON\n", item("0", "not a WAV").dump() + '\n'
    };
    for (const auto& result : invalid) {
        SCOPED_TRACE(result.substr(0, 80));
        EXPECT_THROW(run({json_response(operation(true)), http_response("application/jsonl", result)}), std::exception);
        EXPECT_TRUE(waits.empty());
    }
    auto bad_base64 = item("0");
    bad_base64["response"]["candidates"][0]["content"]["parts"][0]["inlineData"]["data"] = "????";
    EXPECT_THROW(run({json_response(operation(true)), http_response("application/jsonl", bad_base64.dump())}), std::runtime_error);
}
TEST(NoteAudioCodec, ConvertsAndFullyDecodesShortNormalAndLongClipsAndRejectsTruncation) {
    test::TestWorkspace workspace;
    const auto source = workspace.root() / "source.wav", output = workspace.root() / "encoded.m4a";
    for (const std::size_t frames : {2400u, 48000u, 1440000u}) {
        const auto bytes = wav(frames);
        { std::ofstream file(source, std::ios::binary); file.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); }
        ASSERT_NO_THROW(encode_note_audio(source, output));
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr,
            reinterpret_cast<const UInt8*>(output.c_str()), static_cast<CFIndex>(output.string().size()), false);
        ExtAudioFileRef audio{};
        ASSERT_EQ(ExtAudioFileOpenURL(url, &audio), noErr);
        CFRelease(url);
        AudioStreamBasicDescription format{}; UInt32 size = sizeof(format);
        ASSERT_EQ(ExtAudioFileGetProperty(audio, kExtAudioFileProperty_FileDataFormat, &size, &format), noErr);
        EXPECT_EQ(format.mFormatID, kAudioFormatMPEG4AAC); EXPECT_EQ(format.mSampleRate, 24000);
        SInt64 decoded_frames{}; size = sizeof(decoded_frames);
        ASSERT_EQ(ExtAudioFileGetProperty(audio, kExtAudioFileProperty_FileLengthFrames, &size, &decoded_frames), noErr);
        EXPECT_EQ(decoded_frames, static_cast<SInt64>(frames));
        AudioStreamBasicDescription pcm{};
        pcm.mSampleRate = 24000; pcm.mFormatID = kAudioFormatLinearPCM;
        pcm.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        pcm.mBytesPerPacket = pcm.mBytesPerFrame = 2; pcm.mFramesPerPacket = pcm.mChannelsPerFrame = 1; pcm.mBitsPerChannel = 16;
        ASSERT_EQ(ExtAudioFileSetProperty(audio, kExtAudioFileProperty_ClientDataFormat, sizeof(pcm), &pcm), noErr);
        std::int16_t samples[4096]; std::uint64_t total{};
        for (;;) {
            UInt32 read = 4096; AudioBufferList buffer{1, {{1, sizeof(samples), samples}}};
            ASSERT_EQ(ExtAudioFileRead(audio, &read, &buffer), noErr);
            if (!read) break;
            total += read;
        }
        EXPECT_EQ(total, frames); EXPECT_EQ(ExtAudioFileDispose(audio), noErr);
    }
    const auto truncated = wav().substr(0, 100);
    { std::ofstream file(source, std::ios::binary); file.write(truncated.data(), static_cast<std::streamsize>(truncated.size())); }
    EXPECT_THROW(encode_note_audio(source, output), std::runtime_error);
}
#endif
}
}
