#include "providers/voice_output.h"
#include "support/xai_fake_server.h"
#include <gtest/gtest.h>
#include <future>
#include <thread>

namespace cha {
namespace {
using Json = nlohmann::json;
using namespace std::chrono_literals;
std::string packed(const Json& value) {
    const auto bytes = Json::to_msgpack(value);
    return {bytes.begin(), bytes.end()};
}
bool eventually(const std::function<bool()>& ready) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!ready() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(2ms);
    return ready();
}
WorkspaceVoiceProviderOutput output_for(const XaiFakeServer& server, bool fish, std::string model) {
    return {.url = "http://127.0.0.1:" + std::to_string(server.port()) + (fish ? "/v1/tts" : "/v1/text-to-speech"),
        .model = std::move(model), .output_format = fish ? "mp3" : "mp3_44100_128", .connection = "websocket"};
}
}

TEST(VoiceWebSocket, StreamsAllThreeProtocolsBeforeInputEndsAndDrainsFinalAudio) {
    for (const auto* model : {"s2.1-pro", "eleven_flash_v2_5", "eleven_multilingual_v2", "eleven_v3", "eleven_v4", "eleven_v4_turbo",
        "s3_future", "eleven_future", "eleven_v3_future", "eleven_v4_future"}) {
        SCOPED_TRACE(model);
        const bool fish = std::string_view(model).starts_with('s');
        const bool dialogue = std::string_view(model).starts_with("eleven_v3") || std::string_view(model).starts_with("eleven_v4");
        const auto first = fish ? packed({{"event", "audio"}, {"audio", Json::binary({'a', 'b'})}})
            : Json{{"audio", "YWI="}, {dialogue ? "is_final" : "isFinal", nullptr}}.dump();
        const auto tail = fish ? packed({{"event", "audio"}, {"audio", Json::binary({'c', 'd'})}})
            : Json{{"audio", "Y2Q="}}.dump();
        const auto done = fish ? packed({{"event", "finish"}, {"reason", "stop"}})
            : Json{{dialogue ? "is_final" : "isFinal", true}}.dump();
        XaiFakeServer server({.after_audio_done = {tail, done}, .speech = true, .msgpack = fish, .after_text = {first}, .send_ping = true});
        server.start();
        const auto output = output_for(server, fish, model);
        const auto request = make_voice_output_request(output, std::nullopt,
            {.reference_id = "voice/id", .settings = {.speed = 0.9}, .provider = fish ? "fishaudio" : "elevenlabs"});
        std::atomic_bool finish{false}, received{false}, cancel{false};
        auto transfer = std::async(std::launch::async, [&] {
            bool sent = false;
            return stream_voice_websocket(output, "fixture-secret", request, [&] { return cancel.load(); },
                [&](auto, auto) { received = true; }, [&] {
                    if (!sent) { sent = true; return VoiceTextChunk{"Hello.", false}; }
                    return VoiceTextChunk{finish ? " World." : "", finish.load()};
                });
        });
        const bool early_audio = eventually([&] { return received.load(); });
        EXPECT_TRUE(early_audio);
        EXPECT_EQ(transfer.wait_for(0ms), std::future_status::timeout);
        finish = true;
        if (!early_audio) cancel = true;
        const auto audio = transfer.get();
        ASSERT_TRUE(audio);
        EXPECT_EQ(audio->audio, "ababcd");
        server.join();
        EXPECT_NE(server.request().find(fish ? "GET /v1/tts/live" : dialogue ? "GET /v1/text-to-dialogue/stream-input" : "GET /v1/text-to-speech/voice%2Fid/stream-input"), std::string::npos);
        EXPECT_NE(server.request().find(fish ? "Authorization: Bearer fixture-secret" : "xi-api-key: fixture-secret"), std::string::npos);
        if (fish) {
            EXPECT_NE(server.request().find("model: " + std::string(model)), std::string::npos);
            const auto messages = server.binary_messages();
            ASSERT_EQ(messages.size(), 6U);
            const auto init = Json::from_msgpack(messages.front());
            EXPECT_EQ(init["request"]["text"], "");
            EXPECT_EQ(init["request"]["latency"], "low");
            EXPECT_EQ(init["request"]["prosody"]["speed"], 0.9);
            EXPECT_EQ(init["request"]["reference_id"], "voice/id");
            EXPECT_EQ(Json::from_msgpack(messages.back())["event"], "stop");
        } else {
            const auto messages = server.text_messages();
            ASSERT_EQ(messages.size(), 4U);
            const auto init = Json::parse(messages.front());
            if (dialogue) EXPECT_EQ(init["voices"], Json::array({"voice/id"}));
            else EXPECT_EQ(init["voice_settings"]["speed"], 0.9);
            EXPECT_EQ(Json::parse(messages.back()), (dialogue ? Json{{"close_socket", true}} : Json{{"text", ""}}));
        }
    }
}

TEST(VoiceWebSocket, FlushesSentenceAndLineEndingsWithoutFlushingEveryTextChunk) {
    const std::vector<std::string> chunks{"A few", " more words", " end. More words",
        " in one sentence! Next thought", " continues? Some more", " on this line\nNext line",
        " has two. Sentences! And a tail", " ends.", " Final fragment"};
    const std::vector<std::pair<std::string, bool>> expected{
        {"A few", false}, {" more words", false}, {" end.", true}, {" More words", false},
        {" in one sentence!", true}, {" Next thought", false}, {" continues?", true}, {" Some more", false},
        {" on this line\n", true}, {"Next line", false}, {" has two. Sentences!", true}, {" And a tail", false},
        {" ends.", true}, {" Final fragment", false}};
    for (const auto* model : {"s2.1-pro", "eleven_flash_v2_5", "eleven_v4_turbo"}) {
        SCOPED_TRACE(model);
        const bool fish = std::string_view(model).starts_with('s');
        const bool dialogue = std::string_view(model).starts_with("eleven_v4");
        const auto done = fish ? packed({{"event", "finish"}, {"reason", "stop"}})
            : Json{{dialogue ? "is_final" : "isFinal", true}}.dump();
        const auto tail = fish ? packed({{"event", "audio"}, {"audio", Json::binary({'a'})}})
            : Json{{"audio", "YQ=="}}.dump();
        XaiFakeServer server({.after_audio_done = {tail, done}, .speech = true, .msgpack = fish});
        server.start();
        const auto output = output_for(server, fish, model);
        std::size_t next = 0;
        const auto audio = stream_voice_websocket(output, "key", make_voice_output_request(output, std::nullopt,
            {.reference_id = "voice", .provider = fish ? "fishaudio" : "elevenlabs"}),
            [] { return false; }, {}, [&] {
                const auto text = chunks.at(next++);
                return VoiceTextChunk{text, next == chunks.size()};
            });
        ASSERT_TRUE(audio);
        EXPECT_EQ(audio->audio, "a");
        server.join();
        std::vector<Json> messages;
        if (fish) {
            for (const auto& bytes : server.binary_messages()) messages.push_back(Json::from_msgpack(bytes));
        } else {
            for (const auto& text : server.text_messages()) messages.push_back(Json::parse(text));
        }
        std::size_t index = 1; // Initialization precedes the text chunks.
        for (const auto& [text, flush] : expected) {
            ASSERT_LT(index, messages.size());
            const auto& message = messages[index++];
            EXPECT_EQ(fish || !dialogue ? message["text"] : message["inputs"][0]["text"], text);
            if (fish && flush) {
                ASSERT_LT(index, messages.size());
                EXPECT_EQ(messages[index++]["event"], "flush");
            } else if (!fish) EXPECT_EQ(message["flush"], flush);
        }
        ASSERT_EQ(index + 1, messages.size());
        EXPECT_EQ(messages[index], (fish ? Json{{"event", "stop"}}
            : dialogue ? Json{{"close_socket", true}} : Json{{"text", ""}}));
    }
}

TEST(VoiceWebSocket, SupportsCompletedSpeechAndDoesNotTreatATurnBoundaryAsCompletion) {
    XaiFakeServer server({.after_audio_done = {R"({"audio":"YQ==","is_final":true})"},
        .speech = true, .after_text = {R"({"is_final_audio_for_turn":true})"}});
    server.start();
    const auto output = output_for(server, false, "eleven_v4_turbo");
    const auto audio = download_voice_output(output, "key", make_voice_output_request(output, "Hello.",
        {.reference_id = "voice", .provider = "elevenlabs"}), [] { return false; });
    ASSERT_TRUE(audio);
    EXPECT_EQ(audio->audio, "a");
}

TEST(VoiceWebSocket, CancelsWhileInputIsStillOpenAndClosesTheConnection) {
    XaiFakeServer server({.speech = true, .after_text = {R"({"audio":"YQ=="})"}});
    server.start();
    const auto output = output_for(server, false, "eleven_flash_v2_5");
    std::atomic_bool cancel{false}, received{false};
    auto transfer = std::async(std::launch::async, [&] {
        bool sent = false;
        return stream_voice_websocket(output, "secret", make_voice_output_request(output, std::nullopt,
            {.reference_id = "voice", .provider = "elevenlabs"}), [&] { return cancel.load(); },
            [&](auto, auto) { received = true; }, [&] { return VoiceTextChunk{std::exchange(sent, true) ? "" : "Hello.", false}; });
    });
    EXPECT_TRUE(eventually([&] { return received.load(); }));
    cancel = true;
    EXPECT_FALSE(transfer.get());
}

TEST(VoiceWebSocket, RejectsDisconnectProviderErrorsMalformedAudioAndEarlyCompletion) {
    for (const auto& options : {
        XaiFakeServerOptions{.speech = true, .after_text = {R"({"audio":"YQ=="})"}, .close_after_text = true},
        XaiFakeServerOptions{.speech = true, .after_text = {R"({"error":"bad configuration"})"}},
        XaiFakeServerOptions{.speech = true, .after_text = {R"({"audio":"invalid!"})"}},
        XaiFakeServerOptions{.speech = true, .after_text = {R"({"isFinal":true})"}}}) {
        XaiFakeServer server(options);
        server.start();
        const auto output = output_for(server, false, "eleven_multilingual_v2");
        bool sent = false;
        EXPECT_THROW(stream_voice_websocket(output, "secret", make_voice_output_request(output, std::nullopt,
            {.reference_id = "voice", .provider = "elevenlabs"}), [] { return false; }, {},
            [&] { return VoiceTextChunk{std::exchange(sent, true) ? "" : "Hello.", false}; }), std::runtime_error);
    }
    XaiFakeServer fish({.speech = true, .msgpack = true, .after_text = {packed({{"event", "finish"}, {"reason", "error"}})}});
    fish.start();
    const auto output = output_for(fish, true, "s2-pro");
    EXPECT_THROW(download_voice_output(output, "key", make_voice_output_request(output, "Hello.",
        {.reference_id = "voice"}), [] { return false; }), std::runtime_error);
}

TEST(VoiceWebSocket, CleansFormattingAcrossEveryPossibleChunkBoundary) {
    for (const std::string text : {
        "# Heading\n\n**Bold across\nmodel chunks.** Next sentence. End",
        "Hello. [a link](https://example.com/x.y) Next. Final",
        "One. `code across\nchunks` Next. Final",
        "Intro.\n\n```cpp\ncode();\n```\nFinal.",
        "First. _Italic sentence._ Next sentence. Final.",
        "- First item.\n- Second item.\n\nFinal.",
        "1. First item.\n2. Second item.\n\nFinal.",
        "# Heading with closing hashes ##\n\nFinal.",
        "Intro. **Outer *nested formatting* still bold.** Final.",
        "Intro. \\*escaped formatting across chunks.\\* Final.",
        "***Nested **bold** formatting*** Final.",
        "Intro. _snake_case inside emphasis_ Final.",
        "> > **Quoted heading**\n\n- [x] First.\n- [ ] Second.\n",
        "Intro. [reference label][id] Final.",
        "Intro.\n- - -\nFinal.",
        "Intro.\n* * *\nFinal.",
        "Intro.\n_ _ _\nFinal."}) {
        for (const auto* provider : {"fishaudio", "elevenlabs"}) {
            const auto final = speech_text_prefix(text, provider, true);
            std::string spoken;
            for (std::size_t end = 1; end < text.size(); ++end) {
                const auto partial = speech_text_prefix(std::string_view(text).substr(0, end), provider, false);
                ASSERT_TRUE(partial.starts_with(spoken)) << text << " at " << end;
                ASSERT_TRUE(final.starts_with(partial)) << text << " at " << end;
                spoken = partial;
            }
        }
    }
    EXPECT_EQ(speech_text_prefix("Words without punctuation continue ", "fishaudio", false), "Words without punctuation continue");
    EXPECT_EQ(speech_text_prefix("First sentence. Second", "fishaudio", false), "First sentence.");
}
}
