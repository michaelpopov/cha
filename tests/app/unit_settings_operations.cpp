#include "app/application.h"

#include "support/mock_http_server.h"
#include "support/test_workspace.h"
#include "runtime/request_parser.h"
#include "workspace/builtins.h"
#include "workspace/workspace.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
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

nlohmann::json provider_body(const ProviderDetail& provider) {
    nlohmann::json json = provider;
    json.erase("id");
    json.erase("used_by");
    json.erase("writable");
    return json;
}

TEST(ApplicationSettings, ListsAndUpdatesProvidersWithoutSecrets) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    auto application = Application::open(make_command(workspace, database));
    const auto epoch = application->context_epoch();

    const auto listed = application->list_providers(epoch);
    ASSERT_FALSE(listed.empty());
    EXPECT_EQ(listed.front().id, "test");

    const auto created_key = application->create_api_key(
        {.display_name = "Router key", .value = "private-router-secret"}, epoch);
    EXPECT_FALSE(created_key.id.empty());
    EXPECT_TRUE(created_key.has_value);
    EXPECT_EQ(created_key.display_name, "Router key");

    const auto listed_keys = application->list_api_keys(epoch);
    ASSERT_FALSE(listed_keys.empty());
    EXPECT_EQ(nlohmann::json(listed_keys).dump().find("private-router-secret"),
        std::string::npos);

    auto provider = application->get_provider("test", epoch);
    EXPECT_EQ(provider.model, "fake");
    EXPECT_EQ(provider.reasoning_effort, "none");
    nlohmann::json body = provider_body(provider);
    body["api_key"] = created_key.id;
    body["reasoning_effort"] = "low";
    const auto updated = application->update_provider("test", body, epoch);
    EXPECT_EQ(updated.api_key, created_key.id);
    EXPECT_EQ(updated.reasoning_effort, "low");
    EXPECT_EQ(application->get_provider("test", epoch).reasoning_effort, "low");
    body["reasoning_effort"] = "minimal";
    EXPECT_THROW((void)application->update_provider("test", body, epoch), ApplicationError);
    EXPECT_EQ(nlohmann::json(updated).dump().find("private-router-secret"),
        std::string::npos);

    EXPECT_THROW(
        application->delete_provider("test", epoch),
        ApplicationError);

    const auto copied = application->create_provider(
        {.display_name = "Copy", .copy_from = "test"}, epoch);
    EXPECT_EQ(copied.display_name, "Copy");
    application->delete_provider(copied.id, epoch);
}

TEST(ApplicationSettings, SavesCharacterWhenNewProviderCannotUseSearchOverride) {
    test::TestWorkspace workspace;
    workspace.write_provider("fireworks",
        "host = \"api.fireworks.ai\"\nport = 443\nmodel = \"qwen\"\n"
        "api = \"chat_completions\"\nmode = \"test\"\n");
    workspace.write_style("sans-bold", "font = \"sans\"\nweight = \"bold\"\n");
    const auto database = test::import_test_database(workspace.root());
    auto application = Application::open(make_command(workspace, database));
    auto epoch = application->context_epoch();
    const auto voice = application->create_voice(
        {.display_name = "Muller", .elevenlabs_voice_id = "muller"}, epoch);
    ASSERT_NO_THROW((void)application->get_provider("fireworks", epoch));

    for (const auto search : {WebSearchMode::automatic, WebSearchMode::required}) {
        // Switching providers keeps the previous search selection in the form.
        const auto original = application->update_character("guide",
            {.provider = "test", .web_search = search}, epoch);
        EXPECT_EQ(original.web_search, search);
        const auto saved = application->update_character("guide", {
            .provider = "fireworks", .style = "sans-bold", .voice = voice.id,
            .reasoning_effort = "medium", .web_search = search,
            .web_search_tool = true,
        }, epoch);
        EXPECT_EQ(saved.provider, "fireworks");
        EXPECT_EQ(saved.style, "sans-bold");
        EXPECT_EQ(saved.voice, voice.id);
        EXPECT_EQ(saved.reasoning_effort, "medium");
        EXPECT_EQ(saved.web_search, std::nullopt);
        EXPECT_EQ(saved.web_search_tool, true);
    }

    application.reset();
    application = Application::open(make_command(workspace, database));
    epoch = application->context_epoch();
    const auto reloaded = application->get_character("guide", epoch);
    EXPECT_EQ(reloaded.provider, "fireworks");
    EXPECT_EQ(reloaded.style, "sans-bold");
    EXPECT_EQ(reloaded.voice, voice.id);
    EXPECT_EQ(reloaded.reasoning_effort, "medium");
    EXPECT_EQ(reloaded.web_search, std::nullopt);
    EXPECT_EQ(reloaded.web_search_tool, true);
}

TEST(ApplicationSettings, PersistsIndependentVoiceInstrumentationProviderAndEffort) {
    test::TestWorkspace workspace;
    const auto database = test::import_test_database(workspace.root());
    auto application = Application::open(make_command(workspace, database));
    auto epoch = application->context_epoch();
    const auto key = application->create_api_key({.display_name = "Fish", .value = "secret"}, epoch);
    const auto voice = application->create_voice(
        {.display_name = "Reader", .description = "Voice", .elevenlabs_voice_id = "voice"}, epoch);
    const auto provider = application->create_provider(
        {.display_name = "Instrumentation", .copy_from = "test"}, epoch);
    (void)application->save_web_search_settings({.query_provider = "test"}, epoch);
    auto saved = application->save_voice_output_settings({
        .url = "https://api.fish.audio/v1/tts", .model = "s2.1-pro", .api_key = key.id,
        .output_format = "mp3", .default_voice = voice.display_name,
        .instrumentation_provider = provider.id, .instrumentation_reasoning_effort = "high",
    }, epoch);
    EXPECT_EQ(saved.instrumentation_provider, provider.id);
    EXPECT_EQ(saved.instrumentation_reasoning_effort, "high");
    EXPECT_EQ(application->get_provider(provider.id, epoch).reasoning_effort, "none");
    EXPECT_EQ(application->get_provider(provider.id, epoch).used_by,
        (std::vector<std::string>{"Voice instrumentation"}));
    EXPECT_THROW(application->delete_provider(provider.id, epoch), ApplicationError);

    nlohmann::json body = saved;
    EXPECT_EQ(body["instrumentation_provider"], provider.id);
    EXPECT_EQ(parse_voice_output_settings(body).instrumentation_reasoning_effort, "high");
    body["instrumentation_reasoning_effort"] = "invalid";
    EXPECT_THROW(parse_voice_output_settings(body), std::invalid_argument);
    body["instrumentation_reasoning_effort"] = "minimal";
    EXPECT_THROW(parse_voice_output_settings(body), std::invalid_argument);
    body["instrumentation_reasoning_effort"] = nullptr;
    EXPECT_FALSE(parse_voice_output_settings(body).instrumentation_reasoning_effort);
    auto invalid = saved;
    invalid.instrumentation_provider = "missing";
    EXPECT_THROW((void)application->save_voice_output_settings(invalid, epoch), ApplicationError);

    application.reset();
    application = Application::open(make_command(workspace, database));
    epoch = application->context_epoch();
    const auto reloaded = application->get_voice_output_settings(epoch);
    ASSERT_TRUE(reloaded);
    EXPECT_EQ(reloaded->instrumentation_provider, provider.id);
    EXPECT_EQ(reloaded->instrumentation_reasoning_effort, "high");
    EXPECT_EQ(application->get_web_search_settings(epoch).query_provider, "test");

    saved.instrumentation_provider.clear();
    saved.instrumentation_reasoning_effort.reset();
    (void)application->save_voice_output_settings(saved, epoch);
    application->delete_provider(provider.id, epoch);
    application.reset();
    application = Application::open(make_command(workspace, database));
    const auto disabled = application->get_voice_output_settings(application->context_epoch());
    ASSERT_TRUE(disabled);
    EXPECT_TRUE(disabled->instrumentation_provider.empty());
    EXPECT_FALSE(disabled->instrumentation_reasoning_effort);
}

TEST(ApplicationSettings, TestsProvidersOnBackgroundWork) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    auto application = Application::open(make_command(workspace, database));
    const auto epoch = application->context_epoch();
    auto provider = application->get_provider("test", epoch);
    nlohmann::json body = provider_body(provider);
    const std::string stream =
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"OK\"}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n\n";
    MockHttpServer model_server({http_response("text/event-stream", stream)});
    model_server.start();
    body["host"] = "127.0.0.1";
    body["port"] = model_server.port();
    body["base_path"] = "";
    body["mode"] = "test";
    body["model"] = "candidate-model";
    body["https"] = false;
    body["api"] = "responses";
    body["timeout_s"] = 0;
    body["idle_timeout_s"] = 0;
    EXPECT_THROW(
        (void)application->update_provider("test", body, epoch), ApplicationError);

    auto reply = application->test_provider("test", body, epoch);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    std::optional<OperationReply::Result> result;
    while (std::chrono::steady_clock::now() < deadline) {
        result = reply->peek();
        if (result) break;
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(result);
    ASSERT_TRUE(std::holds_alternative<nlohmann::json>(*result));
    model_server.join();
    EXPECT_EQ(application->get_provider("test", epoch).model, "fake");
}

TEST(ApplicationSettings, ShutdownDeadlineBoundsUncooperativeReplyCallback) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    auto application = Application::open(make_command(workspace, database));
    const auto epoch = application->context_epoch();
    auto provider = application->get_provider("test", epoch);
    nlohmann::json body = provider_body(provider);
    const std::string stream =
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"OK\"}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n\n";
    MockHttpServer model_server({http_response("text/event-stream", stream)});
    body["host"] = "127.0.0.1";
    body["port"] = model_server.port();
    body["base_path"] = "";
    body["mode"] = "test";
    body["model"] = "candidate-model";
    body["https"] = false;
    body["api"] = "responses";
    body["timeout_s"] = 0;
    body["idle_timeout_s"] = 0;

    auto reply = application->test_provider("test", body, epoch);
    std::mutex mutex;
    std::condition_variable changed;
    bool callback_started = false;
    bool release = false;
    reply->set_ready_callback([&] {
        std::unique_lock lock(mutex);
        callback_started = true;
        changed.notify_all();
        changed.wait(lock, [&] { return release; });
    });
    model_server.start();
    bool reached_callback = false;
    {
        std::unique_lock lock(mutex);
        reached_callback = changed.wait_for(
            lock, 2s, [&] { return callback_started; });
    }
    EXPECT_TRUE(reached_callback);

    application->request_shutdown();
    const auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(application->join_shutdown(50ms));
    EXPECT_LT(std::chrono::steady_clock::now() - started, 500ms);

    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();
    model_server.join();
    EXPECT_TRUE(application->join_shutdown(2s));
}

TEST(ApplicationSettings, MigratesAppearanceKeysR2AndNonsecretRuntime) {
    test::TestWorkspace workspace;
    workspace.write_style(
        "serif",
        "font = \"serif\"\nstyle = \"italic\"\nweight = \"normal\"\n"
        "size = \"normal\"\ntext_color = \"normal\"\n");
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    auto application = Application::open(make_command(workspace, database));
    const auto epoch = application->context_epoch();

    const auto created_style = application->create_style("Quiet", epoch);
    EXPECT_EQ(created_style.display_name, "Quiet");
    application->delete_style(created_style.id, epoch);

    const auto created_voice = application->create_voice({
        .display_name = "Narrator",
        .description = "A test voice",
        .elevenlabs_voice_id = "voice_ref",
    }, epoch);
    EXPECT_EQ(created_voice.elevenlabs_voice_id, "voice_ref");
    application->delete_voice(created_voice.id, epoch);

    const auto key = application->create_api_key(
        {.display_name = "Voice", .value = "private-voice-secret"}, epoch);
    (void)application->save_voice_input_settings({
        .url = "https://api.openai.com/v1/realtime",
        .model = "gpt-4o-transcribe",
        .api_key = key.id,
        .delay = "low",
        .prompt = "",
    }, epoch);
    const auto runtime = application->get_voice_input_runtime(epoch);
    ASSERT_TRUE(runtime);
    EXPECT_EQ(runtime->model, "gpt-4o-transcribe");
    EXPECT_EQ(nlohmann::json(*runtime).dump().find("private-voice-secret"),
        std::string::npos);
    EXPECT_FALSE(nlohmann::json(*runtime).contains("api_key"));

    EXPECT_FALSE(application->capabilities().can_transfer_r2);
    const auto r2 = application->save_r2_storage({
        .display_name = "Backups",
        .url = "https://account.example/bucket",
        .access_key_id = "access-one",
        .secret_key = std::string("private-secret"),
    }, epoch);
    EXPECT_TRUE(r2.has_secret_key);
    EXPECT_EQ(nlohmann::json(r2).dump().find("private-secret"), std::string::npos);
    EXPECT_TRUE(application->capabilities().can_transfer_r2);
    EXPECT_TRUE(application->bootstrap().capabilities.can_transfer_r2);
    application->delete_r2_storage(epoch);
    EXPECT_FALSE(application->get_r2_storage(epoch));
    EXPECT_FALSE(application->capabilities().can_transfer_r2);

    const auto auth = application->openai_auth_status(epoch);
    EXPECT_EQ(auth.status, "signed_out");
}

TEST(ApplicationSettings, SavesXaiVoiceInputWithoutVoiceOutput) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    auto application = Application::open(make_command(workspace, database));
    const auto epoch = application->context_epoch();
    const auto key = application->create_api_key(
        {.display_name = "xAI", .value = "xai-secret"}, epoch);

    try {
        (void)application->save_voice_input_settings({
            .provider = "openai",
            .url = "wss://api.x.ai/v1/stt",
            .model = "gpt-live-transcribe",
            .api_key = key.id,
            .delay = "low",
            .prompt = "",
        }, epoch);
        FAIL() << "OpenAI save must report its URL requirement";
    } catch (const ApplicationError& error) {
        EXPECT_EQ(error.code, ErrorCode::invalid_argument);
        EXPECT_EQ(error.what(), std::string(openai_voice_input_url_message));
    }
    try {
        (void)application->save_voice_input_settings({
            .provider = "xai",
            .url = "https://api.openai.com/v1/realtime/calls",
            .model = "grok-voice-transcribe-2.0",
            .api_key = key.id,
            .delay = "low",
            .prompt = "",
        }, epoch);
        FAIL() << "xAI save must report its URL requirement";
    } catch (const ApplicationError& error) {
        EXPECT_EQ(error.code, ErrorCode::invalid_argument);
        EXPECT_EQ(error.what(), std::string(xai_voice_input_url_message));
    }

    const auto saved = application->save_voice_input_settings({
        .provider = "xai",
        .url = "wss://api.x.ai/v1/stt",
        .model = "grok-voice-transcribe-2.0",
        .api_key = key.id,
        .delay = "obsolete-delay-value",
        .prompt = "unused",
        .send_phrase = "your turn",
    }, epoch);
    EXPECT_EQ(saved.provider, "xai");
    EXPECT_EQ(saved.delay, "low");
    EXPECT_EQ(saved.prompt, "unused");
    EXPECT_EQ(saved.send_phrase, "your turn");
    EXPECT_FALSE(application->get_voice_output_settings(epoch));

    application->request_shutdown();
    EXPECT_TRUE(application->join_shutdown(2s));
    application.reset();

    auto reopened = Application::open(make_command(workspace, database));
    const auto loaded = reopened->get_voice_input_settings(reopened->context_epoch());
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->provider, "xai");
    EXPECT_EQ(loaded->url, "wss://api.x.ai/v1/stt");
    EXPECT_EQ(loaded->model, "grok-voice-transcribe-2.0");
    EXPECT_EQ(loaded->delay, "low");
    EXPECT_EQ(loaded->prompt, "unused");
    EXPECT_EQ(loaded->send_phrase, "your turn");
    EXPECT_FALSE(reopened->get_voice_output_settings(reopened->context_epoch()));
    const auto runtime = reopened->get_voice_input_runtime(reopened->context_epoch());
    ASSERT_TRUE(runtime);
    EXPECT_EQ(runtime->send_phrase, "your turn");
    EXPECT_EQ(runtime->provider, "xai");
    EXPECT_FALSE(nlohmann::json(*runtime).contains("api_key"));

    reopened->request_shutdown();
    EXPECT_TRUE(reopened->join_shutdown(2s));
}

} // namespace
} // namespace cha::app
