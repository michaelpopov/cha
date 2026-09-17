#include "web/fish_audio.h"
#include "util/logging.h"
#include "web/http_server.h"
#include "web/web_settings.h"
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

namespace cha::web {
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

// Both sides use real sockets. The upstream waits without sending headers,
// exercising cancellation even when no audio bytes are arriving.
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

        // The minimum base pool has one SSE worker and one command worker.
        WebSettings settings;
        settings.session_limit = 1;
        settings.http_request_headroom = 1;
        settings.http_thread_pool_size = 2;
        settings.http_pending_request_limit = 2;
        configure_http_server(server_, settings, true);
        server_.Post("/speech", [&](const httplib::Request& request, httplib::Response& response) {
            proxy_.forward(output_, "secret", make_fish_audio_request(output_, {
                {"text", "Hello"}, {"reference_id", "voice"},
            }), response, request.is_connection_closed);
            completed++;
        });
        server_.Get("/snapshot", [](const httplib::Request&, httplib::Response& response) {
            response.set_content("snapshot", "text/plain");
        });
        server_.Get("/events", [&](const httplib::Request& request, httplib::Response& response) {
            event_started = true;
            while (!shutting_down_ && !request.is_connection_closed()) {
                std::this_thread::sleep_for(10ms);
            }
            response.status = 204;
        });
        port = server_.bind_to_any_port("127.0.0.1");
        if (port < 1) throw std::runtime_error("Could not bind proxy test server");
        server_thread_ = std::thread([&] { server_.listen_after_bind(); });
        server_.wait_until_ready();
    }

    ~SlowFishAudioServer() {
        finish_ = true;
        shutdown();
        upstream_.stop();
        if (upstream_thread_.joinable()) upstream_thread_.join();
    }

    void shutdown() {
        proxy_.stop();
        shutting_down_ = true;
        server_.stop();
        if (server_thread_.joinable()) server_thread_.join();
    }

    std::optional<EntryAudio> download(const std::function<bool()>& cancelled) {
        return download_fish_audio(output_, "secret", make_fish_audio_request(output_, {
            {"text", "Hello"}, {"reference_id", "voice"},
        }), cancelled);
    }

    std::atomic_int started{0};
    std::atomic_int finished{0};
    std::atomic_int disconnected{0};
    std::atomic_int completed{0};
    std::atomic_bool event_started{false};
    int port = 0;

private:
    FishAudioProxy proxy_;
    WorkspaceVoiceOutput output_;
    httplib::Server upstream_;
    httplib::Server server_;
    std::thread upstream_thread_;
    std::thread server_thread_;
    std::atomic_bool finish_{false};
    std::atomic_bool shutting_down_{false};
};

TEST(FishAudio, ShutdownCancelsStalledUpstreamTransfer) {
    SlowFishAudioServer server;
    auto pending = std::async(std::launch::async, [&] {
        httplib::Client client("127.0.0.1", server.port);
        client.set_read_timeout(30s);
        return client.Post("/speech", "{}", "application/json");
    });
    ASSERT_TRUE(wait_until([&] { return server.started == 1; }));
    server.shutdown();
    EXPECT_TRUE(wait_until([&] { return server.disconnected == 1; }));
    EXPECT_EQ(pending.wait_for(10s), std::future_status::ready);
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

TEST(FishAudio, BrowserDisconnectCancelsStalledUpstreamTransfer) {
    SlowFishAudioServer server;
    httplib::Client client("127.0.0.1", server.port);
    client.set_read_timeout(30s);
    auto pending = std::async(std::launch::async, [&] {
        return client.Post("/speech", "{}", "application/json");
    });
    ASSERT_TRUE(wait_until([&] { return server.started == 1; }));
    client.stop();
    EXPECT_EQ(pending.wait_for(10s), std::future_status::ready);
    EXPECT_TRUE(wait_until([&] { return server.disconnected == 1 && server.finished == 1; }));
}

TEST(FishAudio, FullSynthesisCapacityPreservesCommandWorkerAndRejectsOverflow) {
    SlowFishAudioServer server;
    auto events = std::async(std::launch::async, [&] {
        httplib::Client client("127.0.0.1", server.port);
        client.set_read_timeout(30s);
        return client.Get("/events");
    });
    ASSERT_TRUE(wait_until([&] { return server.event_started.load(); }));
    std::vector<std::unique_ptr<httplib::Client>> clients;
    std::vector<std::future<httplib::Result>> synthesis;
    for (std::size_t i = 0; i < fish_audio_concurrency; ++i) {
        clients.push_back(std::make_unique<httplib::Client>("127.0.0.1", server.port));
        clients.back()->set_read_timeout(30s);
        synthesis.push_back(std::async(std::launch::async, [client = clients.back().get()] {
            return client->Post("/speech", "{}", "application/json");
        }));
    }
    ASSERT_TRUE(wait_until([&] { return server.started == static_cast<int>(fish_audio_concurrency); }));
    httplib::Client client("127.0.0.1", server.port);
    client.set_read_timeout(10s);
    const auto snapshot = client.Get("/snapshot");
    ASSERT_TRUE(snapshot);
    EXPECT_EQ(snapshot->status, 200);
    EXPECT_EQ(snapshot->body, "snapshot");
    const auto overflow = client.Post("/speech", "{}", "application/json");
    ASSERT_TRUE(overflow);
    EXPECT_EQ(overflow->status, 503);
    EXPECT_EQ(Json::parse(overflow->body).at("error").at("code"), "speech_busy");
    EXPECT_EQ(server.started, static_cast<int>(fish_audio_concurrency));
    clients.front()->stop();
    ASSERT_TRUE(wait_until([&] { return server.disconnected == 1 && server.completed >= 2; }));
    auto replacement = std::async(std::launch::async, [&] {
        httplib::Client next("127.0.0.1", server.port);
        next.set_read_timeout(30s);
        return next.Post("/speech", "{}", "application/json");
    });
    ASSERT_TRUE(wait_until([&] { return server.started == static_cast<int>(fish_audio_concurrency) + 1; }));
    server.shutdown();
    EXPECT_TRUE(wait_until([&] { return server.disconnected == static_cast<int>(fish_audio_concurrency) + 1; }));
    EXPECT_EQ(events.wait_for(10s), std::future_status::ready);
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
    httplib::Response response;
    forward_fish_audio(output, "fish-secret", request, response);
    server.join();
    ASSERT_EQ(server.requests().size(), 1);
    const std::string& sent = server.requests()[0];
    EXPECT_NE(sent.find("POST /v1/tts "), std::string::npos);
    EXPECT_NE(sent.find("Authorization: Bearer fish-secret"), std::string::npos);
    EXPECT_NE(sent.find("model: custom/model"), std::string::npos);
    EXPECT_NE(sent.find(request.body.dump()), std::string::npos);
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.body, "audio");
    EXPECT_EQ(response.get_header_value("Content-Type"), "audio/mpeg");
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
    httplib::Response response;
    forward_fish_audio(output, "fish-secret", make_fish_audio_request(output, {
        {"text", "Hello"}, {"reference_id", "voice"},
    }), response);
    server.join();
    EXPECT_EQ(response.status, 402);
    EXPECT_EQ(response.body, error);
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
        httplib::Response response;
        forward_fish_audio(output, "fish-secret", make_fish_audio_request(output, {
            {"text", "Hello"}, {"reference_id", "voice"},
        }), response);
        server.join();
        EXPECT_EQ(response.status, status);
        EXPECT_EQ(Json::parse(response.body).at("error").at("message"), message);
        EXPECT_EQ(response.get_header_value("Content-Type"), "application/json");
    }
}

TEST(FishAudio, IncludesCurlCodeAndDiagnosticsForTransportFailures) {
    MockHttpServer server({""}); // Close after reading the request, without sending HTTP headers.
    server.start();
    const WorkspaceVoiceOutput output{
        .url = "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/tts",
        .model = "s2.1-pro", .output_format = "mp3",
    };
    httplib::Response response;
    try {
        forward_fish_audio(output, "fish-secret", make_fish_audio_request(output, {
            {"text", "Hello"}, {"reference_id", "voice"},
        }), response);
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

TEST(FishAudio, LegacyTranscriptRequestsAreRejectedAndPreviewsRequireText) {
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
    ApiKeyStore keys(*config);
    const auto key = keys.create("FishAudio", "secret");
    config->create_voice("Reader", "", "voice");
    config->apply_voice_output_update({
        .url = "https://api.fish.audio/v1/tts", .model = "s2.1-pro",
        .api_key_id = key.id, .output_format = "mp3", .default_voice = "Reader",
    });
    // Valid synthesis reaches the cancelled proxy without making an external request.
    const SessionRepository sessions(path, config->workspace_path(), config->welcome_path(),
        {{"temporary-forum", "temporary-session"}, "Welcome"});
    FishAudioProxy proxy;
    proxy.stop();
    httplib::Server server;
    install_fish_audio_route(server, keys, WebSettings{}, true, proxy);
    const int port = server.bind_to_any_port("127.0.0.1");
    ASSERT_GT(port, 0);
    std::jthread listener([&] { server.listen_after_bind(); });
    struct StopServer {
        httplib::Server& server;
        ~StopServer() { server.stop(); }
    } stop{server};
    server.wait_until_ready();
    httplib::Client client("127.0.0.1", port);
    Json body{
        {"text", "Unrelated browser text"}, {"reference_id", "voice"},
        {"entry", {{"forum_id", "lobby"}, {"session_id", "audio"}, {"entry_id", 1}}},
    };
    const auto empty = client.Post("/api/v1/voice-output/audio", body.dump(), "application/json");
    ASSERT_TRUE(empty);
    EXPECT_EQ(empty->status, 400); // Stored text is empty; browser text cannot replace it.
    body["entry"]["entry_id"] = 3;
    const auto metadata = client.Post("/api/v1/voice-output/audio", body.dump(), "application/json");
    ASSERT_TRUE(metadata);
    EXPECT_EQ(metadata->status, 400); // Hidden timestamp and source references are not spoken.
    body["entry"]["entry_id"] = 2;
    for (const Json& text : {Json(""), Json(nullptr), Json(42)}) {
        SCOPED_TRACE(text.dump());
        body["text"] = text;
        const auto stored = client.Post("/api/v1/voice-output/audio", body.dump(), "application/json");
        ASSERT_TRUE(stored);
        EXPECT_EQ(stored->status, 400); // Legacy entry requests cannot bypass the queue.
    }
    body.erase("text");
    const auto omitted = client.Post("/api/v1/voice-output/audio", body.dump(), "application/json");
    ASSERT_TRUE(omitted);
    EXPECT_EQ(omitted->status, 400);
    body.erase("entry");
    const auto preview = client.Post("/api/v1/voice-output/audio", body.dump(), "application/json");
    ASSERT_TRUE(preview);
    EXPECT_EQ(preview->status, 400); // Previews still require submitted text.
    body["text"] = "Preview text";
    const auto valid_preview = client.Post("/api/v1/voice-output/audio", body.dump(), "application/json");
    ASSERT_TRUE(valid_preview);
    EXPECT_EQ(valid_preview->status, 503);
    const auto entry = sessions.lookup_entry_audio({"lobby", "audio"}, 2);
    ASSERT_TRUE(entry);
    EXPECT_FALSE(entry->cached); // Cancelled synthesis must not save a clip.
}
}
}
