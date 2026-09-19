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
using cha::MockHttpServer;
using cha::http_response;
using cha::web::ApplicationCommand;
using cha::web::ErrorCode;
using cha::web::load_configuration_directory;
using cha::web::find_vault;
using cha::web::VaultDefinition;
using cha::web::ConfigurationDirectory;
using cha::web::ConfigurationTransport;

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
        config_directory, ConfigurationTransport::native);
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

nlohmann::json provider_body(const cha::web::ProviderDetail& provider) {
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

    const auto listed = application->list_providers();
    ASSERT_FALSE(listed.empty());
    EXPECT_EQ(listed.front().id, "test");

    const auto created_key = application->create_api_key(
        {.display_name = "Router key", .value = "private-router-secret"});
    EXPECT_FALSE(created_key.id.empty());
    EXPECT_TRUE(created_key.has_value);
    EXPECT_EQ(created_key.display_name, "Router key");

    const auto listed_keys = application->list_api_keys();
    ASSERT_FALSE(listed_keys.empty());
    EXPECT_EQ(nlohmann::json(listed_keys).dump().find("private-router-secret"),
        std::string::npos);

    auto provider = application->get_provider("test");
    EXPECT_EQ(provider.model, "fake");
    nlohmann::json body = provider_body(provider);
    body["api_key"] = created_key.id;
    const auto updated = application->update_provider("test", body);
    EXPECT_EQ(updated.api_key, created_key.id);
    EXPECT_EQ(nlohmann::json(updated).dump().find("private-router-secret"),
        std::string::npos);

    EXPECT_THROW(
        application->delete_provider("test"),
        ApplicationError);

    const auto copied = application->create_provider(
        {.display_name = "Copy", .copy_from = "test"});
    EXPECT_EQ(copied.display_name, "Copy");
    application->delete_provider(copied.id);
}

TEST(ApplicationSettings, TestsProvidersOnBackgroundWork) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    auto application = Application::open(make_command(workspace, database));
    auto provider = application->get_provider("test");
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
        (void)application->update_provider("test", body), ApplicationError);

    auto reply = application->test_provider("test", body);
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
    EXPECT_EQ(application->get_provider("test").model, "fake");
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

    const auto created_style = application->create_style("Quiet");
    EXPECT_EQ(created_style.display_name, "Quiet");
    application->delete_style(created_style.id);

    const auto created_voice = application->create_voice({
        .display_name = "Narrator",
        .description = "A test voice",
        .elevenlabs_voice_id = "voice_ref",
    });
    EXPECT_EQ(created_voice.elevenlabs_voice_id, "voice_ref");
    application->delete_voice(created_voice.id);

    const auto key = application->create_api_key(
        {.display_name = "Voice", .value = "private-voice-secret"});
    (void)application->save_voice_input_settings({
        .url = "https://api.openai.com/v1/realtime",
        .model = "gpt-4o-transcribe",
        .api_key = key.id,
        .delay = "low",
        .prompt = "",
    });
    const auto runtime = application->get_voice_input_runtime();
    ASSERT_TRUE(runtime);
    EXPECT_EQ(runtime->model, "gpt-4o-transcribe");
    EXPECT_EQ(nlohmann::json(*runtime).dump().find("private-voice-secret"),
        std::string::npos);
    EXPECT_FALSE(nlohmann::json(*runtime).contains("api_key"));

    const auto r2 = application->save_r2_storage({
        .display_name = "Backups",
        .url = "https://account.example/bucket",
        .access_key_id = "access-one",
        .secret_key = std::string("private-secret"),
    });
    EXPECT_TRUE(r2.has_secret_key);
    EXPECT_EQ(nlohmann::json(r2).dump().find("private-secret"), std::string::npos);
    application->delete_r2_storage();
    EXPECT_FALSE(application->get_r2_storage());

    const auto auth = application->openai_auth_status();
    EXPECT_EQ(auth.status, "signed_out");
}

} // namespace
} // namespace cha::app
