#include "app/application.h"
#include "bridge/bridge_router.h"
#include "media/xai_socket.h"
#include "media/xai_transcript.h"
#include "support/test_workspace.h"
#include "support/xai_fake_server.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cha::app {
namespace {

using namespace std::chrono_literals;

ApplicationCommand make_command(
    const test::TestWorkspace& workspace,
    const std::filesystem::path& database) {
    const std::filesystem::path config_directory = workspace.root() / "cha-config";
    std::filesystem::create_directories(config_directory);
    std::ofstream(config_directory / "app.toml")
        << "vault = \"Test\"\n[logging]\nfile = \"runtime.log\"\nlevel = \"off\"\n";
    std::ofstream(config_directory / "test.toml")
        << "vault_name = \"Test\"\ndata = " << std::quoted(database.string()) << "\n";
    const ConfigurationDirectory loaded = load_configuration_directory(config_directory);
    const VaultDefinition* const vault = find_vault(loaded.vaults, loaded.startup_vault);
    std::vector<VaultDefinition> vaults = loaded.vaults;
    VaultDefinition selected = *vault;
    vaults.front() = selected;
    return {
        .config_directory = loaded.directory,
        .vaults = std::move(vaults),
        .vault = std::move(selected),
        .log_file = loaded.log_file,
        .log_level = loaded.log_level,
        .warnings = loaded.warnings,
    };
}

nlohmann::json wait_reply(
    const std::shared_ptr<OperationReply>& reply,
    std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto peeked = reply->peek()) {
            if (const auto* failure = std::get_if<OperationReply::Failure>(&*peeked)) {
                throw ApplicationError(failure->code, failure->message);
            }
            return std::get<nlohmann::json>(*peeked);
        }
        std::this_thread::sleep_for(2ms);
    }
    throw std::runtime_error("Timed out waiting for an xAI reply");
}

struct OwnedApp {
    test::TestWorkspace workspace;
    std::unique_ptr<Application> application;

    OwnedApp() {
        const auto database = test::import_test_database(workspace.root());
        application = Application::open(make_command(workspace, database));
    }

    ~OwnedApp() {
        if (!application) return;
        application->request_shutdown();
        (void)application->join_shutdown(2s);
    }
};

void save_xai(Application& application, std::string url, std::string key = "xai-secret") {
    const auto epoch = application.context_epoch();
    const auto created = application.create_api_key(
        {.display_name = "xAI", .value = key}, epoch);
    (void)application.save_voice_input_settings({
        .provider = "xai",
        .url = std::move(url),
        .model = "grok-voice-transcribe-2.0",
        .api_key = created.id,
        .delay = "low",
        .prompt = "do not send",
    }, epoch);
}

struct Script {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<media::XaiIncoming> incoming;
    std::vector<std::string> text;
    std::vector<std::vector<unsigned char>> binary;
    std::string url;
    std::string authorization;
    bool connected = false;
    bool closed = false;
    std::atomic_bool stall_send{false};
};

class ScriptedSocket final : public media::XaiSocket {
public:
    explicit ScriptedSocket(std::shared_ptr<Script> script) : script_(std::move(script)) {}

    void connect(
        const std::string& url,
        const std::string& authorization,
        std::chrono::steady_clock::time_point deadline,
        const std::function<bool()>& cancelled) override {
        {
            std::lock_guard lock(script_->mu);
            script_->url = url;
            script_->authorization = authorization;
            script_->connected = true;
        }
        script_->cv.notify_all();
        if (cancelled()) {
            throw media::XaiVoiceFailure(
                ErrorCode::operation_cancelled,
                std::string(media::xai_operation_cancelled));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            throw media::XaiVoiceFailure(
                ErrorCode::command_timeout, std::string(media::xai_timed_out));
        }
    }

    media::XaiSendResult send(
        std::string_view payload,
        bool binary,
        std::chrono::steady_clock::time_point deadline,
        const std::function<bool()>& cancelled) override {
        if (script_->stall_send.load()) {
            while (std::chrono::steady_clock::now() < deadline) {
                if (cancelled()) return media::XaiSendResult::cancelled;
                std::this_thread::sleep_for(5ms);
            }
            return media::XaiSendResult::timed_out;
        }
        std::lock_guard lock(script_->mu);
        if (binary) {
            script_->binary.emplace_back(payload.begin(), payload.end());
        } else {
            script_->text.emplace_back(payload);
        }
        script_->cv.notify_all();
        return media::XaiSendResult::sent;
    }

    std::optional<media::XaiIncoming> recv(std::chrono::milliseconds wait) override {
        std::unique_lock lock(script_->mu);
        if (!script_->cv.wait_for(lock, wait, [&] {
                return !script_->incoming.empty() || script_->closed;
            })) {
            return std::nullopt;
        }
        if (script_->incoming.empty()) return media::XaiIncoming{.closed = true};
        auto message = std::move(script_->incoming.front());
        script_->incoming.pop_front();
        return message;
    }

    void close() override {
        std::lock_guard lock(script_->mu);
        script_->closed = true;
        script_->cv.notify_all();
    }

private:
    std::shared_ptr<Script> script_;
};

std::shared_ptr<Script> install_script(Application& application) {
    auto script = std::make_shared<Script>();
    application.set_xai_socket_factory_for_tests([script] {
        return std::make_unique<ScriptedSocket>(script);
    });
    return script;
}

void push_text(Script& script, std::string payload) {
    {
        std::lock_guard lock(script.mu);
        script.incoming.push_back(media::XaiIncoming{.payload = std::move(payload)});
    }
    script.cv.notify_all();
}

void push_event(Script& script, nlohmann::json event) {
    push_text(script, event.dump());
}

void wait_closed(Script& script) {
    std::unique_lock lock(script.mu);
    ASSERT_TRUE(script.cv.wait_for(lock, 2s, [&] { return script.closed; }));
}

nlohmann::json created_event() {
    return {{"type", "transcript.created"}};
}

nlohmann::json final_event(std::string text, double start, double end) {
    return {
        {"type", "transcript.partial"},
        {"is_final", true},
        {"speech_final", false},
        {"text", text},
        {"words", nlohmann::json::array({
            {{"text", text}, {"start", start}, {"end", end}},
        })},
    };
}

std::string encode_base64(const std::vector<unsigned char>& bytes) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    for (std::size_t index = 0; index < bytes.size(); index += 3) {
        const unsigned value = (static_cast<unsigned>(bytes[index]) << 16)
            | (index + 1 < bytes.size() ? static_cast<unsigned>(bytes[index + 1]) << 8 : 0U)
            | (index + 2 < bytes.size() ? static_cast<unsigned>(bytes[index + 2]) : 0U);
        encoded.push_back(table[(value >> 18) & 63U]);
        encoded.push_back(table[(value >> 12) & 63U]);
        encoded.push_back(index + 1 < bytes.size() ? table[(value >> 6) & 63U] : '=');
        encoded.push_back(index + 2 < bytes.size() ? table[value & 63U] : '=');
    }
    return encoded;
}

TEST(XaiVoice, KeepsTheKeyOnTheNativeSocketAndFormatsTheRequest) {
    OwnedApp owned;
    auto script = install_script(*owned.application);
    save_xai(*owned.application, "ws://127.0.0.1:9/v1/stt?dropped=1");
    const auto epoch = owned.application->context_epoch();
    const auto runtime = owned.application->get_voice_input_runtime(epoch);
    ASSERT_TRUE(runtime);
    EXPECT_EQ(nlohmann::json(*runtime).dump().find("xai-secret"), std::string::npos);

    push_event(*script, created_event());
    const auto started = owned.application->start_xai_voice_input(
        "view-1", 4, "dictation-1", {"en"}, epoch, 30000ms);
    const auto result = wait_reply(started);
    EXPECT_EQ(result["session_id"], "dictation-1");
    EXPECT_EQ(result["stop_budget_ms"], 20000);
    EXPECT_EQ(result.dump().find("xai-secret"), std::string::npos);
    EXPECT_EQ(script->authorization, "Authorization: Bearer xai-secret");
    EXPECT_EQ(script->url.find("xai-secret"), std::string::npos);
    EXPECT_EQ(script->url.find("dropped"), std::string::npos);
    EXPECT_NE(script->url.find("language=en"), std::string::npos);
    EXPECT_NE(script->url.find("interim_results=false"), std::string::npos);
    owned.application->cancel_xai_voice_input("view-1", "dictation-1", epoch);
    wait_closed(*script);
}

TEST(XaiVoice, DeliversSeparatePiecesAndSendsAudioBeforeDone) {
    OwnedApp owned;
    auto script = install_script(*owned.application);
    save_xai(*owned.application, "ws://127.0.0.1:9/v1/stt");
    const auto epoch = owned.application->context_epoch();
    push_event(*script, created_event());
    const auto started = owned.application->start_xai_voice_input(
        "view-1", 1, "dictation-1", {"ru"}, epoch, 30000ms);
    EXPECT_EQ(wait_reply(started)["stop_budget_ms"], 20000);
    EXPECT_NE(script->url.find("language=ru"), std::string::npos);

    push_event(*script, final_event("Hello", 0.1, 0.4));
    push_event(*script, final_event("again", 0.5, 0.8));
    const auto first = owned.application->send_xai_voice_audio(
        "view-1", 2, "dictation-1", "AAE=", epoch, 30000ms);
    const auto first_result = wait_reply(first);
    EXPECT_EQ(first_result["pieces"], nlohmann::json::array({"Hello", " again"}));

    push_event(*script, nlohmann::json{
        {"type", "transcript.partial"},
        {"is_final", false},
        {"speech_final", false},
        {"text", "mutable"},
        {"words", nlohmann::json::array()},
    });
    push_event(*script, final_event("Hello", 1.0, 1.4));
    const auto second = owned.application->send_xai_voice_audio(
        "view-1", 3, "dictation-1", encode_base64(std::vector<unsigned char>(3200, 1)),
        epoch, 30000ms);
    EXPECT_EQ(wait_reply(second)["pieces"], nlohmann::json::array({" Hello"}));

    const auto stopped = owned.application->stop_xai_voice_input(
        "view-1", 4, "dictation-1", 20000, epoch, 30000ms);
    {
        std::unique_lock lock(script->mu);
        ASSERT_TRUE(script->cv.wait_for(lock, 2s, [&] {
            return !script->text.empty();
        }));
    }
    push_event(*script, nlohmann::json{
        {"type", "transcript.done"}, {"text", ""}, {"words", nlohmann::json::array()},
    });
    const auto stop_result = wait_reply(stopped);
    EXPECT_EQ(stop_result["pieces"], nlohmann::json::array());
    ASSERT_EQ(script->binary.size(), 2U);
    EXPECT_EQ(script->binary[0], std::vector<unsigned char>({0, 1}));
    EXPECT_EQ(script->binary[1].size(), 3200U);
    ASSERT_EQ(script->text.size(), 1U);
    EXPECT_EQ(script->text[0], "{\"type\":\"audio.done\"}");
}

TEST(XaiVoice, RejectsASecondAudioWhileOneIsOutstandingAndTimesOutTheStall) {
    OwnedApp owned;
    auto script = install_script(*owned.application);
    save_xai(*owned.application, "ws://127.0.0.1:9/v1/stt");
    const auto epoch = owned.application->context_epoch();
    push_event(*script, created_event());
    const auto started = owned.application->start_xai_voice_input(
        "view-1", 1, "dictation-1", {}, epoch, 900ms);
    EXPECT_EQ(wait_reply(started)["stop_budget_ms"], 600);
    EXPECT_EQ(script->url.find("language="), std::string::npos);
    script->stall_send = true;
    const auto began = std::chrono::steady_clock::now();
    const auto audio = owned.application->send_xai_voice_audio(
        "view-1", 2, "dictation-1", "AAE=", epoch, 900ms);
    try {
        (void)wait_reply(owned.application->send_xai_voice_audio(
            "view-1", 3, "dictation-1", "AAE=", epoch, 900ms), 1s);
        FAIL() << "overlapping audio must be rejected";
    } catch (const ApplicationError& error) {
        EXPECT_EQ(error.code, ErrorCode::invalid_argument);
    }
    try {
        (void)wait_reply(audio, 2s);
        FAIL() << "stalled audio must fail";
    } catch (const ApplicationError& error) {
        EXPECT_EQ(error.code, ErrorCode::command_timeout);
        EXPECT_EQ(error.what(), std::string(media::xai_timed_out));
    }
    const auto elapsed = std::chrono::steady_clock::now() - began;
    EXPECT_GE(elapsed, 500ms);
    EXPECT_LT(elapsed, 1500ms);
    EXPECT_EQ(script->binary.size(), 0U);
    wait_closed(*script);
}

TEST(XaiVoice, CancelBeforeStartDoesNotOpenTheSocket) {
    OwnedApp owned;
    auto script = install_script(*owned.application);
    save_xai(*owned.application, "ws://127.0.0.1:9/v1/stt");
    const auto epoch = owned.application->context_epoch();
    owned.application->cancel_xai_voice_input("view-1", "dictation-1", epoch);
    const auto started = owned.application->start_xai_voice_input(
        "view-1", 1, "dictation-1", {"en"}, epoch, 30000ms);
    try {
        (void)wait_reply(started);
        FAIL() << "tombstoned start must fail";
    } catch (const ApplicationError& error) {
        EXPECT_EQ(error.code, ErrorCode::operation_cancelled);
    }
    EXPECT_FALSE(script->connected);
    owned.application->cancel_xai_voice_input("view-1", "missing", epoch);
}

TEST(XaiVoice, RejectsASecondDictationAndANonXaiProvider) {
    OwnedApp owned;
    auto script = install_script(*owned.application);
    save_xai(*owned.application, "ws://127.0.0.1:9/v1/stt");
    const auto epoch = owned.application->context_epoch();
    push_event(*script, created_event());
    (void)wait_reply(owned.application->start_xai_voice_input(
        "view-1", 1, "dictation-1", {"en"}, epoch, 30000ms));
    try {
        (void)wait_reply(owned.application->start_xai_voice_input(
            "view-1", 2, "dictation-2", {"en"}, epoch, 30000ms));
        FAIL() << "one dictation per connection";
    } catch (const ApplicationError& error) {
        EXPECT_EQ(error.what(), std::string("xAI voice input is already active."));
    }
    owned.application->cancel_xai_voice_input("view-1", "dictation-1", epoch);
    wait_closed(*script);

    const auto key = owned.application->create_api_key(
        {.display_name = "OpenAI", .value = "other-secret"}, epoch);
    (void)owned.application->save_voice_input_settings({
        .provider = "openai",
        .url = "https://api.openai.com/v1/realtime/calls",
        .model = "gpt-live-transcribe",
        .api_key = key.id,
        .delay = "low",
        .prompt = "",
    }, epoch);
    try {
        (void)owned.application->start_xai_voice_input(
            "view-1", 3, "dictation-3", {"en"}, epoch, 30000ms);
        FAIL() << "openai provider must not start xAI";
    } catch (const ApplicationError& error) {
        EXPECT_EQ(error.what(), std::string("Voice input provider is not xAI."));
    }
}

TEST(XaiVoice, FailsWhenDoneArrivesBeforeAudioDoneOrTheSocketCloses) {
    OwnedApp owned;
    auto script = install_script(*owned.application);
    save_xai(*owned.application, "ws://127.0.0.1:9/v1/stt");
    const auto epoch = owned.application->context_epoch();
    push_event(*script, created_event());
    (void)wait_reply(owned.application->start_xai_voice_input(
        "view-1", 1, "dictation-1", {"en"}, epoch, 30000ms));
    push_event(*script, final_event("Kept", 0.1, 0.2));
    EXPECT_EQ(
        wait_reply(owned.application->send_xai_voice_audio(
            "view-1", 2, "dictation-1", "AAE=", epoch, 30000ms))["pieces"],
        nlohmann::json::array({"Kept"}));
    push_event(*script, nlohmann::json{
        {"type", "transcript.done"},
        {"text", ""},
        {"words", nlohmann::json::array()},
    });
    wait_closed(*script);
    try {
        (void)wait_reply(owned.application->stop_xai_voice_input(
            "view-1", 3, "dictation-1", 20000, epoch, 30000ms));
        FAIL() << "done before audio.done must fail";
    } catch (const ApplicationError& error) {
        EXPECT_EQ(error.what(), std::string(media::xai_completed_early));
    }
}

TEST(XaiVoice, ReportsMalformedMessagesMissingTimingsAndEarlyClose) {
    OwnedApp owned;
    auto script = install_script(*owned.application);
    save_xai(*owned.application, "ws://127.0.0.1:9/v1/stt");
    const auto epoch = owned.application->context_epoch();
    push_event(*script, created_event());
    (void)wait_reply(owned.application->start_xai_voice_input(
        "view-1", 1, "dictation-1", {"en"}, epoch, 30000ms));
    push_text(*script, "not-json");
    wait_closed(*script);
    try {
        (void)wait_reply(owned.application->send_xai_voice_audio(
            "view-1", 2, "dictation-1", "AAE=", epoch, 30000ms));
        FAIL() << "malformed JSON must fail";
    } catch (const ApplicationError& error) {
        EXPECT_EQ(error.what(), std::string(media::xai_malformed_transcript));
    }

    script->closed = false;
    push_event(*script, created_event());
    (void)wait_reply(owned.application->start_xai_voice_input(
        "view-1", 3, "dictation-2", {"en"}, epoch, 30000ms));
    push_event(*script, nlohmann::json{
        {"type", "transcript.partial"},
        {"is_final", true},
        {"speech_final", true},
        {"text", "Hello"},
    });
    wait_closed(*script);
    try {
        (void)wait_reply(owned.application->stop_xai_voice_input(
            "view-1", 4, "dictation-2", 5000, epoch, 30000ms));
        FAIL() << "missing timings must fail";
    } catch (const ApplicationError& error) {
        EXPECT_EQ(error.what(), std::string(media::xai_missing_word_timings));
    }

    script->closed = false;
    push_event(*script, created_event());
    (void)wait_reply(owned.application->start_xai_voice_input(
        "view-1", 5, "dictation-3", {"en"}, epoch, 30000ms));
    {
        std::lock_guard lock(script->mu);
        script->incoming.push_back(media::XaiIncoming{.closed = true});
    }
    script->cv.notify_all();
    wait_closed(*script);
    try {
        (void)wait_reply(owned.application->stop_xai_voice_input(
            "view-1", 6, "dictation-3", 5000, epoch, 30000ms));
        FAIL() << "early close must fail";
    } catch (const ApplicationError& error) {
        EXPECT_EQ(error.what(), std::string(media::xai_connection_closed));
    }
}

TEST(XaiVoice, ShutdownAndConnectionCloseReleaseTheWorker) {
    OwnedApp owned;
    auto script = install_script(*owned.application);
    save_xai(*owned.application, "ws://127.0.0.1:9/v1/stt");
    const auto started = owned.application->start_xai_voice_input(
        "view-1", 1, "dictation-1", {"en"}, owned.application->context_epoch(), 30000ms);
    owned.application->release_connection_resources("view-1");
    try {
        (void)wait_reply(started);
        FAIL() << "connection close must cancel startup";
    } catch (const ApplicationError& error) {
        EXPECT_EQ(error.code, ErrorCode::operation_cancelled);
    }
    wait_closed(*script);

    script->closed = false;
    script->connected = false;
    push_event(*script, created_event());
    const auto active = owned.application->start_xai_voice_input(
        "view-2", 2, "dictation-2", {"en"}, owned.application->context_epoch(), 30000ms);
    (void)wait_reply(active);
    owned.application->request_shutdown();
    EXPECT_TRUE(owned.application->join_shutdown(2s));
    owned.application.reset();
}

TEST(XaiVoice, RejectsBadAudioWithoutSendingIt) {
    OwnedApp owned;
    auto script = install_script(*owned.application);
    save_xai(*owned.application, "ws://127.0.0.1:9/v1/stt");
    const auto epoch = owned.application->context_epoch();
    push_event(*script, created_event());
    (void)wait_reply(owned.application->start_xai_voice_input(
        "view-1", 1, "dictation-1", {"en"}, epoch, 30000ms));
    try {
        (void)owned.application->send_xai_voice_audio(
            "view-1", 2, "dictation-1", "AA==", epoch, 30000ms);
        FAIL() << "odd PCM must be rejected";
    } catch (const ApplicationError& error) {
        EXPECT_EQ(error.code, ErrorCode::invalid_argument);
    }
    wait_closed(*script);
    EXPECT_TRUE(script->binary.empty());
}

TEST(XaiCurlSocket, HandshakesSendsOrderedAudioAndReassemblesFrames) {
    nlohmann::json partial = final_event("Hello", 0.1, 0.3);
    XaiFakeServer server({
        .messages = {created_event().dump(), partial.dump()},
        .after_audio_done = {nlohmann::json{
            {"type", "transcript.done"}, {"text", ""}, {"words", nlohmann::json::array()},
        }.dump()},
        .fragment_first = true,
        .byte_writes = true,
        .send_ping = true,
    });
    server.start();
    OwnedApp owned;
    save_xai(
        *owned.application,
        "ws://127.0.0.1:" + std::to_string(server.port()) + "/v1/stt?dropped=1",
        "socket-secret");
    const auto epoch = owned.application->context_epoch();
    const auto started = owned.application->start_xai_voice_input(
        "view-1", 1, "dictation-1", {"en"}, epoch, 30000ms);
    const auto ready = wait_reply(started, 3s);
    EXPECT_EQ(ready["session_id"], "dictation-1");
    const auto request_line = server.request().substr(0, server.request().find("\r\n"));
    EXPECT_EQ(request_line.find("socket-secret"), std::string::npos);
    EXPECT_NE(server.request().find("Authorization: Bearer socket-secret"), std::string::npos);
    EXPECT_EQ(server.request().find("dropped"), std::string::npos);
    EXPECT_NE(server.request().find("language=en"), std::string::npos);
    EXPECT_NE(server.request().find("endpointing=400"), std::string::npos);

    const auto audio = owned.application->send_xai_voice_audio(
        "view-1", 2, "dictation-1",
        encode_base64(std::vector<unsigned char>(3200, 7)), epoch, 30000ms);
    const auto pieces = wait_reply(audio, 3s);
    EXPECT_EQ(pieces["pieces"], nlohmann::json::array({"Hello"}));
    const auto stopped = owned.application->stop_xai_voice_input(
        "view-1", 3, "dictation-1", 20000, epoch, 30000ms);
    EXPECT_EQ(wait_reply(stopped, 3s)["pieces"], nlohmann::json::array());
    server.join();
    EXPECT_TRUE(server.saw_pong());
    const auto events = server.events();
    ASSERT_GE(events.size(), 2U);
    EXPECT_EQ(events[0], "b:3200");
    EXPECT_EQ(events[1], "t:{\"type\":\"audio.done\"}");
}

TEST(XaiCurlSocket, AuthenticationFailureDoesNotExposeTheKey) {
    XaiFakeServer server({.http_status = 401});
    server.start();
    OwnedApp owned;
    save_xai(
        *owned.application,
        "ws://127.0.0.1:" + std::to_string(server.port()) + "/v1/stt");
    const auto started = owned.application->start_xai_voice_input(
        "view-1", 1, "dictation-1", {"en"}, owned.application->context_epoch(), 30000ms);
    try {
        (void)wait_reply(started, 3s);
        FAIL() << "HTTP 401 must fail";
    } catch (const ApplicationError& error) {
        EXPECT_EQ(error.what(), std::string(media::xai_authentication_failed));
        EXPECT_EQ(std::string(error.what()).find("xai-secret"), std::string::npos);
    }
    server.join();
    EXPECT_NE(server.request().find("Authorization: Bearer xai-secret"), std::string::npos);
}

TEST(XaiCurlSocket, AcceptKeyMatchesTheWebSocketExample) {
    EXPECT_EQ(
        xai_fake_websocket_accept("dGhlIHNhbXBsZSBub25jZQ=="),
        "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

TEST(XaiBridge, CancelQueuedAheadOfStartRejectsThatSession) {
    OwnedApp owned;
    auto script = install_script(*owned.application);
    save_xai(*owned.application, "ws://127.0.0.1:9/v1/stt");
    bridge::BridgeRouter router(*owned.application);
    const auto connection = router.open_connection();
    const auto epoch = owned.application->context_epoch();
    auto request = [&](std::uint64_t id, std::string method, nlohmann::json params) {
        router.handle_request(connection, nlohmann::json{
            {"connection_id", connection},
            {"id", id},
            {"context_epoch", epoch},
            {"method", method},
            {"params", std::move(params)},
        }.dump());
    };
    request(1, "voiceInput.xai.start", {{"session_id", "dictation-1"}, {"languages", {"en"}}});
    request(2, "voiceInput.xai.cancel", {{"session_id", "dictation-1"}});
    router.run_tasks();
    std::optional<nlohmann::json> batch;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline && !batch) {
        router.run_tasks();
        batch = router.take_delivery(connection);
        if (!batch) router.wait_for_work(5ms);
    }
    ASSERT_TRUE(batch);
    bool saw_cancel = false;
    bool saw_start = false;
    for (const auto& message : batch->at("messages")) {
        if (message.at("id") == 2) {
            saw_cancel = true;
            EXPECT_TRUE(message.at("ok"));
        }
        if (message.at("id") == 1) {
            saw_start = true;
            EXPECT_FALSE(message.at("ok"));
            EXPECT_EQ(message.at("error").at("code"), "operation_cancelled");
        }
    }
    EXPECT_TRUE(saw_cancel);
    EXPECT_TRUE(saw_start);
    EXPECT_FALSE(script->connected);
}

} // namespace
} // namespace cha::app
