#include "web/fish_audio.h"
#include "web/http_server.h"
#include "web/web_settings.h"
#include "providers/voice_output_config.h"
#include "workspace/workspace.h"
#include "support/mock_http_server.h"

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

TEST(FishAudio, TranslatesMp3FormatWithoutChangingConfiguredModel) {
    const WorkspaceVoiceOutput output{
        .model = "eleven_multilingual_v2", .output_format = "mp3_44100_128",
    };
    const auto request = make_fish_audio_request(output, {
        {"text", "Hello"}, {"reference_id", "fish-voice"},
        {"settings", {{"speed", 0.9}, {"stability", 0.5}, {"use_speaker_boost", true}}},
    });
    EXPECT_EQ(request.model, "eleven_multilingual_v2");
    EXPECT_EQ(request.body, Json({
        {"text", "Hello"}, {"reference_id", "fish-voice"},
        {"prosody", {{"speed", 0.9}}}, {"format", "mp3"},
        {"sample_rate", 44100}, {"mp3_bitrate", 128},
    }));
}

TEST(FishAudio, UsesFishDefaultsForLegacyMp3AndPreservesLegacyOpus) {
    WorkspaceVoiceOutput output{.model = "s1", .output_format = "mp3_22050_32"};
    const Json input{{"text", "Hello"}, {"reference_id", "fish-voice"}};
    EXPECT_EQ(make_fish_audio_request(output, input).body, Json({
        {"text", "Hello"}, {"reference_id", "fish-voice"}, {"format", "mp3"},
    }));
    output.output_format = "opus_48000_64";
    EXPECT_EQ(make_fish_audio_request(output, input).body, Json({
        {"text", "Hello"}, {"reference_id", "fish-voice"}, {"format", "opus"},
        {"sample_rate", 48000}, {"opus_bitrate", 64000},
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

TEST(FishAudio, FallsBackToPlayableMp3ForUnsupportedFormats) {
    const WorkspaceVoiceOutput output{
        .model = "s2.1-pro", .output_format = "pcm_44100",
    };
    EXPECT_EQ(make_fish_audio_request(output, {
        {"text", "Hello"}, {"reference_id", "fish-voice"},
    }).body.at("format"), "mp3");
}

TEST(FishAudio, TrimsModelsAndPreservesExplicitValues) {
    for (const std::string model : {"eleven_multilingual_v2", "custom/model", "obsolete model"}) {
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
        EXPECT_TRUE(endpoint.fish_audio);
        EXPECT_EQ(endpoint.url, "https://api.fish.audio/v1/tts");
    }
    const auto custom_port = parse_voice_output_endpoint("https://API.FISH.AUDIO:8443/v1/tts");
    EXPECT_TRUE(custom_port.fish_audio);
    EXPECT_EQ(custom_port.url, "https://api.fish.audio:8443/v1/tts");
    EXPECT_FALSE(parse_voice_output_endpoint("https://api.fish.audio.example/v1/tts").fish_audio);
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
}
}
