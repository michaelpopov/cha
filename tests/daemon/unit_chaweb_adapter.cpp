#include "app/application.h"
#include "app/current_vault.h"
#include "daemon/chaweb_adapter.h"
#include "daemon/scgi.h"
#include "runtime/runtime_settings.h"
#include "support/mock_http_server.h"
#include "support/test_workspace.h"
#include "support/xai_fake_server.h"
#include "storage/session_database.h"
#include "storage/session_repository.h"
#include "util/logging.h"
#include "workspace/builtins.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace cha::daemon {
namespace {

using app::Application;
using namespace std::chrono_literals;

constexpr std::string_view json_type = "application/json";
constexpr std::string_view bootstrap_path = "/api/cha/v1/bootstrap";

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
    const ConfigurationDirectory loaded =
        load_configuration_directory(config_directory);
    const VaultDefinition* const vault =
        find_vault(loaded.vaults, loaded.startup_vault);
    return {
        .config_directory = loaded.directory,
        .vaults = loaded.vaults,
        .vault = *vault,
        .log_file = loaded.log_file,
        .log_level = loaded.log_level,
        .warnings = loaded.warnings,
    };
}

void disable_naming(const test::TestWorkspace& workspace) {
    const auto directory = workspace.root() / "system/session";
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "config.toml")
        << "naming_provider = \"test\"\n";
}

void use_net_provider(
    const test::TestWorkspace& workspace, int port) {
    workspace.write_provider(
        "remote",
        "host = \"127.0.0.1\"\nport = " + std::to_string(port)
            + "\nhttps = false\nmode = \"net\"\nmodel = \"fake\"\n"
              "api = \"chat_completions\"\nstream = false\ntimeout_s = 20\n");
    workspace.write_character_config(
        "display_name = \"Guide\"\nprovider = \"remote\"\n");
    std::ofstream(workspace.root() / "system" / "assistant" / "character.toml")
        << "display_name = \"Assistant\"\nprovider = \"remote\"\n";
}

std::string http_json(std::string_view body) {
    return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
        + std::to_string(body.size())
        + "\r\nConnection: close\r\n\r\n" + std::string(body);
}

std::string sessions_path(std::string_view forum) {
    return "/api/cha/v1/forums/" + std::string(forum) + "/sessions";
}

std::string session_path(std::string_view forum, std::string_view id) {
    return sessions_path(forum) + "/" + std::string(id);
}

nlohmann::json text_body(std::string_view text) {
    return {{"text", text}};
}

struct CgiResponse {
    int status{};
    std::string raw;
    nlohmann::json json;
};

CgiResponse exchange(
    Application& application,
    const ScgiRequest& request,
    ChaWebDeleteSession delete_session = nullptr,
    ChaWebSubmitInput submit_input = nullptr) {
    int fds[2]{};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        throw std::runtime_error("socketpair failed");
    }
    UniqueFd server(fds[0]);
    UniqueFd client(fds[1]);
    std::atomic<bool> stop{false};
    handle_chaweb_request(
        application, request, server.get(), stop, delete_session, submit_input);
    server.close();
    CgiResponse response;
    char buffer[4096];
    while (true) {
        const ssize_t count = ::recv(client.get(), buffer, sizeof(buffer), 0);
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            break;
        }
        response.raw.append(buffer, static_cast<std::size_t>(count));
    }
    if (response.raw.starts_with("Status: ")) {
        response.status = std::stoi(response.raw.substr(8));
    }
    const auto body_at = response.raw.find("\r\n\r\n");
    if (body_at != std::string::npos) {
        const std::string body = response.raw.substr(body_at + 4);
        if (!body.empty() && response.raw.find("Content-Type: application/json") != std::string::npos) {
            response.json = nlohmann::json::parse(body);
        }
    }
    return response;
}

ScgiRequest get_request(std::string uri) {
    return {.method = "GET", .document_uri = std::move(uri)};
}

ScgiRequest post_request(
    std::string uri,
    std::string body,
    std::string type = std::string(json_type)) {
    return {
        .method = "POST",
        .document_uri = std::move(uri),
        .body = std::move(body),
        .content_type = std::move(type),
    };
}

class ChaWebAdapterTest : public testing::Test {
protected:
    void SetUp() override { open_application(); }

    void open_application(RuntimeSettings settings = {}) {
        database_ = test::import_test_database(workspace_.root());
        application_ = Application::open(
            make_command(workspace_, database_), {}, std::move(settings));
    }

    CgiResponse get(std::string uri) {
        return exchange(*application_, get_request(std::move(uri)));
    }

    CgiResponse post(
        std::string uri,
        const nlohmann::json& body,
        std::string type = std::string(json_type),
        ChaWebDeleteSession delete_session = nullptr) {
        return exchange(
            *application_,
            post_request(std::move(uri), body.dump(), std::move(type)),
            delete_session);
    }

    std::string create_session(std::string_view text = "@- note") {
        const CgiResponse created = post(
            sessions_path("lobby"), text_body(text));
        EXPECT_EQ(created.status, 201) << created.raw;
        return created.json.at("id").get<std::string>();
    }

    std::vector<SessionListing> listed(std::string_view forum = "lobby") {
        return application_->list_sessions(
            forum, application_->context_epoch());
    }

    std::string add_audio_entry(EntryStatus status = EntryStatus::complete, bool cached = false, EntryId count = 1) {
        application_.reset();
        std::string session_id;
        {
            auto config = WorkspaceConfigStore::open(database_);
            SessionRepository repository([&] { return config->snapshot(); }, database_,
                config->workspace_path(), config->welcome_path(),
                TemporarySessionSeed{{"temporary", "welcome"}, "Welcome"});
            const auto session = repository.create("lobby", "Audio").identity;
            session_id = session.session_id;
            const auto prepared = repository.prepare(session);
            SessionJournal journal(database_, prepared.session_key);
            for (EntryId id = 1; id <= count; ++id) {
                journal.record_entry(make_character_entry(id, "guide", "Guide", "Hello", status));
                if (cached) {
                    const auto entry = repository.lookup_entry_audio(session, id, false);
                    if (!entry) throw std::runtime_error("Entry was not saved");
                    repository.save_entry_audio(*entry, {std::string("audio\0bytes", 11), "audio/mpeg"});
                }
            }
        }
        application_ = Application::open(make_command(workspace_, database_));
        return session_id;
    }

    test::TestWorkspace workspace_;
    std::filesystem::path database_;
    std::unique_ptr<Application> application_;
};

TEST_F(ChaWebAdapterTest, BootstrapUsesExistingSerializer) {
    const auto boot = application_->bootstrap();
    const CgiResponse response = get(std::string(bootstrap_path));
    EXPECT_EQ(response.status, 200);
    nlohmann::json expected = boot.presentation;
    expected["vault_parent"] = nullptr;
    expected["capabilities"] = {{"can_modify", false}, {"can_transfer_r2", false}};
    EXPECT_EQ(response.json, expected);
    EXPECT_EQ(response.json.at("entrance_forum_id"), std::string(entrance_id));
    EXPECT_FALSE(response.json.contains("state"));
    EXPECT_FALSE(response.json.contains("context_epoch"));
    EXPECT_NE(response.raw.find("Content-Type: application/json"), std::string::npos);
}

TEST_F(ChaWebAdapterTest, VoiceInputUsesStoredSettingsAndKeepsOpenAiCredentialsOnTheServer) {
    EXPECT_TRUE(get("/api/cha/v1/voice-input").json.is_null());
    const std::string answer = "test SDP answer";
    MockHttpServer provider({
        "HTTP/1.1 200 OK\r\nContent-Type: application/sdp\r\nContent-Length: "
        + std::to_string(answer.size()) + "\r\nConnection: close\r\n\r\n" + answer});
    provider.start();
    const auto epoch = application_->context_epoch();
    const auto key = application_->create_api_key(
        {.display_name = "STT", .value = "stt-secret"}, epoch);
    (void)application_->save_voice_input_settings({
        .provider = "openai",
        .url = "http://127.0.0.1:" + std::to_string(provider.port()) + "/realtime",
        .model = "test-transcription",
        .api_key = key.id,
        .delay = "low",
        .prompt = "software",
        .send_phrase = "over to you",
    }, epoch);
    const auto runtime = get("/api/cha/v1/voice-input");
    ASSERT_EQ(runtime.status, 200);
    EXPECT_EQ(runtime.json["provider"], "openai");
    EXPECT_EQ(runtime.json["send_phrase"], "over to you");
    EXPECT_EQ(runtime.raw.find("stt-secret"), std::string::npos);
    EXPECT_FALSE(runtime.json.contains("api_key"));
    const auto connected = post("/api/cha/v1/voice-input/connect",
        nlohmann::json({{"sdp", "browser offer"}, {"languages", {"en"}}}));
    ASSERT_EQ(connected.status, 200);
    EXPECT_EQ(connected.json["sdp"], answer);
    EXPECT_EQ(connected.raw.find("stt-secret"), std::string::npos);
    provider.join();
    ASSERT_EQ(provider.requests().size(), 1U);
    EXPECT_NE(provider.requests().front().find("Authorization: Bearer stt-secret"), std::string::npos);
    EXPECT_NE(provider.requests().front().find("browser offer"), std::string::npos);
    EXPECT_NE(provider.requests().front().find("test-transcription"), std::string::npos);
}

TEST_F(ChaWebAdapterTest, XaiDictationSurvivesSeparateHttpRequestsAndFlushesFinalWords) {
    XaiFakeServer provider({
        .messages = {R"({"type":"transcript.created"})"},
        .after_audio_done = {
            R"({"type":"transcript.partial","is_final":true,"speech_final":true,"text":"Hello","words":[{"text":"Hello","start":0,"end":0.5}]})",
            R"({"type":"transcript.done"})",
        },
    });
    provider.start();
    const auto epoch = application_->context_epoch();
    const auto key = application_->create_api_key(
        {.display_name = "STT", .value = "xai-secret"}, epoch);
    (void)application_->save_voice_input_settings({
        .provider = "xai",
        .url = "ws://127.0.0.1:" + std::to_string(provider.port()) + "/v1/stt",
        .model = "test-transcription",
        .api_key = key.id,
        .delay = "low",
    }, epoch);
    const auto started = post("/api/cha/v1/voice-input/xai/start",
        nlohmann::json({{"session_id", "dictation-1"}, {"languages", {"en"}}}));
    ASSERT_EQ(started.status, 200);
    EXPECT_EQ(started.json["session_id"], "dictation-1");
    const auto audio = post("/api/cha/v1/voice-input/xai/audio",
        nlohmann::json({{"session_id", "dictation-1"},
            {"pcm_base64", xai_fake_base64(std::vector<unsigned char>(3200, 0))}}));
    ASSERT_EQ(audio.status, 200);
    EXPECT_EQ(audio.json["pieces"], nlohmann::json::array());
    const auto stopped = post("/api/cha/v1/voice-input/xai/stop",
        nlohmann::json({{"session_id", "dictation-1"}, {"remaining_ms", 20000}}));
    ASSERT_EQ(stopped.status, 200);
    EXPECT_EQ(stopped.json["pieces"], nlohmann::json::array({"Hello"}));
    EXPECT_EQ(stopped.raw.find("xai-secret"), std::string::npos);
    provider.join();
    ASSERT_EQ(provider.binary_messages().size(), 1U);
    EXPECT_EQ(provider.binary_messages().front().size(), 3200U);
    EXPECT_NE(provider.request().find("Authorization: Bearer xai-secret"), std::string::npos);
    EXPECT_EQ(post("/api/cha/v1/voice-input/xai/cancel",
        nlohmann::json::parse(R"({"session_id":"dictation-1"})")).status, 200);
}

TEST_F(ChaWebAdapterTest, VoiceRequestsRejectInvalidBodiesAndDoNotAcceptBrowserCredentials) {
    EXPECT_EQ(post("/api/cha/v1/voice-input/connect",
        nlohmann::json::parse(R"({"sdp":"offer","languages":[],"api_key":"secret"})")).status, 400);
    EXPECT_EQ(post("/api/cha/v1/voice-input/xai/start",
        nlohmann::json::parse(R"({"session_id":"dictation","languages":[4]})")).status, 400);
    EXPECT_EQ(post("/api/cha/v1/voice-input/xai/audio",
        nlohmann::json::parse(R"({"session_id":"../dictation","pcm_base64":"AAAA"})")).status, 400);
    EXPECT_EQ(post("/api/cha/v1/voice-input/xai/stop",
        nlohmann::json::parse(R"({"session_id":"dictation","remaining_ms":"later"})")).status, 400);
    EXPECT_EQ(post("/api/cha/v1/voice-input/xai/cancel", nlohmann::json::array()).status, 400);
    EXPECT_EQ(post("/api/cha/v1/voice-input/connect", nlohmann::json::object(), "text/plain").status, 415);
    EXPECT_EQ(get("/api/cha/v1/voice-input/xai/start").status, 404);
}

TEST_F(ChaWebAdapterTest, CachedAudioIsAvailableWithoutVoiceConfiguration) {
    EXPECT_TRUE(get("/api/cha/v1/voice-output").json.is_null());
    const auto path = session_path("lobby", add_audio_entry(EntryStatus::complete, true));
    const auto source = path + "/entries/1/audio";
    const auto accepted = post(source, {{"vault_name", "Test"}});
    ASSERT_EQ(accepted.status, 200) << accepted.raw;
    EXPECT_EQ(accepted.json, (nlohmann::json{{"entry_id", 1}, {"cached", true}}));
    EXPECT_EQ(post(source, {{"vault_name", "Test"}}).json, accepted.json);
    const auto status = get(path + "/audio");
    ASSERT_EQ(status.status, 200) << status.raw;
    EXPECT_EQ(status.json.at("cached_entry_ids"), nlohmann::json::array({1}));
    EXPECT_TRUE(status.json.at("downloads").empty());
    EXPECT_TRUE(get(path).json.at("transcript").at(0).at("has_cached_audio").get<bool>());
    const auto audio = get(source);
    ASSERT_EQ(audio.status, 200) << audio.raw;
    EXPECT_NE(audio.raw.find("Content-Type: audio/mpeg"), std::string::npos);
    EXPECT_EQ(audio.raw.substr(audio.raw.find("\r\n\r\n") + 4), std::string("audio\0bytes", 11));
    EXPECT_EQ(post(path + "/stop", nlohmann::json::object()).status, 204);
}

TEST_F(ChaWebAdapterTest, ReadsAudioChunksAndValidatesOffsets) {
    const auto path = session_path("lobby", add_audio_entry(EntryStatus::complete, true));
    const auto source = path + "/entries/1/audio";
    auto read_chunk = [&](std::string offset) {
        auto request = get_request(source);
        request.audio_offset = std::move(offset);
        return exchange(*application_, request);
    };
    const auto first = read_chunk("0");
    ASSERT_EQ(first.status, 200);
    EXPECT_NE(first.raw.find("X-CHA-Audio-Complete: 1\r\n"), std::string::npos);
    EXPECT_EQ(first.raw.substr(first.raw.find("\r\n\r\n") + 4), std::string("audio\0bytes", 11));
    const auto tail = read_chunk("5");
    EXPECT_EQ(tail.raw.substr(tail.raw.find("\r\n\r\n") + 4), std::string("\0bytes", 6));
    const auto end = read_chunk("11");
    EXPECT_EQ(end.status, 200);
    EXPECT_NE(end.raw.find("X-CHA-Audio-Complete: 1\r\n"), std::string::npos);
    EXPECT_TRUE(end.raw.substr(end.raw.find("\r\n\r\n") + 4).empty());
    for (const auto* offset : {"12", "-1", "1x", " 1", "18446744073709551616"}) {
        EXPECT_EQ(read_chunk(offset).status, 400) << offset;
    }
}

TEST_F(ChaWebAdapterTest, AdmitsAudioBatchesAndRejectsInvalidRequestsWithoutChangingCachedAudio) {
    const auto path = session_path("lobby", add_audio_entry(EntryStatus::complete, true, 2)) + "/audio";
    const auto accepted = post(path, {{"vault_name", "Test"}, {"entry_ids", {2, 1}}});
    ASSERT_EQ(accepted.status, 200);
    EXPECT_EQ(accepted.json, (nlohmann::json{{"entries", {
        {{"entry_id", 2}, {"cached", true}}, {{"entry_id", 1}, {"cached", true}}}}}));
    EXPECT_EQ(post(path, {{"vault_name", "Other"}, {"entry_ids", {1, 2}}}).status, 409);
    EXPECT_EQ(post(path, {{"vault_name", "Test"}, {"entry_ids", {1, 3}}}).status, 404);
    for (const auto& ids : std::vector<nlohmann::json>{
             nlohmann::json::array(), {1, 1}, {0}, {-1}, {1.5}, {"1"}, {9007199254740992ULL}, 1}) {
        EXPECT_EQ(post(path, {{"vault_name", "Test"}, {"entry_ids", ids}}).status, 400) << ids;
    }
    EXPECT_EQ(post(path, {{"vault_name", "Test"}}).status, 400);
    EXPECT_EQ(post(path, {{"vault_name", "Test"}, {"entry_ids", {1}}, {"extra", true}}).status, 400);
    auto request = post_request(path, "{\"vault_name\":\"Test\",\"entry_ids\":[1]}", "text/plain");
    EXPECT_EQ(exchange(*application_, request).status, 415);
    const auto status = get(path);
    EXPECT_EQ(status.json.at("cached_entry_ids"), nlohmann::json::array({1, 2}));
    EXPECT_TRUE(status.json.at("downloads").empty());
}

TEST_F(ChaWebAdapterTest, ClearsAllSessionAudioWithoutVoiceConfigurationOrChangingOtherSessions) {
    const auto path = session_path("lobby", add_audio_entry(EntryStatus::complete, true, 2));
    const auto other = session_path("lobby", add_audio_entry(EntryStatus::complete, true));
    const auto transcript = get(path).json.at("transcript");
    const auto clear = [&](const nlohmann::json& body, std::string type = std::string(json_type)) {
        auto request = post_request(path + "/audio", body.dump(), std::move(type));
        request.method = "DELETE";
        return exchange(*application_, request);
    };
    EXPECT_EQ(clear({{"vault_name", "Other"}}).status, 409);
    EXPECT_EQ(clear({{"vault_name", 1}}).status, 400);
    EXPECT_EQ(clear(nlohmann::json::object()).status, 400);
    EXPECT_EQ(clear({{"vault_name", "Test"}, {"entry_id", 1}}).status, 400);
    EXPECT_EQ(clear({{"vault_name", "Test"}}, "text/plain").status, 415);
    EXPECT_EQ(get(path + "/audio").json.at("cached_entry_ids"), nlohmann::json::array({1, 2}));
    ASSERT_EQ(clear({{"vault_name", "Test"}}).status, 204);
    EXPECT_TRUE(get(path + "/audio").json.at("cached_entry_ids").empty());
    EXPECT_TRUE(get(path + "/audio").json.at("downloads").empty());
    EXPECT_EQ(get(path + "/entries/1/audio").status, 404);
    EXPECT_EQ(get(path + "/entries/2/audio").status, 404);
    auto after = get(path).json.at("transcript");
    ASSERT_EQ(after.size(), transcript.size());
    for (std::size_t index = 0; index < after.size(); ++index) {
        EXPECT_FALSE(after[index].at("has_cached_audio").get<bool>());
        after[index]["has_cached_audio"] = true;
    }
    EXPECT_EQ(after, transcript);
    EXPECT_EQ(get(other + "/entries/1/audio").status, 200);
    EXPECT_EQ(get(other + "/audio").json.at("cached_entry_ids"), nlohmann::json::array({1}));
    EXPECT_EQ(clear({{"vault_name", "Test"}}).status, 204);
    auto missing = post_request(session_path("lobby", "missing") + "/audio", "{\"vault_name\":\"Test\"}");
    missing.method = "DELETE";
    EXPECT_EQ(exchange(*application_, missing).status, 404);
}

TEST_F(ChaWebAdapterTest, AudioRequestsValidateEntryVaultAndBody) {
    const auto path = session_path("lobby", add_audio_entry());
    const auto source = path + "/entries/1/audio";
    EXPECT_EQ(get(source).status, 404);
    EXPECT_EQ(post(source, {{"vault_name", "Test"}}).status, 404);
    EXPECT_EQ(post(source, {{"vault_name", "Other"}}).status, 409);
    EXPECT_EQ(post(source, {{"text", "arbitrary speech"}}).status, 400);
    EXPECT_EQ(post(source, {{"vault_name", 1}}).status, 400);
    EXPECT_EQ(post(source, {{"vault_name", "Test"}}, "text/plain").status, 415);
    for (const auto* id : {"0", "-1", "1x", "9223372036854775808", "18446744073709551616"}) {
        EXPECT_EQ(get(path + "/entries/" + id + "/audio").status, 404);
    }
    EXPECT_EQ(get(path + "/entries/1/audio/extra").status, 404);
    EXPECT_EQ(get(session_path("lobby", "missing") + "/audio").status, 404);
}

TEST_F(ChaWebAdapterTest, AudioGenerationRejectsIncompleteAndHumanEntries) {
    const auto path = session_path("lobby", add_audio_entry(EntryStatus::cancelled));
    EXPECT_EQ(post(path + "/entries/1/audio", {{"vault_name", "Test"}}).status, 404);
    const auto human_path = session_path("lobby", create_session());
    const auto snapshot = get(human_path);
    ASSERT_FALSE(snapshot.json.at("transcript").empty());
    const auto id = snapshot.json.at("transcript").at(0).at("id").get<EntryId>();
    EXPECT_EQ(post(human_path + "/entries/" + std::to_string(id) + "/audio",
        {{"vault_name", "Test"}}).status, 400);
}

TEST_F(ChaWebAdapterTest, VoiceRuntimeDoesNotExposeCredentials) {
    const auto epoch = application_->context_epoch();
    const auto key = application_->create_api_key({.display_name = "Fish", .value = "fish-secret"}, epoch);
    const auto voice = application_->create_voice({.display_name = "Reader", .elevenlabs_voice_id = "voice-ref"}, epoch);
    (void)application_->save_voice_output_settings({.url = "https://api.fish.audio/v1/tts",
        .model = "s2.1-pro", .api_key = key.id, .output_format = "mp3", .default_voice = voice.display_name}, epoch);
    const auto runtime = get("/api/cha/v1/voice-output");
    ASSERT_EQ(runtime.status, 200);
    EXPECT_EQ(runtime.json.at("default_voice_id"), "voice-ref");
    EXPECT_FALSE(runtime.json.contains("api_key"));
    EXPECT_EQ(runtime.raw.find("fish-secret"), std::string::npos);
}

TEST_F(ChaWebAdapterTest, ListsSessionsAndRejectsUnknownForums) {
    const std::string id = create_session();
    const auto epoch = application_->context_epoch();
    const CgiResponse listed_response = get(sessions_path("lobby"));
    EXPECT_EQ(listed_response.status, 200);
    EXPECT_EQ(
        listed_response.json,
        nlohmann::json(application_->list_sessions("lobby", epoch)));
    ASSERT_EQ(listed_response.json.size(), 1u);
    EXPECT_EQ(listed_response.json.at(0).at("id"), id);

    const CgiResponse missing = get(sessions_path("missing"));
    EXPECT_EQ(missing.status, 404);
    EXPECT_EQ(missing.json.at("error").at("code"), "not_found");
    EXPECT_FALSE(missing.json.at("error").at("message").get<std::string>().empty());
}

TEST_F(ChaWebAdapterTest, CreatesOpensInputsAndStops) {
    const CgiResponse created =
        post(sessions_path("lobby"), text_body("@- Hello"));
    ASSERT_EQ(created.status, 201) << created.raw;
    const std::string id = created.json.at("id").get<std::string>();
    EXPECT_EQ(
        created.json,
        nlohmann::json(CreateSessionSuccess{
            id, created.json.at("label").get<std::string>()}));

    const CgiResponse snapshot = get(session_path("lobby", id));
    ASSERT_EQ(snapshot.status, 200) << snapshot.raw;
    EXPECT_EQ(snapshot.json.at("session_id"), id);
    EXPECT_TRUE(snapshot.json.contains("generation"));
    EXPECT_TRUE(snapshot.json.contains("transcript"));
    const auto stored = application_->snapshot(
        "lobby", id, application_->context_epoch());
    ASSERT_TRUE(std::holds_alternative<SessionSnapshot>(stored));
    EXPECT_EQ(snapshot.json, nlohmann::json(std::get<SessionSnapshot>(stored)));

    const CgiResponse again =
        post(session_path("lobby", id) + "/input", text_body("@- again"));
    EXPECT_EQ(again.status, 204) << again.raw;
    EXPECT_EQ(again.raw, "Status: 204 No Content\r\n\r\n");

    const CgiResponse stopped =
        post(session_path("lobby", id) + "/stop", nlohmann::json::object());
    EXPECT_EQ(stopped.status, 204) << stopped.raw;

    const CgiResponse idle =
        post(session_path("lobby", id) + "/stop", nlohmann::json::object());
    EXPECT_EQ(idle.status, 204) << idle.raw;
}

TEST_F(ChaWebAdapterTest, UnknownRoutesAndMethodsAreNotFoundWithoutCors) {
    const struct Case {
        const char* method;
        const char* uri;
    } cases[]{
        {"GET", "/api/cha/v1"},
        {"GET", "/api/cha/v1/"},
        {"GET", "/api/cha/v1/unknown"},
        {"GET", "/api/cha/v1foo"},
        {"GET", "/api/cha/v2/bootstrap"},
        {"GET", "/v1/models"},
        {"POST", "/v1/chat/completions"},
        {"POST", "/api/cha/v1/bootstrap"},
        {"OPTIONS", "/api/cha/v1/bootstrap"},
        {"GET", "/api/cha/v1/forums/lobby/sessions/abc/input"},
        {"POST", "/api/cha/v1/forums/lobby/sessions/abc"},
        {"GET", "/api/cha/v1/forums/../sessions"},
        {"GET", "/api/cha/v1/forums/lobby/sessions/has/slash"},
        {"GET", "/api/cha/v1/forums/lobby/sessions/"},
        {"GET", "/api/cha/v1/forums/not valid/sessions"},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(std::string(item.method) + " " + item.uri);
        const CgiResponse response = exchange(
            *application_,
            {.method = item.method, .document_uri = item.uri, .body = "{}"});
        EXPECT_EQ(response.status, 404);
        EXPECT_EQ(response.json.at("error").at("code"), "not_found");
        EXPECT_EQ(response.raw.find("Allow:"), std::string::npos);
        EXPECT_EQ(response.raw.find("Access-Control"), std::string::npos);
        EXPECT_FALSE(response.json.contains("type"));
    }
}

TEST_F(ChaWebAdapterTest, ChecksAndUploadsTheVaultWithTheExpectedR2Version) {
    MockHttpServer server({
        "HTTP/1.1 200 OK\r\nETag: \"uploaded-version\"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        "HTTP/1.1 200 OK\r\nETag: \"uploaded-version\"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        "HTTP/1.1 200 OK\r\nETag: \"next-version\"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
    });
    (void)application_->save_r2_storage({
        .display_name = "Backups",
        .url = "http://127.0.0.1:" + std::to_string(server.port()) + "/bucket/",
        .access_key_id = "test-access-key",
        .secret_key = std::string("test-secret-key"),
    }, application_->context_epoch());
    server.start();
    const auto check = post("/api/cha/v1/vault/upload-check", nlohmann::json::object());
    ASSERT_EQ(check.status, 200) << check.raw;
    EXPECT_EQ(check.json["status"], "missing");
    EXPECT_TRUE(check.json["etag"].is_null());
    EXPECT_EQ(check.json["context_epoch"], application_->context_epoch());
    const auto uploaded = post("/api/cha/v1/vault/upload", {
        {"etag", check.json["etag"]}, {"context_epoch", check.json["context_epoch"]}});
    ASSERT_EQ(uploaded.status, 200) << uploaded.raw;
    EXPECT_GT(uploaded.json["byte_count"].get<std::uintmax_t>(), 0U);
    EXPECT_EQ(application_->current_vault().get().r2_etag, "uploaded-version");
    const auto matching = post("/api/cha/v1/vault/upload-check", nlohmann::json::object());
    ASSERT_EQ(matching.status, 200) << matching.raw;
    EXPECT_EQ(matching.json["status"], "match");
    EXPECT_EQ(matching.json["etag"], "uploaded-version");
    const auto second = post("/api/cha/v1/vault/upload", {
        {"etag", matching.json["etag"]}, {"context_epoch", matching.json["context_epoch"]}});
    server.join();
    ASSERT_EQ(second.status, 200) << second.raw;
    EXPECT_EQ(application_->current_vault().get().r2_etag, "next-version");
    ASSERT_EQ(server.requests().size(), 5U);
    EXPECT_TRUE(server.requests()[0].starts_with("PUT /bucket/"));
    EXPECT_TRUE(server.requests()[1].starts_with("PUT /bucket/"));
    EXPECT_TRUE(server.requests()[2].starts_with("HEAD /bucket/"));
    EXPECT_TRUE(server.requests()[3].starts_with("PUT /bucket/"));
    EXPECT_NE(server.requests()[3].find("If-Match: \"uploaded-version\""), std::string::npos);
    EXPECT_TRUE(server.requests()[4].starts_with("PUT /bucket/"));
}

TEST_F(ChaWebAdapterTest, DownloadsBacksUpAndReloadsTheVault) {
    const auto session_id = create_session();
    test::TestWorkspace remote;
    remote.add_persona("remote_persona", "Remote persona");
    const auto remote_database = test::import_test_database(remote.root());
    std::ifstream input(remote_database, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    input.close();
    MockHttpServer server({
        http_response("application/toml", "vault_name = \"Test\"\ndata = \"remote.sqlite3\"\n"),
        http_response("application/vnd.sqlite3", bytes),
    });
    (void)application_->save_r2_storage({
        .display_name = "Backups",
        .url = "http://127.0.0.1:" + std::to_string(server.port()) + "/bucket/",
        .access_key_id = "test-access-key",
        .secret_key = std::string("test-secret-key"),
    }, application_->context_epoch());
    const auto epoch = application_->context_epoch();
    server.start();
    const auto response = post("/api/cha/v1/vault/download", nlohmann::json::object());
    server.join();
    ASSERT_EQ(response.status, 200) << response.raw;
    EXPECT_GT(response.json["byte_count"].get<std::uintmax_t>(), bytes.size());
    EXPECT_GT(application_->context_epoch(), epoch);
    EXPECT_TRUE(std::filesystem::exists(database_.string() + ".bac"));
    EXPECT_TRUE(std::filesystem::exists(application_->current_vault().get().source.string() + ".bac"));
    EXPECT_EQ(application_->get_persona("remote_persona", application_->context_epoch()).summary.display_name,
        "Remote persona");
    EXPECT_EQ(get(session_path("lobby", session_id)).status, 404);
    EXPECT_EQ(get(std::string(bootstrap_path)).json["capabilities"]["can_transfer_r2"], false);
}

TEST_F(ChaWebAdapterTest, VaultTransfersValidateRequestsAndRejectStaleUploads) {
    for (const std::string path : {"/api/cha/v1/vault/upload-check", "/api/cha/v1/vault/upload",
            "/api/cha/v1/vault/download"}) {
        SCOPED_TRACE(path);
        EXPECT_EQ(get(path).status, 404);
        EXPECT_EQ(post(path, nlohmann::json::object(), "text/plain").status, 415);
        EXPECT_EQ(post(path, {{"unknown", true}}).status, 400);
        EXPECT_EQ(post(path, nlohmann::json::array()).status, 400);
    }
    for (const nlohmann::json& body : {
            nlohmann::json::object(), nlohmann::json{{"etag", nullptr}},
            nlohmann::json{{"etag", 4}, {"context_epoch", 1}},
            nlohmann::json{{"etag", nullptr}, {"context_epoch", -1}}}) {
        EXPECT_EQ(post("/api/cha/v1/vault/upload", body).status, 400);
    }
    const auto epoch = application_->context_epoch();
    (void)application_->create_vault({.display_name = "Child", .copy_from = "Test"}, epoch);
    (void)application_->switch_vault("Child", {}, epoch);
    const auto stale = post("/api/cha/v1/vault/upload", {{"etag", nullptr}, {"context_epoch", epoch}});
    EXPECT_EQ(stale.status, 409);
    EXPECT_EQ(stale.json["error"]["code"], "vault_changed");
}

TEST_F(ChaWebAdapterTest, DownloadsAndMergesTheParentIntoTheCurrentVault) {
    const auto child = application_->create_vault(
        {.display_name = "Child", .copy_from = "Test"}, application_->context_epoch());
    (void)application_->switch_vault(child.name, {}, application_->context_epoch());
    test::TestWorkspace remote;
    remote.add_persona("parent_persona", "Parent persona");
    const auto remote_database = test::import_test_database(remote.root());
    std::ifstream input(remote_database, std::ios::binary);
    const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    input.close();
    MockHttpServer server({
        http_response("application/toml", "vault_name = \"Test\"\ndata = \"remote.sqlite3\"\n"),
        http_response("application/vnd.sqlite3", bytes),
    });
    (void)application_->save_r2_storage({
        .display_name = "Backups",
        .url = "http://127.0.0.1:" + std::to_string(server.port()) + "/bucket/",
        .access_key_id = "test-access-key",
        .secret_key = std::string("test-secret-key"),
    }, application_->context_epoch());
    const auto epoch = application_->context_epoch();
    const auto bootstrap = get(std::string(bootstrap_path));
    EXPECT_EQ(bootstrap.json["vault_parent"], "Test");
    EXPECT_EQ(bootstrap.json["capabilities"]["can_transfer_r2"], true);
    server.start();
    const auto response = post("/api/cha/v1/vault/merge-parent", nlohmann::json::object());
    server.join();
    ASSERT_EQ(response.status, 200) << response.raw;
    EXPECT_GT(response.json.at("context_epoch").get<std::uint64_t>(), epoch);
    EXPECT_EQ(application_->current_vault().get().name, "Child");
    EXPECT_EQ(application_->current_vault().get().parent, "Test");
    EXPECT_EQ(application_->get_persona("parent_persona", application_->context_epoch()).summary.display_name,
        "Parent persona");
    ASSERT_EQ(server.requests().size(), 2U);
    EXPECT_TRUE(server.requests()[1].starts_with("GET /bucket/" + database_.filename().string() + " HTTP/1.1"));
}

TEST_F(ChaWebAdapterTest, ParentMergeValidatesRequestsAndReportsMissingParent) {
    const std::string path = "/api/cha/v1/vault/merge-parent";
    EXPECT_EQ(get(path).status, 404);
    EXPECT_EQ(post(path, nlohmann::json::object(), "text/plain").status, 415);
    for (const nlohmann::json& body : {
            nlohmann::json{{"unknown", "value"}}, nlohmann::json{{"password", 42}},
            nlohmann::json{{"password", "secret"}, {"extra", true}},
            nlohmann::json::array()}) {
        EXPECT_EQ(post(path, body).status, 400);
    }
    const auto response = post(path, nlohmann::json::object());
    EXPECT_EQ(response.status, 400);
    EXPECT_EQ(response.json["error"]["code"], "invalid_argument");
    EXPECT_EQ(response.json["error"]["message"], "The current vault has no parent");
}

TEST_F(ChaWebAdapterTest, ParentMergeReportsMissingR2Key) {
    const auto child = application_->create_vault(
        {.display_name = "Child", .copy_from = "Test"}, application_->context_epoch());
    (void)application_->switch_vault(child.name, {}, application_->context_epoch());
    const auto response = post("/api/cha/v1/vault/merge-parent", nlohmann::json::object());
    EXPECT_EQ(response.status, 400);
    EXPECT_EQ(response.json["error"]["code"], "invalid_argument");
    EXPECT_EQ(response.json["error"]["message"], "The current vault has no R2 key");
}

TEST_F(ChaWebAdapterTest, ParentMergeReportsR2HttpFailures) {
    const auto child = application_->create_vault(
        {.display_name = "Child", .copy_from = "Test"}, application_->context_epoch());
    (void)application_->switch_vault(child.name, {}, application_->context_epoch());
    const std::string missing = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    for (const bool missing_definition : {true, false}) {
        SCOPED_TRACE(missing_definition);
        MockHttpServer server(missing_definition ? std::vector<std::string>{missing}
            : std::vector<std::string>{http_response("application/toml",
                "vault_name = \"Test\"\ndata = \"remote.sqlite3\"\n"), missing});
        (void)application_->save_r2_storage({
            .display_name = "Backups",
            .url = "http://127.0.0.1:" + std::to_string(server.port()) + "/bucket/",
            .access_key_id = "test-access-key",
            .secret_key = std::string("test-secret-key"),
        }, application_->context_epoch());
        const auto epoch = application_->context_epoch();
        server.start();
        const auto response = post("/api/cha/v1/vault/merge-parent", nlohmann::json::object());
        server.join();
        EXPECT_EQ(response.status, 400) << response.raw;
        EXPECT_EQ(response.json["error"]["code"], "invalid_argument");
        EXPECT_EQ(response.json["error"]["message"], missing_definition
            ? "R2 vault definition was not found. The bucket may contain a legacy database-only upload; "
                "upload with the current CHA version before downloading."
            : "R2 download failed with HTTP status 404");
        EXPECT_EQ(application_->context_epoch(), epoch);
        EXPECT_EQ(application_->current_vault().get().name, "Child");
    }
}

TEST_F(ChaWebAdapterTest, ParentMergeRequiresTheProtectedParentsPassword) {
    const auto child = application_->create_vault(
        {.display_name = "Child", .copy_from = "Test"}, application_->context_epoch());
    (void)application_->switch_vault(child.name, {}, application_->context_epoch());
    (void)application_->update_vault("Test", {.display_name = "Test", .password = "secret"},
        application_->context_epoch());
    const auto response = post("/api/cha/v1/vault/merge-parent", nlohmann::json::object());
    EXPECT_EQ(response.status, 401);
    EXPECT_EQ(response.json["error"]["code"], "source_vault_password_required");
}

TEST_F(ChaWebAdapterTest, DeletesOnlyTheNamedSession) {
    const std::string removed = create_session();
    const std::string kept = create_session();
    const CgiResponse response = exchange(
        *application_,
        {.method = "DELETE", .document_uri = session_path("lobby", removed)});
    EXPECT_EQ(response.status, 204) << response.raw;
    EXPECT_EQ(response.raw, "Status: 204 No Content\r\n\r\n");
    const auto rows = listed();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows.front().id, kept);
    EXPECT_EQ(get(session_path("lobby", removed)).status, 404);
    EXPECT_EQ(get(session_path("lobby", kept)).status, 200);
    EXPECT_EQ(exchange(
        *application_,
        {.method = "DELETE", .document_uri = session_path("lobby", removed)}).status, 404);
}

TEST_F(ChaWebAdapterTest, DeleteRejectsMismatchedForumsAndWelcome) {
    workspace_.add_forum("stoics", "Stoics", "guide");
    application_.reset();
    open_application();
    const std::string id = create_session();
    for (const auto& path : {
            session_path("stoics", id),
            session_path("missing", id),
            session_path("builtin-entrance", "builtin-welcome"),
            sessions_path("lobby"),
            session_path("lobby", id) + "/input"}) {
        const CgiResponse response = exchange(
            *application_, {.method = "DELETE", .document_uri = path});
        EXPECT_EQ(response.status, 404) << response.raw;
        EXPECT_EQ(response.json.at("error").at("code"), "not_found");
    }
    ASSERT_EQ(listed().size(), 1u);
    EXPECT_EQ(listed().front().id, id);
}

TEST_F(ChaWebAdapterTest, DeleteTimeoutKeepsTheStoredSessionAndReturnsAnError) {
    RuntimeSettings settings;
    settings.delete_deadline = 0ms;
    application_.reset();
    open_application(settings);
    const std::string id = create_session();
    const CgiResponse response = exchange(
        *application_, {.method = "DELETE", .document_uri = session_path("lobby", id)});
    EXPECT_EQ(response.status, 500) << response.raw;
    EXPECT_EQ(response.json.at("error").at("code"), "session_stopping");
    EXPECT_EQ(response.json.at("error").at("message"),
        "The session is still stopping. Try deleting it again.");
    ASSERT_EQ(listed().size(), 1u);
    EXPECT_EQ(listed().front().id, id);
}

TEST_F(ChaWebAdapterTest, RejectsMalformedBodiesAndUnknownFields) {
    const std::string id = create_session();
    const struct Case {
        std::string uri;
        std::string body;
    } cases[]{
        {sessions_path("lobby"), "{"},
        {sessions_path("lobby"), "[]"},
        {sessions_path("lobby"), "\"text\""},
        {sessions_path("lobby"), "{\"text\":1}"},
        {sessions_path("lobby"), "{\"text\":\"hi\",\"extra\":true}"},
        {session_path("lobby", id) + "/input", "{\"text\":\"hi\",\"n\":1}"},
        {session_path("lobby", id) + "/stop", "{\"reason\":\"x\"}"},
        {session_path("lobby", id) + "/stop", "[]"},
    };
    const auto before = listed().size();
    for (const auto& item : cases) {
        SCOPED_TRACE(item.body);
        const CgiResponse response = exchange(
            *application_, post_request(item.uri, item.body));
        EXPECT_EQ(response.status, 400);
        EXPECT_EQ(response.json.at("error").at("code"), "invalid_argument");
    }
    EXPECT_EQ(listed().size(), before);
}

TEST_F(ChaWebAdapterTest, ForumSessionMismatchAndUnknownStopAreNotFound) {
    workspace_.add_forum("stoics", "Stoics", "guide");
    application_.reset();
    open_application();
    const std::string id = create_session();

    const CgiResponse mismatch = get(session_path("stoics", id));
    EXPECT_EQ(mismatch.status, 404);
    EXPECT_EQ(mismatch.json.at("error").at("code"), "not_found");

    const CgiResponse unknown = post(
        session_path("lobby", "missing") + "/stop", nlohmann::json::object());
    EXPECT_EQ(unknown.status, 404);
}

TEST_F(ChaWebAdapterTest, PromptLimitIsCheckedBeforeCreation) {
    RuntimeSettings settings;
    settings.prompt_limit = 8;
    application_.reset();
    open_application(settings);
    const std::size_t limit = application_->settings().prompt_limit;
    const CgiResponse allowed = post(
        sessions_path("lobby"), text_body(std::string(limit, 'a')));
    EXPECT_EQ(allowed.status, 201) << allowed.raw;

    const auto before = listed().size();
    const CgiResponse rejected = post(
        sessions_path("lobby"), text_body(std::string(limit + 1, 'a')));
    EXPECT_EQ(rejected.status, 400);
    EXPECT_EQ(rejected.json.at("error").at("code"), "prompt_too_large");
    EXPECT_EQ(listed().size(), before);

    const std::string id = allowed.json.at("id").get<std::string>();
    const CgiResponse input = post(
        session_path("lobby", id) + "/input",
        text_body(std::string(limit + 1, 'b')));
    EXPECT_EQ(input.status, 400);
    EXPECT_EQ(input.json.at("error").at("code"), "prompt_too_large");
}

TEST_F(ChaWebAdapterTest, PostMediaTypesAreValidatedBeforeMutation) {
    const std::string existing = create_session();
    const std::string posts[]{
        sessions_path("lobby"),
        session_path("lobby", existing) + "/input",
        session_path("lobby", existing) + "/stop",
    };
    const char* accepted[]{
        "application/json",
        "Application/JSON",
        " application/json ",
        "application/json; charset=utf-8",
        "application/json;charset=UTF-8",
    };
    const char* rejected[]{
        "",
        "text/plain",
        "application/jsonx",
        "application/jsonp",
        "application/jsonyes",
        "json",
        "text/application/json",
    };

    for (const char* type : accepted) {
        SCOPED_TRACE(type);
        const CgiResponse create = post(
            sessions_path("lobby"), text_body("@- Hello"), type);
        EXPECT_EQ(create.status, 201) << create.raw;
        const std::string id = create.json.at("id").get<std::string>();
        EXPECT_EQ(
            post(
                session_path("lobby", id) + "/input",
                text_body("@- Hi"),
                type)
                .status,
            204);
        EXPECT_EQ(
            post(
                session_path("lobby", id) + "/stop",
                nlohmann::json::object(),
                type)
                .status,
            204);
    }

    const auto before = listed().size();
    for (const auto& path : posts) {
        for (const char* type : rejected) {
            SCOPED_TRACE(std::string(path) + " " + type);
            const nlohmann::json body = std::string_view(path).ends_with("/stop")
                ? nlohmann::json::object()
                : text_body("should not store");
            const CgiResponse response = post(path, body, type);
            EXPECT_EQ(response.status, 415);
            EXPECT_EQ(response.json.at("error").at("code"), "invalid_argument");
        }
        const CgiResponse malformed = exchange(
            *application_,
            post_request(path, "{", std::string(json_type)));
        EXPECT_EQ(malformed.status, 400);
        EXPECT_EQ(malformed.json.at("error").at("code"), "invalid_argument");
    }
    EXPECT_EQ(listed().size(), before);
}

TEST_F(ChaWebAdapterTest, AcceptsSelfNotesAndCleansUpParserRejection) {
    const CgiResponse note =
        post(sessions_path("lobby"), text_body("@- keep this note"));
    ASSERT_EQ(note.status, 201) << note.raw;
    const std::string id = note.json.at("id").get<std::string>();
    const CgiResponse snapshot = get(session_path("lobby", id));
    ASSERT_EQ(snapshot.status, 200);
    bool found = false;
    for (const auto& entry : snapshot.json.at("transcript")) {
        if (entry.at("text").get<std::string>().find("keep this note")
            != std::string::npos) {
            found = true;
        }
    }
    EXPECT_TRUE(found);

    const auto before = listed().size();
    const CgiResponse rejected =
        post(sessions_path("lobby"), text_body("/not-a-command"));
    EXPECT_EQ(rejected.status, 422);
    EXPECT_EQ(rejected.json.at("error").at("code"), "invalid_argument");
    EXPECT_NE(
        rejected.json.at("error").at("message").get<std::string>().find(
            "Unknown command"),
        std::string::npos);
    EXPECT_EQ(listed().size(), before);

    const CgiResponse later = post(
        session_path("lobby", id) + "/input", text_body("/still-unknown"));
    EXPECT_EQ(later.status, 422);
    EXPECT_EQ(listed().size(), before);
    EXPECT_TRUE(std::holds_alternative<SessionSnapshot>(
        application_->snapshot("lobby", id, application_->context_epoch())));
}

TEST_F(ChaWebAdapterTest, CleanupFailureReturns500AndUnknownDeleteIsSuccess) {
    const auto fail = [](Application&,
                         std::string_view,
                         std::string_view,
                         std::uint64_t) -> std::optional<ErrorCode> {
        return ErrorCode::internal_error;
    };
    const auto gone = [](Application& application,
                         std::string_view forum_id,
                         std::string_view session_id,
                         std::uint64_t epoch) -> std::optional<ErrorCode> {
        (void)application.delete_session(forum_id, session_id, epoch);
        return ErrorCode::not_found;
    };

    const CgiResponse failed = post(
        sessions_path("lobby"),
        text_body("/not-a-command"),
        std::string(json_type),
        fail);
    EXPECT_EQ(failed.status, 500);
    EXPECT_EQ(failed.json.at("error").at("code"), "internal_error");
    EXPECT_FALSE(listed().empty());

    const auto before = listed().size();
    const CgiResponse cleaned = post(
        sessions_path("lobby"),
        text_body("/not-a-command"),
        std::string(json_type),
        gone);
    EXPECT_EQ(cleaned.status, 422);
    EXPECT_EQ(listed().size(), before);
}

TEST_F(ChaWebAdapterTest, KeepsStoredInputAfterSubmissionFailures) {
    const ChaWebSubmitInput failures[]{
        [](Application& application, std::string_view forum,
           std::string_view session, RawCommand command,
           std::uint64_t epoch) -> CommandSubmitResult {
            (void)application.submit(forum, session, std::move(command), epoch);
            return ErrorCode::internal_error;
        },
        [](Application& application, std::string_view forum,
           std::string_view session, RawCommand command,
           std::uint64_t epoch) -> CommandSubmitResult {
            (void)application.submit(forum, session, std::move(command), epoch);
            throw app::ApplicationError(ErrorCode::internal_error);
        },
        [](Application& application, std::string_view forum,
           std::string_view session, RawCommand command,
           std::uint64_t epoch) -> CommandSubmitResult {
            (void)application.submit(forum, session, std::move(command), epoch);
            throw std::runtime_error("Injected failure after storing input");
        },
    };
    for (const auto fail : failures) {
        const auto before = listed();
        const CgiResponse response = exchange(
            *application_,
            post_request(sessions_path("lobby"), text_body("@- keep this note").dump()),
            nullptr, fail);
        EXPECT_EQ(response.status, 500) << response.raw;
        EXPECT_EQ(response.json.at("error").at("code"), "internal_error");
        const auto after = listed();
        ASSERT_EQ(after.size(), before.size() + 1);
        const auto created = std::find_if(after.begin(), after.end(), [&](const auto& row) {
            return std::none_of(before.begin(), before.end(), [&](const auto& old) {
                return row.id == old.id;
            });
        });
        ASSERT_NE(created, after.end());
        const CgiResponse snapshot = get(session_path("lobby", created->id));
        ASSERT_EQ(snapshot.status, 200) << snapshot.raw;
        const auto& transcript = snapshot.json.at("transcript");
        EXPECT_TRUE(std::any_of(transcript.begin(), transcript.end(), [](const auto& entry) {
            return entry.at("text") == "keep this note";
        }));
    }
}

TEST_F(ChaWebAdapterTest, ErrorsUseTheSharedSafeMessages) {
    const CgiResponse limited = exchange(
        *application_,
        post_request(sessions_path("lobby"), text_body("Hello").dump()),
        nullptr,
        [](Application&, std::string_view, std::string_view, RawCommand,
           std::uint64_t) -> CommandSubmitResult {
            return ErrorCode::session_limit_reached;
        });
    EXPECT_EQ(limited.status, 500) << limited.raw;
    EXPECT_EQ(limited.json.at("error").at("code"), "session_limit_reached");
    EXPECT_EQ(
        limited.json.at("error").at("message"),
        "Another session has not closed yet.");

    const CgiResponse thrown = exchange(
        *application_,
        post_request(sessions_path("lobby"), text_body("Hello").dump()),
        nullptr,
        [](Application&, std::string_view, std::string_view, RawCommand,
           std::uint64_t) -> CommandSubmitResult {
            throw app::ApplicationError(
                ErrorCode::internal_error, "sqlite detail");
        });
    EXPECT_EQ(thrown.status, 500) << thrown.raw;
    EXPECT_EQ(
        thrown.json.at("error").at("message"),
        "The request could not be completed.");
}

TEST_F(ChaWebAdapterTest, LoadsARetiredSessionAndDoesNotRetryUnknownOnes) {
    const std::string id = create_session();
    const auto epoch = application_->context_epoch();
    application_->close_session("lobby", id, epoch);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (application_->live_session_count() > 0
        && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(5ms);
    }
    EXPECT_EQ(application_->live_session_count(), 0u);

    const CgiResponse snapshot = get(session_path("lobby", id));
    ASSERT_EQ(snapshot.status, 200) << snapshot.raw;
    EXPECT_EQ(snapshot.json.at("session_id"), id);

    const auto before = listed().size();
    const CgiResponse missing = post(
        session_path("lobby", "missing") + "/input", text_body("Hello"));
    EXPECT_EQ(missing.status, 404);
    EXPECT_EQ(listed().size(), before);
}

TEST_F(ChaWebAdapterTest, UnavailableApplicationReturns503) {
    application_->mark_unusable();
    const CgiResponse response = get(std::string(bootstrap_path));
    EXPECT_EQ(response.status, 503);
    EXPECT_EQ(
        response.json.at("error").at("code"), "application_unavailable");
    EXPECT_EQ(
        response.json.at("error").at("message"),
        "The application is unavailable.");
}

TEST(ChaWebAdapter, UnknownSubmissionOutcomeKeepsTheSession) {
    test::TestWorkspace workspace;
    MockHttpServer server({http_json(
        R"({"answers":{"recipient":{"type":"choice","choice":"undefined"}}})")});
    server.pause_before_response(1);
    const auto directory = workspace.root() / "system/jev";
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "config.toml")
        << "url = \"http://127.0.0.1:" << server.port()
        << "/decisions\"\nmodel = \"jev\"\napi_key = \"api_key_1\"\n";
    RuntimeSettings settings;
    settings.command_deadline = 200ms;
    auto application = Application::open(
        make_command(workspace, test::import_test_database(workspace.root())),
        {},
        settings);
    ASSERT_EQ(
        application->create_api_key(
            {.display_name = "Jev", .value = "jev-secret"},
            application->context_epoch())
            .id,
        "api_key_1");
    server.start();
    CgiResponse response;
    std::thread worker([&] {
        response = exchange(
            *application,
            post_request(sessions_path("lobby"), text_body("Hello").dump()));
    });
    ASSERT_TRUE(server.wait_for_requests(1, 5s));
    worker.join();
    EXPECT_EQ(response.status, 500) << response.raw;
    EXPECT_EQ(response.json.at("error").at("code"), "command_timeout");
    EXPECT_FALSE(
        application->list_sessions("lobby", application->context_epoch())
            .empty());
    server.resume_responses();
    server.join();
}

TEST(ChaWebAdapter, InputReturnsWhileGenerationContinues) {
    test::TestWorkspace workspace;
    disable_naming(workspace);
    MockHttpServer server({http_json(
        R"({"choices":[{"message":{"content":"Later"}}]})")});
    server.pause_before_response(1);
    use_net_provider(workspace, server.port());
    auto application = Application::open(
        make_command(workspace, test::import_test_database(workspace.root())));
    server.start();
    const CgiResponse created = exchange(
        *application,
        post_request(sessions_path("lobby"), text_body("Hello").dump()));
    ASSERT_EQ(created.status, 201) << created.raw;
    ASSERT_TRUE(server.wait_for_requests(1, 5s));
    const std::string id = created.json.at("id").get<std::string>();
    const CgiResponse snapshot = exchange(
        *application, get_request(session_path("lobby", id)));
    ASSERT_EQ(snapshot.status, 200) << snapshot.raw;
    EXPECT_TRUE(snapshot.json.at("generation").at("active").get<bool>());
    server.resume_responses();
    server.join();
}

TEST(ChaWebAdapter, DeleteStopsGenerationAndPreventsTheSessionFromReturning) {
    test::TestWorkspace workspace;
    disable_naming(workspace);
    MockHttpServer server({http_json(
        R"({"choices":[{"message":{"content":"Later"}}]})")});
    server.pause_before_response(1);
    use_net_provider(workspace, server.port());
    auto application = Application::open(
        make_command(workspace, test::import_test_database(workspace.root())));
    server.start();
    const CgiResponse created = exchange(
        *application,
        post_request(sessions_path("lobby"), text_body("Hello").dump()));
    ASSERT_EQ(created.status, 201) << created.raw;
    ASSERT_TRUE(server.wait_for_requests(1, 5s));
    const std::string id = created.json.at("id").get<std::string>();
    const CgiResponse snapshot = exchange(
        *application, get_request(session_path("lobby", id)));
    EXPECT_TRUE(snapshot.json.at("generation").at("active").get<bool>());
    const CgiResponse deleted = exchange(
        *application, {.method = "DELETE", .document_uri = session_path("lobby", id)});
    EXPECT_EQ(deleted.status, 204) << deleted.raw;
    EXPECT_TRUE(application->list_sessions("lobby", application->context_epoch()).empty());
    server.resume_responses();
    server.join();
    EXPECT_EQ(exchange(*application, get_request(session_path("lobby", id))).status, 404);
}

class ChaWebLogGuard {
public:
    ChaWebLogGuard() {
        shutdown_diagnostic_logging();
        directory_ = std::filesystem::temp_directory_path()
            / ("cha_chaweb_maintenance_"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory_);
        initialize_diagnostic_logging(directory_ / "cha.log", "info");
    }

    ~ChaWebLogGuard() {
        shutdown_diagnostic_logging();
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

private:
    std::filesystem::path directory_;
};

nlohmann::json chat_message(nlohmann::json tool_calls, std::string content = {}) {
    nlohmann::json message{{"role", "assistant"}};
    if (tool_calls.empty()) {
        message["content"] = std::move(content);
    } else {
        message["content"] = nullptr;
        message["tool_calls"] = std::move(tool_calls);
    }
    return {
        {"choices", nlohmann::json::array({nlohmann::json{{"message", std::move(message)}}})},
        {"usage", {{"prompt_tokens", 10}, {"completion_tokens", 2}}},
    };
}

nlohmann::json function_call(std::string id, std::string name, const nlohmann::json& arguments) {
    return {
        {"id", std::move(id)},
        {"type", "function"},
        {"function", {{"name", std::move(name)}, {"arguments", arguments.dump()}}},
    };
}

const std::string kGuideConfig = "display_name = \"Guide\"\nprovider = \"remote\"\n";
const std::string kGuideRenamed = "display_name = \"Guide renamed\"\nprovider = \"remote\"\n";

std::string http_error(int status, std::string_view reason) {
    return "HTTP/1.1 " + std::to_string(status) + " " + std::string(reason)
        + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
}

nlohmann::json wait_snapshot(
    Application& application,
    std::string_view forum,
    std::string_view session,
    bool active) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    CgiResponse snapshot;
    while (std::chrono::steady_clock::now() < deadline) {
        snapshot = exchange(application, get_request(session_path(forum, session)));
        if (snapshot.status == 200
            && snapshot.json.at("generation").at("active").get<bool>() == active) {
            return snapshot.json;
        }
        std::this_thread::sleep_for(10ms);
    }
    ADD_FAILURE() << snapshot.raw;
    return snapshot.json;
}

bool transcript_has_notice(const nlohmann::json& snapshot, std::string_view text) {
    for (const auto& entry : snapshot.at("transcript")) {
        if (entry.at("kind") == "notice"
            && entry.at("text").get<std::string>().find(text) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> tool_names(const nlohmann::json& body) {
    std::vector<std::string> names;
    for (const auto& tool : body.at("tools")) {
        names.push_back(tool.at("function").at("name").get<std::string>());
    }
    return names;
}

TEST(ChaWebAdapter, OrdinarySessionDoesNotReceiveMaintenanceTools) {
    test::TestWorkspace workspace;
    disable_naming(workspace);
    MockHttpServer server({http_json(chat_message(nlohmann::json::array(), "Hello").dump())});
    use_net_provider(workspace, server.port());
    std::ofstream(workspace.root() / "system" / "assistant" / "character.toml")
        << "display_name = \"Assistant\"\nprovider = \"test\"\n";
    auto application = Application::open(
        make_command(workspace, test::import_test_database(workspace.root())));
    server.start();
    const CgiResponse created = exchange(
        *application,
        post_request(sessions_path("lobby"), text_body("Hello").dump()));
    ASSERT_EQ(created.status, 201) << created.raw;
    ASSERT_TRUE(server.wait_for_requests(1, 5s));
    server.join();
    const auto body = nlohmann::json::parse(request_body(server.requests().front()));
    const std::string encoded = body.dump();
    EXPECT_EQ(encoded.find("Operating instructions for Assistant"), std::string::npos);
    EXPECT_EQ(encoded.find("vault_config_list"), std::string::npos);
}

TEST(ChaWebAdapter, WelcomeSetsUndoesAndCreatesCharacterWithPreservedDefaults) {
    for (const bool fail_refresh : {false, true}) {
        test::TestWorkspace workspace;
        disable_naming(workspace);
        using Json = nlohmann::json;
        const auto set = chat_message(Json::array({function_call("set1", "vault_config_apply", {
            {"action", "apply"}, {"version", "1"}, {"changes", Json::array({Json{
                {"path", "characters/guide/character.toml"}, {"operation", "set"},
                {"content", nullptr}, {"key", "reasoning_effort"}, {"value", "high"}}})}})}));
        const auto undo = chat_message(Json::array({function_call("undo1", "vault_config_apply", {
            {"action", "undo"}, {"version", "2"}, {"changes", nullptr}})}));
        const auto create = chat_message(Json::array({function_call("create1", "add_character", {
            {"version", "3"}, {"name", "Scholar"}, {"description", "A careful thinker"},
            {"provider_id", "remote"}, {"profile", "# Scholar\nA careful thinker.\n"}, {"forum_id", "lobby"}})}));
        MockHttpServer server({http_json(set.dump()), http_json(undo.dump()), http_json(create.dump()),
            http_json(chat_message(Json::array(), "Created Scholar.").dump())});
        if (fail_refresh) server.pause_before_response(3);
        use_net_provider(workspace, server.port());
        auto command = make_command(workspace, test::import_test_database(workspace.root()));
        command.chaweb_host = true;
        auto application = Application::open(std::move(command));
        const auto original = application->store().read_config(
            std::vector<std::string>{"forums/lobby/config.toml", "characters/guide/character.toml"});
        const auto persona = application->store().snapshot()->find_forum("lobby")->default_persona_id;
        server.start();
        const auto response = exchange(*application, post_request(
            session_path(entrance_id, welcome_id) + "/input",
            text_body("Set Guide effort, undo it, then create Scholar in the lobby").dump()));
        ASSERT_EQ(response.status, 204) << response.raw;
        if (fail_refresh) {
            ASSERT_TRUE(server.wait_for_requests(3, 5s));
            force_next_forum_sync_failure();
            server.resume_responses();
        }
        const auto finished = wait_snapshot(*application, entrance_id, welcome_id, false);
        EXPECT_TRUE(transcript_has_notice(finished, "Configuration was saved."));
        EXPECT_EQ(transcript_has_notice(finished, "Refresh failed:"), fail_refresh);
        EXPECT_TRUE(transcript_has_notice(finished, "Undo is not available."));
        server.join();
        ASSERT_EQ(server.requests().size(), 4U);
        const auto snapshot = application->store().snapshot();
        ASSERT_NE(snapshot->find_character("character_1"), nullptr);
        EXPECT_EQ(snapshot->find_character("character_1")->provider_id, "remote");
        EXPECT_FALSE(snapshot->find_character("guide")->reasoning_effort);
        ASSERT_NE(snapshot->find_forum_member("lobby", "character_1"), nullptr);
        EXPECT_EQ(snapshot->find_forum("lobby")->default_character_id, "guide");
        EXPECT_EQ(snapshot->find_forum("lobby")->default_persona_id, persona);
        const auto read = application->store().read_config(
            std::vector<std::string>{"forums/lobby/config.toml", "characters/guide/character.toml"});
        EXPECT_EQ(read.files[0].content, "default_character = \"guide\"\n" + original.files[0].content);
        EXPECT_EQ(read.files[1].content, original.files[1].content);
        const auto final = Json::parse(request_body(server.requests().back()));
        bool saw_creation = false;
        for (const auto& message : final["messages"]) {
            if (message.value("role", "") != "tool") continue;
            const auto result = Json::parse(message["content"].get<std::string>());
            if (result.contains("character_id")) {
                saw_creation = true;
                EXPECT_EQ(result["committed"], true);
                EXPECT_EQ(result["character_id"], "character_1");
                EXPECT_EQ(result["undo_available"], false);
                EXPECT_EQ(result["message"].get<std::string>().find("Refresh failed:") != std::string::npos,
                    fail_refresh);
            }
        }
        EXPECT_TRUE(saw_creation);
    }
}

TEST(ChaWebAdapter, WelcomeRepairsAndUndoesThroughChat) {
    ChaWebLogGuard logs;
    test::TestWorkspace workspace;
    disable_naming(workspace);
    const auto read_and_logs = chat_message(nlohmann::json::array({
        function_call("read1", "vault_config_read",
            {{"paths", nlohmann::json::array({"characters/guide/character.toml"})}}),
        function_call("logs1", "assistant_logs", {
            {"minimum_level", nullptr},
            {"contains", "guide diagnosis"},
            {"limit", 5},
        }),
    }));
    const auto diagnosis = chat_message(nlohmann::json::array(), "The guide name is ready to change.");
    const auto apply = chat_message(nlohmann::json::array({
        function_call("apply1", "vault_config_apply", {
            {"action", "apply"},
            {"version", "1"},
            {"changes", nlohmann::json::array({nlohmann::json{
                {"path", "characters/guide/character.toml"},
                {"operation", "replace"}, {"key", nullptr}, {"value", nullptr},
                {"content", kGuideRenamed},
            }})},
        }),
    }));
    const auto saved = chat_message(nlohmann::json::array(), "Saved the guide name.");
    const auto undo = chat_message(nlohmann::json::array({
        function_call("undo1", "vault_config_apply", {
            {"action", "undo"},
            {"version", "2"},
            {"changes", nullptr},
        }),
    }));
    const auto restored = chat_message(nlohmann::json::array(), "Restored the guide name.");
    const auto hello = chat_message(nlohmann::json::array(), "Hello");
    MockHttpServer server({
        http_json(hello.dump()),
        http_json(read_and_logs.dump()),
        http_json(diagnosis.dump()),
        http_json(apply.dump()),
        http_json(saved.dump()),
        http_json(undo.dump()),
        http_json(restored.dump()),
    });
    use_net_provider(workspace, server.port());
    const auto database = test::import_test_database(workspace.root());
    auto command = make_command(workspace, database);
    command.chaweb_host = true;
    auto application = Application::open(std::move(command));
    ASSERT_EQ(application->store().config_revision(), 1u);
    ASSERT_EQ(
        application->store().read_config(
            std::vector<std::string>{"characters/guide/character.toml"}).files.at(0).content,
        kGuideConfig);
    server.start();
    const CgiResponse created = exchange(
        *application, post_request(sessions_path("lobby"), text_body("Hello").dump()));
    ASSERT_EQ(created.status, 201) << created.raw;
    const std::string lobby = created.json.at("id").get<std::string>();
    ASSERT_FALSE(wait_snapshot(*application, "lobby", lobby, false).is_null());

    log_info("guide diagnosis marker");
    const std::string input = session_path(entrance_id, welcome_id) + "/input";
    const CgiResponse review = exchange(
        *application, post_request(input, text_body("Review the guide").dump()));
    ASSERT_EQ(review.status, 204) << review.raw;
    const auto reviewed = wait_snapshot(*application, entrance_id, welcome_id, false);
    EXPECT_NE(reviewed.dump().find("The guide name is ready to change."), std::string::npos);

    const CgiResponse fix = exchange(
        *application, post_request(input, text_body("Fix it").dump()));
    ASSERT_EQ(fix.status, 204) << fix.raw;
    const auto fixed = wait_snapshot(*application, entrance_id, welcome_id, false);
    EXPECT_TRUE(transcript_has_notice(fixed, "Configuration was saved."));
    const auto boot = exchange(*application, get_request(std::string(bootstrap_path)));
    ASSERT_EQ(boot.status, 200) << boot.raw;
    bool renamed = false;
    for (const auto& character : boot.json.at("characters")) {
        if (character.at("id") == "guide") {
            EXPECT_EQ(character.at("display_name"), "Guide renamed");
            renamed = true;
        }
    }
    EXPECT_TRUE(renamed);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    bool lobby_closed = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto listed = application->list_sessions("lobby", application->context_epoch());
        if (!listed.empty() && !listed.front().live) {
            lobby_closed = true;
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    EXPECT_TRUE(lobby_closed);
    EXPECT_EQ(
        exchange(*application, get_request(session_path(entrance_id, welcome_id))).status,
        200);

    const CgiResponse restore = exchange(
        *application, post_request(input, text_body("Undo").dump()));
    ASSERT_EQ(restore.status, 204) << restore.raw;
    const auto restored_snapshot = wait_snapshot(*application, entrance_id, welcome_id, false);
    EXPECT_TRUE(transcript_has_notice(restored_snapshot, "Configuration was saved."));
    EXPECT_EQ(
        application->store().snapshot()->find_character("guide")->character.display_name,
        "Guide");
    server.join();

    ASSERT_EQ(server.requests().size(), 7u);
    const auto first = nlohmann::json::parse(request_body(server.requests()[1]));
    EXPECT_EQ(tool_names(first), (std::vector<std::string>{
        "vault_config_list", "vault_config_read", "vault_config_apply", "add_character",
        "assistant_logs", "assistant_logging",
        "host_config_list", "host_config_read", "host_config_write",
        "assistant_openai_login"}));
    const std::string system = first.at("messages").at(0).at("content").get<std::string>();
    EXPECT_NE(system.find(
        "You are Assistant, the CHA application guide. In Welcome you can "
        "diagnose and repair this vault."), std::string::npos);
    EXPECT_NE(system.find("Host: cha-daemon (ChaWeb)\n\n"), std::string::npos);
    EXPECT_NE(system.find("# Operating instructions for Assistant"), std::string::npos);
    EXPECT_NE(system.find("# CHA workspace maintainer guide"), std::string::npos);
    EXPECT_NE(system.find("# CHA daemon on Linux"), std::string::npos);
    EXPECT_EQ(system.find("Host: desktop application\n\n# Operating instructions"), std::string::npos);
    const auto continued = nlohmann::json::parse(request_body(server.requests()[2]));
    bool saw_file = false;
    bool saw_marker = false;
    for (const auto& message : continued.at("messages")) {
        if (message.value("role", "") != "tool") continue;
        const auto payload = nlohmann::json::parse(
            message.at("content").get<std::string>(), nullptr, false);
        if (payload.is_discarded() || !payload.is_object()) continue;
        if (payload.contains("files")) {
            for (const auto& file : payload["files"]) {
                if (file.value("content", std::string{}) == kGuideConfig) saw_file = true;
            }
        }
        if (payload.contains("entries")) {
            for (const auto& entry : payload["entries"]) {
                const auto text = entry.value("text", std::string{});
                if (text.find("guide diagnosis marker") != std::string::npos) saw_marker = true;
            }
        }
    }
    EXPECT_TRUE(saw_file);
    EXPECT_TRUE(saw_marker);
    const auto saved_request = request_body(server.requests()[4]);
    EXPECT_NE(saved_request.find("Configuration was saved."), std::string::npos);
    const auto undo_request = request_body(server.requests()[5]);
    EXPECT_EQ(undo_request.find("Configuration was saved."), std::string::npos);
}

TEST(ChaWebAdapter, WelcomeKeepsANoticeWhenTheAnswerFailsAfterCommit) {
    test::TestWorkspace workspace;
    disable_naming(workspace);
    const auto apply = chat_message(nlohmann::json::array({
        function_call("apply1", "vault_config_apply", {
            {"action", "apply"},
            {"version", "1"},
            {"changes", nlohmann::json::array({nlohmann::json{
                {"path", "characters/guide/character.toml"},
                {"operation", "replace"}, {"key", nullptr}, {"value", nullptr},
                {"content", kGuideRenamed},
            }})},
        }),
    }));
    MockHttpServer server({http_json(apply.dump()), http_error(500, "Server Error")});
    use_net_provider(workspace, server.port());
    auto command = make_command(workspace, test::import_test_database(workspace.root()));
    command.chaweb_host = true;
    auto application = Application::open(std::move(command));
    ASSERT_EQ(application->store().config_revision(), 1u);
    server.start();
    const CgiResponse fix = exchange(
        *application,
        post_request(
            session_path(entrance_id, welcome_id) + "/input",
            text_body("Fix it").dump()));
    ASSERT_EQ(fix.status, 204) << fix.raw;
    const auto snapshot = wait_snapshot(*application, entrance_id, welcome_id, false);
    EXPECT_TRUE(transcript_has_notice(snapshot, "Configuration was saved."));
    const auto boot = exchange(*application, get_request(std::string(bootstrap_path)));
    bool renamed = false;
    for (const auto& character : boot.json.at("characters")) {
        if (character.at("id") == "guide") {
            EXPECT_EQ(character.at("display_name"), "Guide renamed");
            renamed = true;
        }
    }
    EXPECT_TRUE(renamed);
    server.join();
}

TEST(ChaWebAdapter, StopAfterASavedRepairStoresTheNotice) {
    test::TestWorkspace workspace;
    disable_naming(workspace);
    const auto apply = chat_message(nlohmann::json::array({
        function_call("apply1", "vault_config_apply", {
            {"action", "apply"},
            {"version", "1"},
            {"changes", nlohmann::json::array({nlohmann::json{
                {"path", "characters/guide/character.toml"},
                {"operation", "replace"}, {"key", nullptr}, {"value", nullptr},
                {"content", kGuideRenamed},
            }})},
        }),
    }));
    const auto late = chat_message(nlohmann::json::array(), "This answer should not be required.");
    MockHttpServer server({http_json(apply.dump()), http_json(late.dump())});
    server.pause_before_response(2);
    use_net_provider(workspace, server.port());
    auto command = make_command(workspace, test::import_test_database(workspace.root()));
    command.chaweb_host = true;
    auto application = Application::open(std::move(command));
    ASSERT_EQ(application->store().config_revision(), 1u);
    server.start();
    const std::string welcome = session_path(entrance_id, welcome_id);
    const CgiResponse fix = exchange(
        *application, post_request(welcome + "/input", text_body("Fix it").dump()));
    ASSERT_EQ(fix.status, 204) << fix.raw;
    ASSERT_TRUE(server.wait_for_requests(2, 5s));
    const CgiResponse stopped = exchange(
        *application, post_request(welcome + "/stop", "{}"));
    EXPECT_EQ(stopped.status, 204) << stopped.raw;
    const auto snapshot = wait_snapshot(*application, entrance_id, welcome_id, false);
    EXPECT_TRUE(transcript_has_notice(snapshot, "Configuration was saved."));
    server.resume_responses();
    server.join();
}

} // namespace
} // namespace cha::daemon
