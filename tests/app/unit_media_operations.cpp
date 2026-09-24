#include "app/application.h"

#include "support/mock_http_server.h"
#include "support/test_workspace.h"
#include "workspace/builtins.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <thread>

namespace cha::app {
namespace {

using namespace std::chrono_literals;

ApplicationCommand make_command(
    const test::TestWorkspace& workspace,
    const std::filesystem::path& database) {
    const std::filesystem::path config_directory =
        workspace.root() / "cha-config";
    std::filesystem::create_directories(config_directory);
    {
        std::ofstream app(config_directory / "app.toml");
        app << "vault = \"Test\"\n"
            << "[logging]\nfile = \"runtime.log\"\nlevel = \"off\"\n";
    }
    {
        std::ofstream vault(config_directory / "test.toml");
        vault << "vault_name = \"Test\"\n"
              << "data = " << std::quoted(database.string()) << "\n";
    }
    const ConfigurationDirectory loaded = load_configuration_directory(
        config_directory);
    const VaultDefinition* const vault =
        find_vault(loaded.vaults, loaded.startup_vault);
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
            if (const auto* failure =
                    std::get_if<OperationReply::Failure>(&*peeked)) {
                throw ApplicationError(failure->code, failure->message);
            }
            return std::get<nlohmann::json>(*peeked);
        }
        std::this_thread::sleep_for(2ms);
    }
    throw std::runtime_error("Timed out waiting for media reply");
}

std::optional<ResourceBytes> wait_resource(Application& application, const std::string& id) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto bytes = application.read_resource("view-1", id)) return bytes;
        std::this_thread::sleep_for(2ms);
    }
    return std::nullopt;
}

TEST(ApplicationMedia, SynthesizesSpeechIntoARevocableResource) {
    MockHttpServer server({http_response("audio/mpeg", "AUDIO")});
    server.start();
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    auto application = Application::open(make_command(workspace, database));
    const auto epoch = application->context_epoch();
    const auto key = application->create_api_key(
        {.display_name = "Fish", .value = "fish-secret"}, epoch);
    const auto voice = application->create_voice({
        .display_name = "Narrator",
        .description = "Test",
        .elevenlabs_voice_id = "voice-ref",
    }, epoch);
    (void)application->save_voice_output_settings({
        .url = "https://api.fish.audio/v1/tts",
        .model = "s2.1-pro",
        .api_key = key.id,
        .output_format = "mp3",
        .default_voice = voice.display_name,
    }, epoch);
    application->set_speech_url_override(
        "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/tts");

    const auto reply = application->start_speech(
        "view-1", 7, "Hello",
        {.reference_id = "voice-ref"}, 1);
    const auto result = wait_reply(reply);
    EXPECT_EQ(result["url"], "/media/" + result["resource_id"].get<std::string>());
    EXPECT_EQ(result["mime_type"], "audio/mpeg");
    EXPECT_EQ(result["byte_length"], 0);
    EXPECT_EQ(result["streaming"], true);

    const auto body = wait_resource(*application, result["resource_id"].get<std::string>());
    ASSERT_TRUE(body);
    EXPECT_EQ(body->body, "AUDIO");
    EXPECT_FALSE(application->read_resource(
        "view-2", result["resource_id"].get<std::string>()));

    application->release_resource(
        "view-1", result["resource_id"].get<std::string>(), 1);
    EXPECT_FALSE(application->read_resource(
        "view-1", result["resource_id"].get<std::string>()));

    application->request_shutdown();
    (void)application->join_shutdown(2s);
    server.join();
}

TEST(ApplicationMedia, LateCancellationRevokesACompletedSpeechResource) {
    MockHttpServer server({
        http_response("audio/mpeg", "AUDIO"),
        http_response("audio/mpeg", "AUDIO"),
    });
    server.start();
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    auto application = Application::open(make_command(workspace, database));
    const auto epoch = application->context_epoch();
    const auto key = application->create_api_key(
        {.display_name = "Fish", .value = "fish-secret"}, epoch);
    const auto voice = application->create_voice({
        .display_name = "Narrator",
        .description = "Test",
        .elevenlabs_voice_id = "voice-ref",
    }, epoch);
    (void)application->save_voice_output_settings({
        .url = "https://api.fish.audio/v1/tts",
        .model = "s2.1-pro",
        .api_key = key.id,
        .output_format = "mp3",
        .default_voice = voice.display_name,
    }, epoch);
    application->set_speech_url_override(
        "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/tts");

    const auto reply = application->start_speech(
        "view-1", 7, "Hello",
        {.reference_id = "voice-ref"}, 1);
    const auto result = wait_reply(reply);
    EXPECT_EQ(result["url"], "/media/" + result["resource_id"].get<std::string>());
    EXPECT_EQ(result["mime_type"], "audio/mpeg");
    EXPECT_EQ(result["byte_length"], 0);
    EXPECT_EQ(result["streaming"], true);

    const auto body = wait_resource(*application, result["resource_id"].get<std::string>());
    ASSERT_TRUE(body);
    EXPECT_EQ(body->body, "AUDIO");
    EXPECT_FALSE(application->read_resource(
        "view-2", result["resource_id"].get<std::string>()));

    // Join/reap the completed worker before cancellation, without closing its document.
    std::this_thread::sleep_for(20ms);
    EXPECT_THROW(application->cancel_speech("view-1", 7, 0), ApplicationError);
    EXPECT_TRUE(application->read_resource(
        "view-1", result["resource_id"].get<std::string>()));
    application->cancel_speech("view-1", 7, 1);
    EXPECT_FALSE(application->read_resource(
        "view-1", result["resource_id"].get<std::string>()));

    const auto abandoned = wait_reply(application->start_speech(
        "view-1", 8, "Hello", {.reference_id = "voice-ref"}, 1));
    const auto resource_id = abandoned["resource_id"].get<std::string>();
    ASSERT_TRUE(wait_resource(*application, resource_id));
    application->release_request_resources("view-2", 8);
    application->release_request_resources("view-1", 7);
    EXPECT_TRUE(application->read_resource("view-1", resource_id));
    application->release_request_resources("view-1", 8);
    EXPECT_FALSE(application->read_resource("view-1", resource_id));

    application->request_shutdown();
    (void)application->join_shutdown(2s);
    EXPECT_NO_THROW(application->release_request_resources("view-1", 8));
    server.join();
}

TEST(ApplicationMedia, CancelledSpeechDoesNotRegisterAResource) {
    MockHttpServer server({http_response("audio/mpeg", "AUDIO")});
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    auto application = Application::open(make_command(workspace, database));
    const auto epoch = application->context_epoch();
    const auto key = application->create_api_key(
        {.display_name = "Fish", .value = "fish-secret"}, epoch);
    const auto voice = application->create_voice({
        .display_name = "Narrator",
        .description = "Test",
        .elevenlabs_voice_id = "voice-ref",
    }, epoch);
    (void)application->save_voice_output_settings({
        .url = "https://api.fish.audio/v1/tts",
        .model = "s2.1-pro",
        .api_key = key.id,
        .output_format = "mp3",
        .default_voice = voice.display_name,
    }, epoch);
    application->set_speech_url_override(
        "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/tts");

    const auto reply = application->start_speech(
        "view-1", 3, "Hello",
        {.reference_id = "voice-ref"}, 1);
    std::this_thread::sleep_for(20ms);
    application->cancel_speech("view-1", 3, 1);
    try {
        (void)wait_reply(reply, 2s);
        FAIL() << "cancelled speech should not complete";
    } catch (const ApplicationError& error) {
        EXPECT_EQ(error.code, ErrorCode::operation_cancelled);
    }

    application->request_shutdown();
    (void)application->join_shutdown(2s);
}

TEST(ApplicationMedia, ReturnsPlayableBytesBeforeUpstreamFinishesAndReleaseCancelsIt) {
    // No Content-Length: the response stays open until CHA cancels the transfer.
    MockHttpServer server({"HTTP/1.1 200 OK\r\nContent-Type: audio/mpeg\r\nConnection: close\r\n\r\nFIRST"}, true);
    server.start();
    test::TestWorkspace workspace;
    const auto database = test::import_test_database(workspace.root());
    auto application = Application::open(make_command(workspace, database));
    const auto epoch = application->context_epoch();
    const auto key = application->create_api_key({.display_name = "Fish", .value = "secret"}, epoch);
    const auto voice = application->create_voice({
        .display_name = "Narrator", .description = "Test", .elevenlabs_voice_id = "ref",
    }, epoch);
    (void)application->save_voice_output_settings({
        .url = "https://api.fish.audio/v1/tts", .model = "s2.1-pro", .api_key = key.id,
        .output_format = "mp3", .default_voice = voice.display_name,
    }, epoch);
    application->set_speech_url_override("http://127.0.0.1:" + std::to_string(server.port()) + "/v1/tts");
    const auto result = wait_reply(application->start_speech("view", 1, "Hello", {}, epoch));
    ASSERT_TRUE(result["streaming"]);
    const auto id = result["resource_id"].get<std::string>();
    const auto chunk = application->read_resource_chunk("view", id, 0);
    ASSERT_TRUE(chunk);
    EXPECT_EQ(chunk->body, "FIRST");
    EXPECT_FALSE(chunk->complete);
    EXPECT_FALSE(chunk->failed);
    EXPECT_FALSE(application->read_resource("view", id));
    EXPECT_FALSE(application->read_resource_chunk("other", id, 0));
    application->release_resource("view", id, epoch);
    EXPECT_FALSE(application->read_resource_chunk("view", id, 0));
    server.join(); // Requires release to cancel the still-open provider request.
    application->request_shutdown();
    EXPECT_TRUE(application->join_shutdown(2s));
}

TEST(ApplicationMedia, ConnectsVoiceInputWithoutExposingTheStoredKey) {
    MockHttpServer server({http_response("application/sdp", "v=0 answer")});
    server.start();
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    auto application = Application::open(make_command(workspace, database));
    const auto epoch = application->context_epoch();
    const auto key = application->create_api_key(
        {.display_name = "Realtime", .value = "voice-secret"}, epoch);
    (void)application->save_voice_input_settings({
        .provider = "openai",
        .url = "http://127.0.0.1:" + std::to_string(server.port()) + "/v1/realtime",
        .model = "gpt-4o-transcribe",
        .api_key = key.id,
        .delay = "low",
        .prompt = "",
    }, epoch);
    const auto runtime = application->get_voice_input_runtime(epoch);
    ASSERT_TRUE(runtime);
    EXPECT_FALSE(nlohmann::json(*runtime).contains("api_key"));
    EXPECT_EQ(nlohmann::json(*runtime).dump().find("voice-secret"),
        std::string::npos);

    const auto reply = application->connect_voice_input(
        "view-1", 4, "v=0 offer", {"en"}, 1);
    const auto result = wait_reply(reply);
    EXPECT_EQ(result["sdp"], "v=0 answer");
    ASSERT_FALSE(server.requests().empty());
    EXPECT_NE(server.requests().front().find("Bearer voice-secret"), std::string::npos);
    EXPECT_NE(server.requests().front().find("v=0 offer"), std::string::npos);

    application->request_shutdown();
    (void)application->join_shutdown(2s);
    server.join();
}

TEST(ApplicationMedia, RejectsXaiBeforeTheOpenAiRequest) {
    MockHttpServer server({http_response("application/sdp", "v=0 answer")});
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    auto application = Application::open(make_command(workspace, database));
    const auto epoch = application->context_epoch();
    const auto key = application->create_api_key(
        {.display_name = "xAI", .value = "xai-secret"}, epoch);
    (void)application->save_voice_input_settings({
        .provider = "xai",
        .url = "ws://127.0.0.1:" + std::to_string(server.port()) + "/v1/stt",
        .model = "grok-voice-transcribe-2.0",
        .api_key = key.id,
        .delay = "low",
        .prompt = "do not send",
    }, epoch);

    const auto started = std::chrono::steady_clock::now();
    try {
        (void)application->connect_voice_input(
            "view-1", 4, "v=0 offer", {"en"}, epoch);
        FAIL() << "xAI must not use the OpenAI connection";
    } catch (const ApplicationError& error) {
        EXPECT_EQ(error.code, ErrorCode::invalid_argument);
        EXPECT_EQ(error.what(), std::string("Voice input provider is not OpenAI."));
    }
    EXPECT_LT(std::chrono::steady_clock::now() - started, 500ms);
    EXPECT_TRUE(server.requests().empty());

    application->request_shutdown();
    (void)application->join_shutdown(2s);
}

TEST(ApplicationMedia, RejectsUnknownAudioSourcesAndClearsConnectionResources) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    auto application = Application::open(make_command(workspace, database));
    try {
        (void)application->audio_source(
            "view-1", "lobby", "welcome", 1, "Test", 1);
        FAIL() << "missing cached audio should fail";
    } catch (const ApplicationError& error) {
        EXPECT_EQ(error.code, ErrorCode::not_found);
    }
    const auto status = application->audio_status("lobby", "welcome", "Test", 1);
    EXPECT_TRUE(status.downloads.empty());
    application->release_connection_resources("view-1");
    application->request_shutdown();
    (void)application->join_shutdown(2s);
}

} // namespace
} // namespace cha::app
