#include "media/note_audio.h"
#include "support/mock_http_server.h"
#include "support/test_workspace.h"
#include "util/base64.h"
#include <gtest/gtest.h>
#include <fstream>
#ifdef _WIN32
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#endif
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
            const NoteAudioCredentials credentials = [&] { return service; };
            auto batch = submit_note_audio(credentials, jsonl, cancel);
            const auto id = note_audio_batch_id(batch);
            while (!batch.value("done", false)) {
                wait_note_audio(10, cancel, [&](int seconds) { waits.push_back(seconds); if (cancel_on_wait) cancel.store(true); });
                batch = read_note_audio_batch(credentials, id, cancel);
            }
            download_note_audio(credentials, batch, count, workspace.root(), cancel);
            clips = process_note_audio(count, workspace.root(), cancel);
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
    bool cancel_on_wait{};
    std::vector<int> waits;
    std::vector<std::string> requests, upload_requests;
};

#if defined(__APPLE__) || defined(_WIN32)
TEST(NoteAudioCodec, DistinguishesKnownFileFailuresFromUnknownAndFormatErrorsAtEveryStep) {
#ifdef __APPLE__
    const std::vector<long> file_errors{-34, -36, -43, -47, -54, -120};
    const long format_error = kAudioFileUnsupportedDataFormatError;
#else
    const std::vector<long> file_errors{HRESULT_FROM_WIN32(ERROR_DISK_FULL), HRESULT_FROM_WIN32(ERROR_WRITE_FAULT),
        HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND), STG_E_MEDIUMFULL, STG_E_WRITEFAULT, STG_E_READFAULT};
    const long format_error = MF_E_INVALIDMEDIATYPE;
#endif
    for (const auto* operation : {"output creation", "output write", "output finalization"}) {
        for (const auto code : file_errors) {
            const auto error = note_audio_codec_error(operation, code);
            EXPECT_EQ(error.kind, NoteAudioFailure::storage);
            EXPECT_EQ(error.code, code);
            EXPECT_NE(std::string(error.what()).find(operation), std::string::npos);
        }
        EXPECT_EQ(note_audio_codec_error(operation, format_error).kind, NoteAudioFailure::invalid);
        EXPECT_EQ(note_audio_codec_error(operation, -987654).kind, NoteAudioFailure::invalid);
    }
    test::TestWorkspace workspace;
    try {
        encode_note_audio(workspace.root() / "missing.wav", workspace.root() / "output.m4a");
        FAIL() << "Missing input file must fail";
    } catch (const NoteAudioError& error) {
        EXPECT_EQ(error.kind, NoteAudioFailure::storage);
        EXPECT_NE(error.code, 0);
    }
}
#endif

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
    EXPECT_THROW(prepare_note_audio(service, std::vector<std::string>(1001, "x")), NoteAudioSizeError);
    std::string unicode;
    for (int i = 0; i < 5000; ++i) unicode += "😀";
    EXPECT_NO_THROW(prepare_note_audio(service, {unicode}));
    EXPECT_THROW(prepare_note_audio(service, {unicode + "x"}), NoteAudioSizeError);
    EXPECT_THROW(prepare_note_audio(service, std::vector<std::string>(1000, std::string(5000, '\1'))), NoteAudioSizeError);
    // Escaped control characters increase JSONL bytes without increasing the
    // code-point count. Fill precisely to the byte boundary, then exceed it.
    std::vector<std::string> boundary(1000, std::string(4999, 'x'));
    auto remaining = 8388608 - prepare_note_audio(service, boundary).size();
    std::size_t index{};
    while (remaining >= 5) { boundary[index / 4999][index % 4999] = '\1'; ++index; remaining -= 5; }
    while (remaining) { boundary[index / 4999][index % 4999] = '\t'; ++index; --remaining; }
    EXPECT_EQ(prepare_note_audio(service, boundary).size(), 8388608u);
    boundary[index / 4999][index % 4999] = '\t';
    EXPECT_THROW(prepare_note_audio(service, boundary), NoteAudioSizeError);
    service.voice = "obsolete";
    EXPECT_THROW(prepare_note_audio(service, {"x"}), std::runtime_error);
    service.voice = "Kore"; service.url = "https://example.com/v1beta";
    EXPECT_THROW(prepare_note_audio(service, {"x"}), std::runtime_error);
}

TEST(NoteAudioInput, SettingsErrorsPrecedeSizeErrors) {
    for (const auto& service : std::vector<NoteAudioService>{
        {"https://example.com/path", "secret", "Kore"},
        {"https://example.com", "", "Kore"},
        {"https://example.com", "secret", "obsolete"}}) {
        auto settings = service;
        try {
            (void)prepare_note_audio(settings, std::vector<std::string>(1001, "x"));
            FAIL() << "Invalid service settings must fail";
        } catch (const NoteAudioSizeError&) { FAIL() << "Settings must be checked first"; }
        catch (const std::runtime_error&) {}
    }
}

TEST(NoteAudioService, PreCancelledAttemptMakesNoRequests) {
    test::TestWorkspace workspace;
    MockHttpServer server({});
    NoteAudioService service{"http://127.0.0.1:" + std::to_string(server.port()), "secret", "Kore"};
    std::atomic_bool cancel{true};
    // The server never starts, so only the cancellation check can end the
    // first request; a transfer failure would report a different message.
    try {
        (void)submit_note_audio([&] { return service; }, prepare_note_audio(service, {"Hello"}), cancel);
        FAIL();
    } catch (const std::runtime_error& error) { EXPECT_STREQ(error.what(), "Audio generation was interrupted."); }
    EXPECT_FALSE(std::filesystem::exists(workspace.root() / "results.jsonl"));
    EXPECT_FALSE(std::filesystem::exists(workspace.root() / "0.m4a"));
}

#if defined(__APPLE__) || defined(_WIN32)
TEST_F(NoteAudioServiceTest, CancellationInterruptsRemoteWaitBeforeAnotherRead) {
    cancel_on_wait = true;
    EXPECT_THROW(run({json_response(operation())}), std::runtime_error);
    EXPECT_EQ(waits, (std::vector<int>{10}));
    EXPECT_EQ(requests.size(), 2u);
    EXPECT_FALSE(std::filesystem::exists(workspace.root() / "results.jsonl"));
    EXPECT_FALSE(std::filesystem::exists(workspace.root() / "0.m4a"));
}

TEST_F(NoteAudioServiceTest, PostsAreNotRetried) {
    EXPECT_THROW(run({failure_response(500)}), std::runtime_error);
    EXPECT_TRUE(waits.empty());
    EXPECT_EQ(requests.size(), 2u);
    EXPECT_THROW(run({}, 1, failure_response(500)), std::runtime_error);
    EXPECT_TRUE(waits.empty());
    EXPECT_EQ(requests.size(), 1u);
}
TEST_F(NoteAudioServiceTest, UnusableCreationRepliesKeepTheirCauseAndWarnWithoutAnEventsReference) {
    for (const auto& response : std::vector<std::string>{
        json_response({{"name", "batches/invalid"}}), http_response("application/json", "not JSON"),
        http_response("application/json", std::string(65537, 'x')), failure_response(500)}) {
        try {
            (void)run({response});
            FAIL() << "Invalid creation response must fail";
        } catch (const NoteAudioError& error) {
            EXPECT_EQ(error.kind, NoteAudioFailure::uncertain);
            EXPECT_NE(std::string(error.what()).find("Starting again may generate audio twice"), std::string::npos);
            EXPECT_EQ(std::string(error.what()).find("Check Events"), std::string::npos);
        }
        EXPECT_TRUE(waits.empty());
        EXPECT_EQ(requests.size(), 2u);
    }
}
#if defined(__APPLE__) || defined(_WIN32)
TEST_F(NoteAudioServiceTest, CodecFailuresKeepTheBlockNumberAndFailureKind) {
    auto bytes = wav(); bytes.pop_back();
    try {
        (void)run({json_response(operation(true)), http_response("application/jsonl", item("0", bytes).dump() + '\n')});
        FAIL() << "Truncated WAV must fail";
    } catch (const NoteAudioError& error) {
        EXPECT_EQ(error.kind, NoteAudioFailure::invalid);
        EXPECT_NE(std::string(error.what()).find("text block 1"), std::string::npos);
        EXPECT_NE(std::string(error.what()).find("truncated or invalid WAV"), std::string::npos);
    }
}
#endif
TEST_F(NoteAudioServiceTest, RestartsPartialDownloadsAndAcceptsOutOfOrderResults) {
    const auto results = item("1").dump() + '\n' + item("0").dump() + '\n';
    const auto partial = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(results.size())
        + "\r\nConnection: close\r\n\r\n" + results.substr(0, 31);
    MockHttpServer server({partial, http_response("application/jsonl", results)});
    NoteAudioService service{"http://127.0.0.1:" + std::to_string(server.port()), "key", "Kore"};
    const NoteAudioCredentials credentials = [&] { return service; };
    server.start();
    EXPECT_THROW(download_note_audio(credentials, operation(true), 2, workspace.root(), cancel), NoteAudioError);
    download_note_audio(credentials, operation(true), 2, workspace.root(), cancel);
    const auto clips = process_note_audio(2, workspace.root(), cancel);
    server.join();
    ASSERT_EQ(clips.size(), 2u);
    EXPECT_EQ(clips[0].filename(), "0.m4a");
    EXPECT_EQ(clips[1].filename(), "1.m4a");
    EXPECT_EQ(server.requests().size(), 2u);

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
#endif
#ifdef __APPLE__
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
#ifdef _WIN32
TEST(NoteAudioCodec, WindowsResamplesAndDecodesShortNormalAndLongClips) {
    using Microsoft::WRL::ComPtr;
    ASSERT_TRUE(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)));
    struct Runtime {
        ~Runtime() { MFShutdown(); CoUninitialize(); }
    } runtime;
    ASSERT_TRUE(SUCCEEDED(MFStartup(MF_VERSION)));
    test::TestWorkspace workspace;
    const auto source = workspace.root() / L"narration-\u97f3.wav";
    const auto output = workspace.root() / L"narration-\u97f3.m4a";
    for (const std::size_t frames : {2400u, 48000u, 1440000u}) {
        SCOPED_TRACE(frames);
        const auto bytes = wav(frames);
        { std::ofstream file(source, std::ios::binary); file.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); }
        ASSERT_NO_THROW(encode_note_audio(source, output));
        ComPtr<IMFSourceReader> reader;
        ASSERT_TRUE(SUCCEEDED(MFCreateSourceReaderFromURL(output.c_str(), nullptr, &reader)));
        ComPtr<IMFMediaType> type;
        ASSERT_TRUE(SUCCEEDED(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &type)));
        GUID subtype{}; UINT32 rate{}, channels{};
        ASSERT_TRUE(SUCCEEDED(type->GetGUID(MF_MT_SUBTYPE, &subtype)));
        EXPECT_EQ(subtype, MFAudioFormat_AAC);
        ASSERT_TRUE(SUCCEEDED(type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate)));
        ASSERT_TRUE(SUCCEEDED(type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels)));
        EXPECT_EQ(rate, 48000u); EXPECT_EQ(channels, 1u);
        ASSERT_TRUE(SUCCEEDED(MFCreateMediaType(&type)));
        ASSERT_TRUE(SUCCEEDED(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)));
        ASSERT_TRUE(SUCCEEDED(type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM)));
        ASSERT_TRUE(SUCCEEDED(type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16)));
        ASSERT_TRUE(SUCCEEDED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, type.Get())));
        std::vector<std::int16_t> decoded;
        for (;;) {
            DWORD flags{};
            ComPtr<IMFSample> sample;
            ASSERT_TRUE(SUCCEEDED(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags, nullptr, &sample)));
            if (sample) {
                ComPtr<IMFMediaBuffer> buffer;
                ASSERT_TRUE(SUCCEEDED(sample->ConvertToContiguousBuffer(&buffer)));
                BYTE* data{}; DWORD length{};
                ASSERT_TRUE(SUCCEEDED(buffer->Lock(&data, nullptr, &length)));
                const auto* values = reinterpret_cast<const std::int16_t*>(data);
                decoded.insert(decoded.end(), values, values + length / 2);
                ASSERT_TRUE(SUCCEEDED(buffer->Unlock()));
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        }
        // AAC adds frame padding. It must not shorten speech or change its pace.
        EXPECT_GE(decoded.size(), frames * 2);
        EXPECT_LE(decoded.size(), frames * 2 + 2048);
        ASSERT_GT(decoded.size(), 2048u);
        // The input square wave is 240 Hz. Check its middle to exclude padding.
        const auto first = decoded.size() / 4, last = decoded.size() * 3 / 4;
        std::size_t crossings{};
        for (auto i = first + 1; i < last; ++i)
            if (decoded[i - 1] < 0 && decoded[i] >= 0) ++crossings;
        EXPECT_NEAR(static_cast<double>(crossings) * 48000 / static_cast<double>(last - first), 240.0, 25.0);
    }
    for (const auto& bytes : {wav().substr(0, 100), wav(0), std::string("invalid")}) {
        std::ofstream(source, std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        EXPECT_THROW(encode_note_audio(source, output), std::runtime_error);
    }
    auto wrong_rate = wav(); wrong_rate[24] = 0x44; wrong_rate[25] = static_cast<char>(0xac);
    std::ofstream(source, std::ios::binary).write(wrong_rate.data(), static_cast<std::streamsize>(wrong_rate.size()));
    EXPECT_THROW(encode_note_audio(source, output), std::runtime_error);
}
#endif
}
}
