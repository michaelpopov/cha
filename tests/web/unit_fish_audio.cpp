#include "web/fish_audio.h"
#include "util/logging.h"
#include "providers/voice_output_config.h"
#include "providers/api_key_store.h"
#include "session/sqlite_storage.h"
#include "session/session_repository.h"
#include "workspace/workspace.h"
#include "workspace/workspace_config_store.h"
#include "support/mock_http_server.h"
#include "support/test_workspace.h"

#include <fstream>
#include <limits>
#include <gtest/gtest.h>
#include <httplib.h>
#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

namespace cha {
namespace {
using Json = nlohmann::json;
using namespace std::chrono_literals;

bool wait_until(const std::function<bool()>& ready) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (!ready() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    return ready();
}

// Upstream FishAudio listener that waits without sending headers so
// cancellation can be observed on a real socket.
class SlowFishAudioServer {
public:
    SlowFishAudioServer() {
        upstream_.Post("/v1/tts", [&](const httplib::Request& request, httplib::Response& response) {
            started++;
            const auto deadline = std::chrono::steady_clock::now() + 30s;
            while (!finish_ && !request.is_connection_closed()
                   && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(10ms);
            }
            if (request.is_connection_closed()) disconnected++;
            finished++;
            response.set_content("audio", "audio/mpeg");
        });
        const int upstream_port = upstream_.bind_to_any_port("127.0.0.1");
        if (upstream_port < 1) throw std::runtime_error("Could not bind upstream test server");
        upstream_thread_ = std::thread([&] { upstream_.listen_after_bind(); });
        upstream_.wait_until_ready();
        output_.url = "http://127.0.0.1:" + std::to_string(upstream_port) + "/v1/tts";
        output_.model = "s2.1-pro";
        output_.output_format = "mp3";
    }

    ~SlowFishAudioServer() {
        finish_ = true;
        proxy_.stop();
        upstream_.stop();
        if (upstream_thread_.joinable()) upstream_thread_.join();
    }

    void shutdown() {
        proxy_.stop();
    }

    std::optional<EntryAudio> download(const std::function<bool()>& cancelled) {
        return download_fish_audio(output_, "secret", make_fish_audio_request(output_, {
            {"text", "Hello"}, {"reference_id", "voice"},
        }), cancelled);
    }

    FishAudioTransfer synthesize(const std::function<bool()>& cancelled) {
        return proxy_.synthesize(output_, "secret", make_fish_audio_request(output_, {
            {"text", "Hello"}, {"reference_id", "voice"},
        }), cancelled);
    }

    std::atomic_int started{0};
    std::atomic_int finished{0};
    std::atomic_int disconnected{0};

private:
    FishAudioProxy proxy_;
    WorkspaceVoiceOutput output_;
    httplib::Server upstream_;
    std::thread upstream_thread_;
    std::atomic_bool finish_{false};
};

TEST(FishAudio, ShutdownCancelsStalledUpstreamTransfer) {
    SlowFishAudioServer server;
    auto pending = std::async(std::launch::async, [&] {
        return server.synthesize([] { return false; });
    });
    ASSERT_TRUE(wait_until([&] { return server.started == 1; }));
    server.shutdown();
    EXPECT_TRUE(wait_until([&] { return server.disconnected == 1; }));
    EXPECT_EQ(pending.wait_for(10s), std::future_status::ready);
    EXPECT_TRUE(pending.get().cancelled);
}

TEST(FishAudio, BackgroundCancellationStopsStalledTransferWithoutIncomingHttpRequest) {
    SlowFishAudioServer server;
    std::atomic_bool cancelled{};
    auto transfer = std::async(std::launch::async, [&] { return server.download([&] { return cancelled.load(); }); });
    struct Cancel { std::atomic_bool& flag; ~Cancel() { flag = true; } } cleanup{cancelled};
    ASSERT_TRUE(wait_until([&] { return server.started == 1; }));
    cancelled = true;
    ASSERT_EQ(transfer.wait_for(2s), std::future_status::ready);
    EXPECT_FALSE(transfer.get());
    EXPECT_TRUE(wait_until([&] { return server.disconnected == 1; }));
}

TEST(FishAudio, FullSynthesisCapacityRejectsOverflow) {
    SlowFishAudioServer server;
    std::vector<std::future<FishAudioTransfer>> in_flight;
    for (std::size_t i = 0; i < fish_audio_concurrency; ++i) {
        in_flight.push_back(std::async(std::launch::async, [&] {
            return server.synthesize([] { return false; });
        }));
    }
    ASSERT_TRUE(wait_until([&] {
        return server.started == static_cast<int>(fish_audio_concurrency);
    }));
    const auto overflow = server.synthesize([] { return false; });
    EXPECT_TRUE(overflow.busy);
    EXPECT_EQ(server.started, static_cast<int>(fish_audio_concurrency));
    server.shutdown();
    for (auto& pending : in_flight) {
        EXPECT_EQ(pending.wait_for(10s), std::future_status::ready);
        EXPECT_TRUE(pending.get().cancelled);
    }
}

TEST(FishAudio, IgnoresObsoleteVoiceSettingsWithoutChangingConfiguredModel) {
    const WorkspaceVoiceOutput output{
        .model = "s2.1-pro", .output_format = "mp3",
    };
    const auto request = make_fish_audio_request(output, {
        {"text", "Hello"}, {"reference_id", "fish-voice"},
        {"settings", {{"speed", 0.9}, {"stability", 0.5}, {"use_speaker_boost", true}}},
    });
    const auto typed = make_fish_audio_request(output, "Hello",
        FishAudioSynthesis{.reference_id = "fish-voice", .settings = {.speed = 0.9}});
    EXPECT_EQ(typed.model, request.model);
    EXPECT_EQ(typed.body, request.body);
    EXPECT_EQ(request.model, "s2.1-pro");
    EXPECT_EQ(request.body, Json({
        {"text", "Hello"}, {"reference_id", "fish-voice"},
        {"prosody", {{"speed", 0.9}}}, {"format", "mp3"},
    }));
}

TEST(FishAudio, PreservesExplicitFishAudioModelAndFormat) {
    for (const std::string format : {"mp3", "wav", "opus"}) {
        const WorkspaceVoiceOutput output{
            .model = "s2.1-pro-free", .output_format = format,
        };
        const auto request = make_fish_audio_request(output, {
            {"text", "Hello"}, {"reference_id", "fish-voice"},
        });
        EXPECT_EQ(request.model, "s2.1-pro-free");
        EXPECT_EQ(request.body, Json({
            {"text", "Hello"}, {"reference_id", "fish-voice"}, {"format", format},
        }));
    }
}

TEST(FishAudio, TrimsModelsAndPreservesExplicitValues) {
    for (const std::string model : {"s2.1-pro", "custom/model", "obsolete model"}) {
        const WorkspaceVoiceOutput output{
            .model = normalize_voice_output_model(" \t" + model + " \n"), .output_format = "mp3",
        };
        EXPECT_EQ(make_fish_audio_request(output, {
            {"text", "Hello"}, {"reference_id", "fish-voice"},
        }).model, model);
    }
    EXPECT_THROW(normalize_voice_output_model("model\r\nother-header: value"), std::invalid_argument);
    EXPECT_THROW(normalize_voice_output_model(" \t"), std::invalid_argument);
}

TEST(FishAudio, NormalizesEndpointSpellingsAndDefaultPorts) {
    for (const std::string url : {
             "https://api.fish.audio", "https://api.fish.audio/",
             "HTTPS://API.FISH.AUDIO:443", " https://API.FISH.AUDIO:443/v1/tts ",
         }) {
        const auto endpoint = parse_voice_output_endpoint(url);
        EXPECT_EQ(endpoint, "https://api.fish.audio/v1/tts");
    }
    const auto custom_port = parse_voice_output_endpoint("https://API.FISH.AUDIO:8443/v1/tts");
    EXPECT_EQ(custom_port, "https://api.fish.audio:8443/v1/tts");
    EXPECT_THROW(parse_voice_output_endpoint("https://api.fish.audio.example/v1/tts"), std::invalid_argument);
    EXPECT_THROW(parse_voice_output_endpoint("https://api.elevenlabs.io/v1/text-to-speech"), std::invalid_argument);
    EXPECT_THROW(parse_voice_output_endpoint("http://API.FISH.AUDIO/v1/tts"), std::invalid_argument);
}

TEST(FishAudio, RejectsMalformedSynthesisInput) {
    const WorkspaceVoiceOutput output{
        .model = "s2.1-pro", .output_format = "mp3",
    };
    for (const Json& input : {
             Json::object(),
             Json{{"text", 1}, {"reference_id", "voice"}},
             Json{{"text", "Hello"}, {"reference_id", ""}},
             Json{{"text", "Hello"}, {"reference_id", "voice"}, {"settings", nullptr}},
             Json{{"text", "Hello"}, {"reference_id", "voice"}, {"settings", {{"speed", 0.1}}}},
             Json{{"text", "Hello"}, {"reference_id", "voice"}, {"settings", {{"speed", "fast"}}}},
         }) {
        EXPECT_THROW(make_fish_audio_request(output, input), std::invalid_argument);
    }
}

TEST(FishAudio, TypedBuilderChecksIdentityAndSpeedLimits) {
    const WorkspaceVoiceOutput output{.model = "s2.1-pro", .output_format = "mp3"};
    const std::optional<std::string> reference{"voice"};
    for (const double speed : {0.5, 2.0}) {
        const auto typed = make_fish_audio_request(output, "Hello",
            FishAudioSynthesis{.reference_id = reference, .settings = {.speed = speed}});
        const auto adapted = make_fish_audio_request(output,
            {{"text", "Hello"}, {"reference_id", "voice"}, {"settings", {{"speed", speed}}}});
        EXPECT_EQ(typed.body, adapted.body);
        EXPECT_EQ(typed.body.at("prosody").at("speed"), speed);
    }
    for (const double speed : {0.49, 2.01, std::numeric_limits<double>::infinity(),
             std::numeric_limits<double>::quiet_NaN()}) {
        EXPECT_THROW(make_fish_audio_request(output, "Hello",
            FishAudioSynthesis{.reference_id = reference, .settings = {.speed = speed}}),
            std::invalid_argument);
    }
    EXPECT_THROW(make_fish_audio_request(output, "Hello", FishAudioSynthesis{}), std::invalid_argument);
    EXPECT_THROW(make_fish_audio_request(output, "Hello", FishAudioSynthesis{.reference_id = ""}),
        std::invalid_argument);
    EXPECT_THROW(make_fish_audio_request(output, "", FishAudioSynthesis{.reference_id = reference}), std::invalid_argument);
}

TEST(FishAudio, DecoderRetainsFailuresAndWarnsAboutIgnoredSettingsOnlyWhenConsumed) {
    test::TestWorkspace workspace;
    const auto log_file = workspace.root() / "fish-audio.log";
    initialize_diagnostic_logging(log_file, "warn");
    struct StopLogging { ~StopLogging() { shutdown_diagnostic_logging(); } } stop;
    const auto contents = [&] {
        std::ifstream input(log_file);
        return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    };
    const WorkspaceVoiceOutput output{.model = "s2.1-pro", .output_format = "mp3"};
    const auto synthesis = decode_fish_audio_synthesis({{"reference_id", "voice"},
        {"settings", {{"speed", 0.9}, {"stability", 0.5}, {"use_speaker_boost", true}}}});
    EXPECT_EQ(synthesis.settings.speed, 0.9);
    EXPECT_EQ(synthesis.ignored_settings, (std::vector<std::string>{"stability", "use_speaker_boost"}));
    EXPECT_EQ(contents().find("Ignoring unsupported"), std::string::npos);
    const auto request = make_fish_audio_request(output, "Hello", synthesis);
    EXPECT_FALSE(request.body.contains("stability"));
    EXPECT_FALSE(request.body.contains("use_speaker_boost"));
    EXPECT_NE(contents().find("Ignoring unsupported FishAudio voice setting: stability"), std::string::npos);
    EXPECT_NE(contents().find("Ignoring unsupported FishAudio voice setting: use_speaker_boost"), std::string::npos);
    for (const Json& settings : {Json(nullptr), Json{{"speed", "fast"}}}) {
        const auto malformed = decode_fish_audio_synthesis({{"reference_id", "voice"}, {"settings", settings}});
        ASSERT_TRUE(malformed.decoding_failure);
        EXPECT_THROW(make_fish_audio_request(output, "Hello", malformed), std::invalid_argument);
    }
}

TEST(FishAudio, ForwardsAuthenticationAndReturnsAudioBytes) {
    MockHttpServer server({
        "HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nContent-Length: 5\r\nConnection: close\r\n\r\naudio",
    });
    server.start();
    const WorkspaceVoiceOutput output{
        .url = "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/tts",
        .model = "custom/model", .output_format = "mp3",
    };
    const auto request = make_fish_audio_request(output, {
        {"text", "Hello"}, {"reference_id", "voice"},
    });
    const auto result = download_fish_audio(
        output, "fish-secret", request, [] { return false; });
    server.join();
    ASSERT_EQ(server.requests().size(), 1);
    const std::string& sent = server.requests()[0];
    EXPECT_NE(sent.find("POST /v1/tts "), std::string::npos);
    EXPECT_NE(sent.find("Authorization: Bearer fish-secret"), std::string::npos);
    EXPECT_NE(sent.find("model: custom/model"), std::string::npos);
    EXPECT_NE(sent.find(request.body.dump()), std::string::npos);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->audio, "audio");
    EXPECT_EQ(result->content_type, "audio/mpeg");
}

TEST(FishAudio, BackgroundDownloadAcceptsAudioSubtypesAndParameters) {
    for (const std::string type : {"audio/opus", "audio/mpeg; charset=binary", "Audio/Ogg; codecs=opus", "audio/x-wav"}) {
        SCOPED_TRACE(type);
        MockHttpServer server({"HTTP/1.1 200 OK\r\nContent-Type: " + type
            + "\r\nContent-Length: 5\r\nConnection: close\r\n\r\naudio"});
        server.start();
        const WorkspaceVoiceOutput output{.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/tts",
            .model = "s2.1-pro", .output_format = "opus"};
        const auto result = download_fish_audio(output, "secret", make_fish_audio_request(output,
            {{"text", "Hello"}, {"reference_id", "voice"}}), [] { return false; });
        server.join();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->audio, "audio");
        EXPECT_EQ(result->content_type, type);
    }
    EXPECT_FALSE(valid_entry_audio({"", "audio/opus"}));
    EXPECT_FALSE(valid_entry_audio({"error", "application/json"}));
    EXPECT_FALSE(valid_entry_audio({"audio", "audio/; codecs=opus"}));
    EXPECT_FALSE(valid_entry_audio({"audio", ""}));
}

TEST(FishAudio, PreservesUpstreamErrors) {
    const std::string error = Json({{"message", "Insufficient credits"}}).dump();
    MockHttpServer server({
        "HTTP/1.1 402 Payment Required\r\nContent-Type: application/json\r\nContent-Length: "
        + std::to_string(error.size()) + "\r\nConnection: close\r\n\r\n" + error,
    });
    server.start();
    const WorkspaceVoiceOutput output{
        .url = "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/tts",
        .model = "s2.1-pro", .output_format = "mp3",
    };
    EXPECT_THROW(
        (void)download_fish_audio(
            output, "fish-secret",
            make_fish_audio_request(output, {
                {"text", "Hello"}, {"reference_id", "voice"},
            }),
            [] { return false; }),
        std::runtime_error);
    server.join();
    EXPECT_EQ(fish_audio_http_error_message(402), "Insufficient FishAudio credits.");
}

TEST(FishAudio, SuppliesUsefulErrorsWhenUpstreamBodyIsEmpty) {
    for (const auto& [status, message] : std::vector<std::pair<int, std::string>>{
             {401, "Authentication failed. Check the FishAudio API key."},
             {429, "FishAudio rate limit reached. Try again shortly."},
             {503, "FishAudio request failed."}}) {
        MockHttpServer server({
            "HTTP/1.1 " + std::to_string(status)
            + " Error\r\nContent-Length: 2\r\nConnection: close\r\n\r\n  ",
        });
        server.start();
        const WorkspaceVoiceOutput output{
            .url = "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/tts",
            .model = "s2.1-pro", .output_format = "mp3",
        };
        EXPECT_THROW(
            (void)download_fish_audio(
                output, "fish-secret",
                make_fish_audio_request(output, {
                    {"text", "Hello"}, {"reference_id", "voice"},
                }),
                [] { return false; }),
            std::runtime_error);
        server.join();
        EXPECT_EQ(fish_audio_http_error_message(status), message);
    }
}

TEST(FishAudio, IncludesCurlCodeAndDiagnosticsForTransportFailures) {
    MockHttpServer server({""}); // Close after reading the request, without sending HTTP headers.
    server.start();
    const WorkspaceVoiceOutput output{
        .url = "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/tts",
        .model = "s2.1-pro", .output_format = "mp3",
    };
    try {
        (void)download_fish_audio(
            output, "fish-secret",
            make_fish_audio_request(output, {
                {"text", "Hello"}, {"reference_id", "voice"},
            }),
            [] { return false; });
        FAIL() << "Expected a transport failure";
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("curl 52"), std::string::npos);
        EXPECT_NE(message.find("Empty reply from server"), std::string::npos);
        EXPECT_NE(message.find("; "), std::string::npos); // Includes curl's error buffer too.
        EXPECT_EQ(message.find("fish-secret"), std::string::npos);
    }
    server.join();
}

TEST(FishAudio, EntrySpeechTextOmitsEmptyAndMetadataOnlyEntries) {
    test::TestWorkspace workspace;
    const auto path = test::import_test_database(workspace.root());
    {
        storage::SqliteDatabase database(path, storage::SqliteDatabase::Mode::read_write);
        database.execute("INSERT INTO forums (forum_id) VALUES ('lobby')");
        database.execute("INSERT INTO sessions (forum_key, session_id, label, updated_at, "
            "history_epoch, next_entry_id, next_request_id) VALUES (1, 'audio', 'Audio', 1, 1, 4, 1)");
        database.execute("INSERT INTO entries (session_key, entry_id, epoch, kind, participant_id, "
            "display_name, addressed_to, addressed_to_name, text, status) VALUES "
            "(1, 1, 1, 0, 'human', 'You', '-', '-', '', 0), "
            "(1, 2, 1, 0, 'human', 'You', '-', '-', 'Stored transcript', 0), "
            "(1, 3, 1, 1, 'guide', 'Guide', '', '', "
            "'  [2026-09-16T12:00:00.123Z] ([source](https://example.com))', 0)");
    }
    const auto config = WorkspaceConfigStore::open(path);
    const SessionRepository sessions(
            [&config] { return config->snapshot(); }, path, config->workspace_path(), config->welcome_path(),
        {{"temporary-forum", "temporary-session"}, "Welcome"});
    const auto empty = sessions.lookup_entry_audio({"lobby", "audio"}, 1);
    ASSERT_TRUE(empty);
    EXPECT_TRUE(entry_speech_text(*empty).empty());
    const auto stored = sessions.lookup_entry_audio({"lobby", "audio"}, 2);
    ASSERT_TRUE(stored);
    EXPECT_EQ(entry_speech_text(*stored), "Stored transcript");
    const auto metadata = sessions.lookup_entry_audio({"lobby", "audio"}, 3);
    ASSERT_TRUE(metadata);
    EXPECT_TRUE(entry_speech_text(*metadata).empty());
}
}
}
