#include "web/application_runtime.h"

#include "session/session_lease.h"
#include "session/session_repository.h"
#include "session/sqlite_storage.h"
#include "session/workspace_session_database.h"
#include "support/test_workspace.h"
#include "support/mock_http_server.h"
#include "util/environment.h"
#include "util/logging.h"
#include "util/private_filesystem.h"
#include "util/toml_file.h"
#include "workspace/builtins.h"
#include "workspace/workspace.h"
#include "workspace/workspace_config_store.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <latch>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#ifndef _WIN32
#include "support/lease_test_process.h"
#include <sys/stat.h>
#endif

namespace cha::web {
namespace {

class ScopedEnvironmentVariable {
public:
    explicit ScopedEnvironmentVariable(std::string name)
        : name_(std::move(name)) {
        if (const char* value = std::getenv(name_.c_str())) previous_ = value;
    }

    ~ScopedEnvironmentVariable() {
        if (previous_) {
            (void)set_environment_variable(name_, *previous_);
        } else {
            (void)unset_environment_variable(name_);
        }
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
};

ApplicationCommand make_command(
    const test::TestWorkspace& workspace,
    const std::filesystem::path& database,
    std::optional<std::filesystem::path> modify = {},
    std::optional<std::filesystem::path> mirror = {}) {
    const std::filesystem::path config_directory =
        workspace.root() / "cha-config";
    std::filesystem::create_directories(config_directory);
    {
        std::ofstream app(config_directory / "app.toml");
        app << "vault = \"Test\"\n"
            << "[web]\nhost = \"127.0.0.1\"\nport = 0\n"
            << "[logging]\nfile = \"runtime.log\"\nlevel = \"off\"\n";
    }
    {
        std::ofstream vault(config_directory / "test.toml");
        vault << "vault_name = \"Test\"\n"
              << "data = " << std::quoted(database.string()) << "\n";
        if (modify) {
            vault << "modify = " << std::quoted(modify->string()) << "\n";
        }
        if (mirror) {
            vault << "mirror = " << std::quoted(mirror->string()) << "\n";
        }
    }
    const ConfigurationDirectory loaded =
        load_configuration_directory(config_directory);
    const VaultDefinition* const vault =
        find_vault(loaded.vaults, loaded.startup_vault);
    const std::filesystem::path application_root =
        workspace.root() / "runtime-assets";
    std::filesystem::create_directories(application_root / "web");
    std::ofstream(application_root / "web" / "index.html")
        << "<!doctype html><title>CHA</title>";
    return {
        .config_directory = loaded.directory,
        .vaults = loaded.vaults,
        .vault = *vault,
        .root = application_root,
        .host = "127.0.0.1",
        .port = 0,
        .log_file = loaded.log_file,
        .log_level = loaded.log_level,
    };
}

std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

std::filesystem::path openai_auth_path(
    const std::filesystem::path& config_directory) {
    return config_directory / "openai-auth.json";
}

constexpr const char runtime_access[] = "runtime-access-secret";
constexpr const char runtime_refresh[] = "runtime-refresh-secret";
constexpr const char runtime_account[] = "acct_runtime";

const httplib::Headers kRuntimeCookie{
    {"Cookie", "CHA_RUNTIME=private-test-token"}};

bool contains_runtime_secret(std::string_view text) {
    return text.find(runtime_access) != std::string_view::npos
        || text.find(runtime_refresh) != std::string_view::npos
        || text.find(runtime_account) != std::string_view::npos;
}

void write_runtime_auth(const std::filesystem::path& config_directory) {
    create_private_file(
        openai_auth_path(config_directory),
        nlohmann::json{
            {"access_token", runtime_access},
            {"refresh_token", runtime_refresh},
            {"expires_at", 2'000'000'000},
            {"account_id", runtime_account},
        }.dump());
}

void expect_auth_snapshot(
    const httplib::Result& result,
    std::string_view status) {
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, 200) << result->body;
    EXPECT_EQ(result->get_header_value("Cache-Control"), "no-store");
    EXPECT_FALSE(contains_runtime_secret(result->body));
    const auto json = nlohmann::json::parse(result->body);
    EXPECT_EQ(json.at("status").get<std::string>(), status);
    EXPECT_FALSE(json.contains("access_token"));
    EXPECT_FALSE(json.contains("refresh_token"));
    EXPECT_FALSE(json.contains("account_id"));
}

TEST(ApplicationRuntime, UsesEphemeralPortAndRequiresPrivateCookie) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    const ApplicationCommand command = make_command(workspace, database);
    auto runtime = ApplicationRuntime::open(command, "private-test-token");
    const int port = runtime->start();
    EXPECT_GT(port, 0);

    httplib::Client client("127.0.0.1", port);
    const auto rejected = client.Get("/health");
    ASSERT_TRUE(rejected);
    EXPECT_EQ(rejected->status, 404);

    const auto accepted = client.Get(
        "/health", httplib::Headers{
            {"Cookie", "CHA_RUNTIME=private-test-token"}});
    ASSERT_TRUE(accepted);
    EXPECT_EQ(accepted->status, 200);

    runtime->shutdown();
}

TEST(ApplicationRuntime, UploadsInProcessAndResumesAnOpenSession) {
    ScopedEnvironmentVariable url("CHA_R2_URL");
    ScopedEnvironmentVariable access("CHA_R2_ACCESS_KEY_ID");
    ScopedEnvironmentVariable secret("CHA_R2_SECRET_ACCESS_KEY");
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    MockHttpServer r2({http_response("application/xml", "")});
    ASSERT_TRUE(set_environment_variable(
        "CHA_R2_URL",
        "http://127.0.0.1:" + std::to_string(r2.port()) + "/backups"));
    ASSERT_TRUE(set_environment_variable("CHA_R2_ACCESS_KEY_ID", "access"));
    ASSERT_TRUE(set_environment_variable("CHA_R2_SECRET_ACCESS_KEY", "secret"));

    auto runtime = ApplicationRuntime::open(
        make_command(workspace, database), "private-test-token");
    const int port = runtime->start();
    httplib::Client client("127.0.0.1", port);
    const httplib::Headers headers{
        {"Cookie", "CHA_RUNTIME=private-test-token"}};
    const auto created = client.Post(
        "/api/v1/forums/lobby/sessions",
        headers,
        R"({"label":"Before upload"})",
        "application/json");
    ASSERT_TRUE(created);
    ASSERT_EQ(created->status, 201) << created->body;
    const std::string session_id =
        nlohmann::json::parse(created->body).at("id");
    const std::string open_path =
        "/api/v1/forums/lobby/sessions/" + session_id + "/open";
    const auto opened = client.Post(
        open_path, headers, "{}", "application/json");
    ASSERT_TRUE(opened);
    ASSERT_EQ(opened->status, 200) << opened->body;

    r2.start();
    const R2DatabaseTransfer transferred = runtime->upload_database();
    r2.join();
    EXPECT_GT(transferred.byte_count, 0U);
    ASSERT_EQ(r2.requests().size(), 1U);
    EXPECT_TRUE(r2.requests().front().starts_with(
        "PUT /backups/workspace.sqlite3 HTTP/1.1"));

    const auto reopened = client.Post(
        open_path, headers, "{}", "application/json");
    ASSERT_TRUE(reopened);
    EXPECT_EQ(reopened->status, 200) << reopened->body;
    runtime->shutdown();
}

TEST(ApplicationRuntime, DownloadsInProcessAndPublishesTheNewWorkspace) {
    ScopedEnvironmentVariable url("CHA_R2_URL");
    ScopedEnvironmentVariable access("CHA_R2_ACCESS_KEY_ID");
    ScopedEnvironmentVariable secret("CHA_R2_SECRET_ACCESS_KEY");
    test::TestWorkspace local_workspace;
    test::TestWorkspace remote_workspace;
    remote_workspace.add_persona("remote", "Remote Persona");
    const std::filesystem::path local =
        test::import_test_database(local_workspace.root());
    const std::filesystem::path remote =
        test::import_test_database(remote_workspace.root());
    MockHttpServer r2({http_response(
        "application/vnd.sqlite3", file_bytes(remote))});
    ASSERT_TRUE(set_environment_variable(
        "CHA_R2_URL",
        "http://127.0.0.1:" + std::to_string(r2.port()) + "/backups"));
    ASSERT_TRUE(set_environment_variable("CHA_R2_ACCESS_KEY_ID", "access"));
    ASSERT_TRUE(set_environment_variable("CHA_R2_SECRET_ACCESS_KEY", "secret"));

    auto runtime = ApplicationRuntime::open(
        make_command(local_workspace, local), "private-test-token");
    const int port = runtime->start();
    r2.start();
    const R2DatabaseTransfer transferred = runtime->download_database();
    r2.join();
    EXPECT_GT(transferred.byte_count, 0U);
    std::filesystem::path backup = local;
    backup += ".bac";
    EXPECT_TRUE(std::filesystem::is_regular_file(backup));

    httplib::Client client("127.0.0.1", port);
    const auto bootstrap = client.Get(
        "/api/v1/bootstrap", httplib::Headers{
            {"Cookie", "CHA_RUNTIME=private-test-token"}});
    ASSERT_TRUE(bootstrap);
    ASSERT_EQ(bootstrap->status, 200) << bootstrap->body;
    const auto personas = nlohmann::json::parse(bootstrap->body).at("personas");
    EXPECT_TRUE(std::ranges::any_of(personas, [](const auto& persona) {
        return persona.at("id") == "remote";
    }));
    runtime->shutdown();
}

TEST(ApplicationRuntime, ExportsInProcessAndReplacesModifyDirectory) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    const std::filesystem::path modify = workspace.root() / "modify";
    std::filesystem::create_directories(modify);
    std::ofstream(modify / "stale.txt") << "stale";
    ApplicationCommand command = make_command(workspace, database, modify);
    auto runtime = ApplicationRuntime::open(command, "private-test-token");
    (void)runtime->start();

    const WorkspaceConfigTransfer transferred =
        runtime->export_configuration();
    EXPECT_GT(transferred.file_count, 0U);
    EXPECT_FALSE(std::filesystem::exists(modify / "stale.txt"));
    EXPECT_TRUE(std::filesystem::is_regular_file(
        modify / "forums" / "lobby" / "config.toml"));
    runtime->shutdown();
}

TEST(ApplicationRuntime, ImportsInProcessAndPublishesTheNewWorkspace) {
    test::TestWorkspace local_workspace;
    test::TestWorkspace modified_workspace;
    modified_workspace.add_persona("modified", "Modified Persona");
    const std::filesystem::path database =
        test::import_test_database(local_workspace.root());
    ApplicationCommand command = make_command(
        local_workspace, database, modified_workspace.root());
    auto runtime = ApplicationRuntime::open(command, "private-test-token");
    const int port = runtime->start();

    const WorkspaceConfigTransfer transferred =
        runtime->import_configuration();
    EXPECT_GT(transferred.file_count, 0U);

    httplib::Client client("127.0.0.1", port);
    const auto bootstrap = client.Get(
        "/api/v1/bootstrap", httplib::Headers{
            {"Cookie", "CHA_RUNTIME=private-test-token"}});
    ASSERT_TRUE(bootstrap);
    ASSERT_EQ(bootstrap->status, 200) << bootstrap->body;
    const auto personas = nlohmann::json::parse(bootstrap->body).at("personas");
    EXPECT_TRUE(std::ranges::any_of(personas, [](const auto& persona) {
        return persona.at("id") == "modified";
    }));
    runtime->shutdown();
}

TEST(ApplicationRuntime, AFailedReopenIsFatalAndRefusesLaterTransfers) {
    ScopedEnvironmentVariable url("CHA_R2_URL");
    ScopedEnvironmentVariable access("CHA_R2_ACCESS_KEY_ID");
    ScopedEnvironmentVariable secret("CHA_R2_SECRET_ACCESS_KEY");
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    MockHttpServer r2({http_response("application/xml", "")});
    ASSERT_TRUE(set_environment_variable(
        "CHA_R2_URL",
        "http://127.0.0.1:" + std::to_string(r2.port()) + "/backups"));
    ASSERT_TRUE(set_environment_variable("CHA_R2_ACCESS_KEY_ID", "access"));
    ASSERT_TRUE(set_environment_variable("CHA_R2_SECRET_ACCESS_KEY", "secret"));

    auto runtime = ApplicationRuntime::open(
        make_command(workspace, database), "private-test-token");
    (void)runtime->start();

    r2.start();
    force_next_workspace_config_fault(WorkspaceConfigFault::restore);
    EXPECT_THROW(
        (void)runtime->upload_database(), WorkspaceRestartRequiredError);
    r2.join();

    // The store is closed for good, so the runtime refuses to try again
    // instead of dereferencing a database handle it no longer has.
    EXPECT_THROW(
        (void)runtime->upload_database(), WorkspaceRestartRequiredError);
    runtime->shutdown();
}

TEST(ApplicationRuntime, AuthRoutesUseTheCookieGateAndStartSignedOut) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    const ApplicationCommand command = make_command(workspace, database);
    auto runtime = ApplicationRuntime::open(command, "private-test-token");
    const int port = runtime->start();
    httplib::Client client("127.0.0.1", port);

    const auto rejected = client.Get("/api/v1/openai/auth");
    ASSERT_TRUE(rejected);
    EXPECT_EQ(rejected->status, 404);
    EXPECT_EQ(rejected->get_header_value("Cache-Control"), "no-store");
    EXPECT_FALSE(contains_runtime_secret(rejected->body));

    const auto status = client.Get("/api/v1/openai/auth", kRuntimeCookie);
    expect_auth_snapshot(status, "signed_out");
    EXPECT_FALSE(std::filesystem::exists(openai_auth_path(command.config_directory)));

    const auto created = client.Post(
        "/api/v1/forums/lobby/sessions",
        kRuntimeCookie,
        R"({"label":"Unsigned"})",
        "application/json");
    ASSERT_TRUE(created);
    ASSERT_EQ(created->status, 201) << created->body;
    const std::string session_id =
        nlohmann::json::parse(created->body).at("id");
    const auto opened = client.Post(
        "/api/v1/forums/lobby/sessions/" + session_id + "/open",
        kRuntimeCookie,
        "{}",
        "application/json");
    ASSERT_TRUE(opened);
    EXPECT_EQ(opened->status, 200) << opened->body;
    runtime->shutdown();
}

TEST(ApplicationRuntime, LoadsSyntheticCredentialsAndLeavesThemThroughMaintenance) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    const std::filesystem::path modify = workspace.root() / "modify";
    const std::filesystem::path mirror = workspace.root() / "mirror";
    std::filesystem::create_directories(mirror);
    ApplicationCommand command =
        make_command(workspace, database, modify, mirror);
    write_runtime_auth(command.config_directory);
    const std::string original =
        file_bytes(openai_auth_path(command.config_directory));
    command.log_level = "debug";
    auto runtime = ApplicationRuntime::open(command, "private-test-token");
    const int port = runtime->start();
    httplib::Client client("127.0.0.1", port);

    expect_auth_snapshot(
        client.Get("/api/v1/openai/auth", kRuntimeCookie), "connected");

    const WorkspaceConfigTransfer exported = runtime->export_configuration();
    EXPECT_GT(exported.file_count, 0U);
    EXPECT_FALSE(std::filesystem::exists(
        modify / "workspace.sqlite3.openai-auth.json"));
    EXPECT_EQ(file_bytes(openai_auth_path(command.config_directory)), original);
    EXPECT_EQ(file_bytes(database).find(runtime_access), std::string::npos);

    std::filesystem::remove(openai_auth_path(command.config_directory));
    const WorkspaceConfigTransfer imported = runtime->import_configuration();
    EXPECT_GT(imported.file_count, 0U);
    expect_auth_snapshot(
        client.Get("/api/v1/openai/auth", kRuntimeCookie), "connected");

    for (const auto& entry : std::filesystem::recursive_directory_iterator(mirror)) {
        if (!entry.is_regular_file()) continue;
        EXPECT_FALSE(contains_runtime_secret(file_bytes(entry.path())))
            << entry.path();
    }
    EXPECT_FALSE(contains_runtime_secret(file_bytes(command.log_file)));
    runtime->shutdown();
}

TEST(ApplicationRuntime, UploadsTheDatabaseWithoutTheAuthFile) {
    ScopedEnvironmentVariable url("CHA_R2_URL");
    ScopedEnvironmentVariable access("CHA_R2_ACCESS_KEY_ID");
    ScopedEnvironmentVariable secret("CHA_R2_SECRET_ACCESS_KEY");
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    const ApplicationCommand command = make_command(workspace, database);
    write_runtime_auth(command.config_directory);
    MockHttpServer r2({http_response("application/xml", "")});
    ASSERT_TRUE(set_environment_variable(
        "CHA_R2_URL",
        "http://127.0.0.1:" + std::to_string(r2.port()) + "/backups"));
    ASSERT_TRUE(set_environment_variable("CHA_R2_ACCESS_KEY_ID", "access"));
    ASSERT_TRUE(set_environment_variable("CHA_R2_SECRET_ACCESS_KEY", "secret"));

    auto runtime = ApplicationRuntime::open(command, "private-test-token");
    const int port = runtime->start();
    httplib::Client client("127.0.0.1", port);
    expect_auth_snapshot(
        client.Get("/api/v1/openai/auth", kRuntimeCookie), "connected");

    r2.start();
    const R2DatabaseTransfer transferred = runtime->upload_database();
    r2.join();
    EXPECT_GT(transferred.byte_count, 0U);
    ASSERT_EQ(r2.requests().size(), 1U);
    EXPECT_FALSE(contains_runtime_secret(r2.requests().front()));
    EXPECT_TRUE(std::filesystem::is_regular_file(
        openai_auth_path(command.config_directory)));
    expect_auth_snapshot(
        client.Get("/api/v1/openai/auth", kRuntimeCookie), "connected");
    runtime->shutdown();
}

TEST(ApplicationRuntime, DownloadLeavesTheAuthOwnerConnected) {
    ScopedEnvironmentVariable url("CHA_R2_URL");
    ScopedEnvironmentVariable access("CHA_R2_ACCESS_KEY_ID");
    ScopedEnvironmentVariable secret("CHA_R2_SECRET_ACCESS_KEY");
    test::TestWorkspace local_workspace;
    test::TestWorkspace remote_workspace;
    remote_workspace.add_persona("remote", "Remote Persona");
    const std::filesystem::path local =
        test::import_test_database(local_workspace.root());
    const std::filesystem::path remote =
        test::import_test_database(remote_workspace.root());
    const ApplicationCommand command = make_command(local_workspace, local);
    write_runtime_auth(command.config_directory);
    MockHttpServer r2({http_response(
        "application/vnd.sqlite3", file_bytes(remote))});
    ASSERT_TRUE(set_environment_variable(
        "CHA_R2_URL",
        "http://127.0.0.1:" + std::to_string(r2.port()) + "/backups"));
    ASSERT_TRUE(set_environment_variable("CHA_R2_ACCESS_KEY_ID", "access"));
    ASSERT_TRUE(set_environment_variable("CHA_R2_SECRET_ACCESS_KEY", "secret"));

    auto runtime = ApplicationRuntime::open(command, "private-test-token");
    const int port = runtime->start();
    httplib::Client client("127.0.0.1", port);
    r2.start();
    (void)runtime->download_database();
    r2.join();
    EXPECT_TRUE(std::filesystem::is_regular_file(
        openai_auth_path(command.config_directory)));
    expect_auth_snapshot(
        client.Get("/api/v1/openai/auth", kRuntimeCookie), "connected");
    runtime->shutdown();
}

TEST(ApplicationRuntime, DisconnectRemovesSyntheticCredentials) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    const ApplicationCommand command = make_command(workspace, database);
    write_runtime_auth(command.config_directory);
    auto runtime = ApplicationRuntime::open(command, "private-test-token");
    const int port = runtime->start();
    httplib::Client client("127.0.0.1", port);
    const auto disconnected = client.Post(
        "/api/v1/openai/auth/disconnect",
        kRuntimeCookie,
        "{}",
        "application/json");
    expect_auth_snapshot(disconnected, "signed_out");
    EXPECT_FALSE(std::filesystem::exists(
        openai_auth_path(command.config_directory)));
    runtime->shutdown();
}

TEST(ApplicationRuntime, InvalidAuthFileStartsSignedOut) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    const ApplicationCommand command = make_command(workspace, database);
    create_private_file(
        openai_auth_path(command.config_directory),
        std::string("{\"access_token\":\"") + runtime_access + "\"}");
    auto runtime = ApplicationRuntime::open(command, "private-test-token");
    const int port = runtime->start();
    httplib::Client client("127.0.0.1", port);
    const auto status = client.Get("/api/v1/openai/auth", kRuntimeCookie);
    expect_auth_snapshot(status, "signed_out");
    const auto json = nlohmann::json::parse(status->body);
    ASSERT_TRUE(json.contains("error"));
    EXPECT_TRUE(json.at("error").is_string());
    EXPECT_FALSE(contains_runtime_secret(json.at("error").get<std::string>()));
    runtime->shutdown();
}

TEST(ApplicationRuntime, OpensTheConfiguredVaultAndFailsIfItsDatabaseIsMissing) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    const ApplicationCommand command = make_command(workspace, database);
    auto runtime = ApplicationRuntime::open(command, "private-test-token");
    EXPECT_EQ(runtime->current_vault().name, "Test");
    EXPECT_EQ(runtime->current_vault().data, command.vault.data);
    runtime->shutdown();

    const std::filesystem::path missing = workspace.root() / "missing.sqlite3";
    const ApplicationCommand absent = make_command(workspace, missing);
    EXPECT_THROW(
        (void)ApplicationRuntime::open(absent, "private-test-token"),
        std::runtime_error);
}

TEST(ApplicationRuntime, LoadsConfigDirectoryDotenvAndIgnoresDatabaseAdjacent) {
    constexpr char variable[] = "CHA_RUNTIME_CONFIG_DOTENV_E8F1";
    ScopedEnvironmentVariable guard(variable);
    ASSERT_TRUE(unset_environment_variable(variable));
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    const ApplicationCommand command = make_command(workspace, database);
    std::ofstream(command.config_directory / ".env")
        << "CHA_RUNTIME_CONFIG_DOTENV_E8F1=from-config\n";
    std::ofstream(database.parent_path() / ".env")
        << "CHA_RUNTIME_CONFIG_DOTENV_E8F1=from-database\n";
    auto runtime = ApplicationRuntime::open(command, "private-test-token");
    EXPECT_STREQ(std::getenv(variable), "from-config");
    runtime->shutdown();
}

void seed_lobby_session(
    const std::filesystem::path& database,
    std::string_view label) {
    auto store = WorkspaceConfigStore::open(database);
    SessionRepository repository(
        store->database_path(),
        store->workspace_path(),
        store->welcome_path(),
        TemporarySessionSeed{
            {std::string(entrance_id), std::string(welcome_id)},
            std::string(welcome_name)});
    (void)repository.create("lobby", std::string(label));
}

struct TwoVaultRuntime {
    explicit TwoVaultRuntime(
        bool with_mirrors = false,
        bool with_modify = false) {
        workspace_a.add_persona("alpha", "Alpha");
        workspace_b.add_persona("beta", "Beta");
        database_a = workspace_a.root() / "a.sqlite3";
        database_b = workspace_b.root() / "b.sqlite3";
        (void)test::import_test_database(workspace_a.root(), database_a);
        (void)test::import_test_database(workspace_b.root(), database_b);
        modify_a = workspace_a.root() / "modify-a";
        modify_b = workspace_b.root() / "modify-b";
        mirror_a = workspace_a.root() / "mirror-a";
        mirror_b = workspace_b.root() / "mirror-b";
        if (with_modify) {
            std::filesystem::create_directories(modify_a);
            std::filesystem::create_directories(modify_b);
        }
        if (with_mirrors) {
            std::filesystem::create_directories(mirror_a);
            std::filesystem::create_directories(mirror_b);
        }

        const std::filesystem::path config_directory =
            workspace_a.root() / "cha-config";
        std::filesystem::create_directories(config_directory);
        {
            std::ofstream app(config_directory / "app.toml");
            app << "vault = \"A\"\n"
                << "[web]\nhost = \"127.0.0.1\"\nport = 0\n"
                << "[logging]\nfile = \"runtime.log\"\nlevel = \"off\"\n";
        }
        auto write_vault = [&](
                               std::string_view file,
                               std::string_view name,
                               const std::filesystem::path& data,
                               const std::optional<std::filesystem::path>& modify,
                               const std::optional<std::filesystem::path>& mirror) {
            std::ofstream vault(config_directory / std::string(file));
            vault << "vault_name = \"" << name << "\"\n"
                  << "data = " << std::quoted(data.string()) << "\n";
            if (modify) {
                vault << "modify = " << std::quoted(modify->string()) << "\n";
            }
            if (mirror) {
                vault << "mirror = " << std::quoted(mirror->string()) << "\n";
            }
        };
        write_vault(
            "a.toml",
            "A",
            database_a,
            with_modify ? std::optional{modify_a} : std::nullopt,
            with_mirrors ? std::optional{mirror_a} : std::nullopt);
        write_vault(
            "b.toml",
            "B",
            database_b,
            with_modify ? std::optional{modify_b} : std::nullopt,
            with_mirrors ? std::optional{mirror_b} : std::nullopt);

        const ConfigurationDirectory loaded =
            load_configuration_directory(config_directory);
        const VaultDefinition* const vault =
            find_vault(loaded.vaults, loaded.startup_vault);
        const std::filesystem::path application_root =
            workspace_a.root() / "runtime-assets";
        std::filesystem::create_directories(application_root / "web");
        std::ofstream(application_root / "web" / "index.html")
            << "<!doctype html><title>CHA</title>";
        command = {
            .config_directory = loaded.directory,
            .vaults = loaded.vaults,
            .vault = *vault,
            .root = application_root,
            .host = "127.0.0.1",
            .port = 0,
            .log_file = loaded.log_file,
            .log_level = loaded.log_level,
        };
    }

    test::TestWorkspace workspace_a;
    test::TestWorkspace workspace_b;
    std::filesystem::path database_a;
    std::filesystem::path database_b;
    std::filesystem::path modify_a;
    std::filesystem::path modify_b;
    std::filesystem::path mirror_a;
    std::filesystem::path mirror_b;
    ApplicationCommand command;
};

bool bootstrap_has_persona(const nlohmann::json& body, std::string_view id) {
    for (const auto& persona : body.at("personas")) {
        if (persona.at("id").get<std::string>() == id) return true;
    }
    return false;
}

std::string create_lobby_session(
    httplib::Client& client,
    std::string_view label) {
    const auto created = client.Post(
        "/api/v1/forums/lobby/sessions",
        kRuntimeCookie,
        nlohmann::json{{"label", label}}.dump(),
        "application/json");
    if (!created || created->status != 201) return {};
    return nlohmann::json::parse(created->body).at("id").get<std::string>();
}

bool open_lobby_session(httplib::Client& client, const std::string& session_id) {
    const auto opened = client.Post(
        "/api/v1/forums/lobby/sessions/" + session_id + "/open",
        kRuntimeCookie,
        "{}",
        "application/json");
    return opened && opened->status == 200;
}

nlohmann::json get_bootstrap(httplib::Client& client) {
    const auto bootstrap = client.Get("/api/v1/bootstrap", kRuntimeCookie);
    if (!bootstrap || bootstrap->status != 200) return {};
    return nlohmann::json::parse(bootstrap->body);
}

TEST(ApplicationRuntime, InheritedEnvironmentWinsOverConfigDotenv) {
    constexpr char variable[] = "CHA_RUNTIME_CONFIG_DOTENV_E8F2";
    ScopedEnvironmentVariable guard(variable);
    ASSERT_TRUE(set_environment_variable(variable, "from-process"));
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    const ApplicationCommand command = make_command(workspace, database);
    std::ofstream(command.config_directory / ".env")
        << "CHA_RUNTIME_CONFIG_DOTENV_E8F2=from-config\n";
    auto runtime = ApplicationRuntime::open(command, "private-test-token");
    EXPECT_STREQ(std::getenv(variable), "from-process");
    runtime->shutdown();
}

TEST(ApplicationRuntime, SwitchVaultAToBToAKeepsOriginAndStoredSessions) {
    TwoVaultRuntime pair;
    seed_lobby_session(pair.database_a, "Stored on A");
    auto runtime = ApplicationRuntime::open(pair.command, "private-test-token");
    const int port = runtime->start();
    httplib::Client client("127.0.0.1", port);
    const std::string live = create_lobby_session(client, "Live on A");
    ASSERT_FALSE(live.empty());
    ASSERT_TRUE(open_lobby_session(client, live));
    EXPECT_EQ(runtime->current_vault().name, "A");

    runtime->switch_vault("B");
    EXPECT_EQ(runtime->current_vault().name, "B");
    httplib::Client after("127.0.0.1", port);
    const auto bootstrap_b = get_bootstrap(after);
    EXPECT_TRUE(bootstrap_has_persona(bootstrap_b, "beta"));
    EXPECT_FALSE(bootstrap_has_persona(bootstrap_b, "alpha"));
    bool saw_stored_on_b = false;
    for (const auto& recent : bootstrap_b.at("recent_sessions")) {
        if (recent.at("session_label").get<std::string>() == "Stored on A") {
            saw_stored_on_b = true;
        }
    }
    EXPECT_FALSE(saw_stored_on_b);
    const std::string on_b = create_lobby_session(after, "Live on B");
    ASSERT_FALSE(on_b.empty());
    ASSERT_TRUE(open_lobby_session(after, on_b));

    runtime->switch_vault("A");
    EXPECT_EQ(runtime->current_vault().name, "A");
    httplib::Client back("127.0.0.1", port);
    const auto bootstrap_a = get_bootstrap(back);
    EXPECT_TRUE(bootstrap_has_persona(bootstrap_a, "alpha"));
    EXPECT_FALSE(bootstrap_has_persona(bootstrap_a, "beta"));
    bool saw_stored_on_a = false;
    for (const auto& recent : bootstrap_a.at("recent_sessions")) {
        if (recent.at("session_label").get<std::string>() == "Stored on A") {
            saw_stored_on_a = true;
        }
    }
    EXPECT_TRUE(saw_stored_on_a);
    const toml::table table =
        read_toml_file(pair.command.config_directory / "app.toml", "config file");
    EXPECT_EQ(table["vault"].value<std::string>(), "A");
    runtime->shutdown();
}

TEST(ApplicationRuntime, SameVaultSwitchIsANoOp) {
    TwoVaultRuntime pair;
    auto runtime = ApplicationRuntime::open(pair.command, "private-test-token");
    const int port = runtime->start();
    httplib::Client client("127.0.0.1", port);
    const std::string live = create_lobby_session(client, "Keep me");
    ASSERT_FALSE(live.empty());
    ASSERT_TRUE(open_lobby_session(client, live));
    const std::string original =
        file_bytes(pair.command.config_directory / "app.toml");

    runtime->switch_vault("a");
    EXPECT_EQ(runtime->current_vault().name, "A");
    EXPECT_EQ(file_bytes(pair.command.config_directory / "app.toml"), original);
    const auto listing = client.Get(
        "/api/v1/forums/lobby/sessions", kRuntimeCookie);
    ASSERT_TRUE(listing);
    ASSERT_EQ(listing->status, 200) << listing->body;
    bool running = false;
    for (const auto& session : nlohmann::json::parse(listing->body)) {
        if (session.at("id").get<std::string>() == live) {
            running = session.at("live").get<bool>();
        }
    }
    EXPECT_TRUE(running);
    runtime->shutdown();
}

TEST(ApplicationRuntime, UnknownAndInvalidTargetsFailBeforeDraining) {
    TwoVaultRuntime pair(true);
    auto runtime = ApplicationRuntime::open(pair.command, "private-test-token");
    const int port = runtime->start();
    httplib::Client client("127.0.0.1", port);
    const std::string live = create_lobby_session(client, "Still live");
    ASSERT_FALSE(live.empty());
    ASSERT_TRUE(open_lobby_session(client, live));
    const std::string original =
        file_bytes(pair.command.config_directory / "app.toml");

    EXPECT_THROW(runtime->switch_vault("missing"), UnknownVaultError);
    EXPECT_EQ(runtime->current_vault().name, "A");

    std::filesystem::remove(pair.database_b);
    EXPECT_THROW(runtime->switch_vault("B"), std::runtime_error);
    EXPECT_EQ(runtime->current_vault().name, "A");
    (void)test::import_test_database(pair.workspace_b.root(), pair.database_b);

    std::ofstream(pair.database_b, std::ios::binary | std::ios::trunc)
        << "not a database";
    EXPECT_THROW(runtime->switch_vault("B"), std::runtime_error);
    EXPECT_EQ(runtime->current_vault().name, "A");
    std::filesystem::remove(pair.database_b);
    (void)test::import_test_database(pair.workspace_b.root(), pair.database_b);

    {
        storage::SqliteDatabase database(
            pair.database_b, storage::SqliteDatabase::Mode::read_write);
        database.execute("DROP TABLE config");
        database.execute(
            "PRAGMA user_version = "
            + std::to_string(workspace_session_database_version_v1));
    }
    EXPECT_THROW(runtime->switch_vault("B"), std::runtime_error);
    EXPECT_EQ(runtime->current_vault().name, "A");
    (void)test::import_test_database(pair.workspace_b.root(), pair.database_b);

    EXPECT_EQ(file_bytes(pair.command.config_directory / "app.toml"), original);

    const auto listing = client.Get(
        "/api/v1/forums/lobby/sessions", kRuntimeCookie);
    ASSERT_TRUE(listing);
    bool running = false;
    for (const auto& session : nlohmann::json::parse(listing->body)) {
        if (session.at("id").get<std::string>() == live) {
            running = session.at("live").get<bool>();
        }
    }
    EXPECT_TRUE(running);
    runtime->shutdown();
}

TEST(ApplicationRuntime, DrainTimeoutLeavesOldVaultSelectedAndAllowsRetry) {
    TwoVaultRuntime pair;
    pair.command.test_shutdown_grace_ms = 0;
    auto runtime = ApplicationRuntime::open(pair.command, "private-test-token");
    const int port = runtime->start();
    httplib::Client client("127.0.0.1", port);
    const std::string live = create_lobby_session(client, "Needs draining");
    ASSERT_FALSE(live.empty());
    ASSERT_TRUE(open_lobby_session(client, live));
    const std::string original =
        file_bytes(pair.command.config_directory / "app.toml");

    std::string failure;
    try {
        runtime->switch_vault("B");
    } catch (const std::exception& error) {
        failure = error.what();
    }
    ASSERT_EQ(
        failure,
        "Could not pause active sessions for database maintenance");
    EXPECT_EQ(runtime->current_vault().name, "A");
    EXPECT_EQ(file_bytes(pair.command.config_directory / "app.toml"), original);
    const auto bootstrap_a = get_bootstrap(client);
    EXPECT_TRUE(bootstrap_has_persona(bootstrap_a, "alpha"));
    EXPECT_FALSE(bootstrap_has_persona(bootstrap_a, "beta"));

    bool switched = false;
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(2);
    while (!switched && std::chrono::steady_clock::now() < deadline) {
        try {
            runtime->switch_vault("B");
            switched = true;
        } catch (const std::runtime_error& error) {
            EXPECT_STREQ(
                error.what(),
                "Could not pause active sessions for database maintenance");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    ASSERT_TRUE(switched);
    EXPECT_EQ(runtime->current_vault().name, "B");
    runtime->shutdown();
}

#ifndef _WIN32
TEST(ApplicationRuntime, BusyTargetLeaseLeavesTheOldVaultRunning) {
    TwoVaultRuntime pair;
    auto runtime = ApplicationRuntime::open(pair.command, "private-test-token");
    const int port = runtime->start();
    httplib::Client client("127.0.0.1", port);
    const std::string live = create_lobby_session(client, "Still live");
    ASSERT_FALSE(live.empty());
    ASSERT_TRUE(open_lobby_session(client, live));
    const std::string original =
        file_bytes(pair.command.config_directory / "app.toml");

    test::LeaseHolderProcess holder(pair.database_b);
    EXPECT_THROW(runtime->switch_vault("B"), SessionBusyError);
    EXPECT_EQ(runtime->current_vault().name, "A");
    EXPECT_EQ(file_bytes(pair.command.config_directory / "app.toml"), original);
    const auto listing = client.Get(
        "/api/v1/forums/lobby/sessions", kRuntimeCookie);
    ASSERT_TRUE(listing);
    const auto sessions = nlohmann::json::parse(listing->body);
    const auto found = std::ranges::find_if(sessions, [&](const auto& session) {
        return session.at("id").template get<std::string>() == live;
    });
    ASSERT_NE(found, sessions.end());
    EXPECT_TRUE(found->at("live").template get<bool>());
    runtime->shutdown();
}
#endif

TEST(ApplicationRuntime, FailedSwitchReopenIsFatalAndRefusesLaterTransfers) {
    TwoVaultRuntime pair(false, true);
    auto runtime = ApplicationRuntime::open(pair.command, "private-test-token");
    const int port = runtime->start();
    const std::string original =
        file_bytes(pair.command.config_directory / "app.toml");
    httplib::Client client("127.0.0.1", port);
    force_next_workspace_config_fault(WorkspaceConfigFault::restore);
    const auto failed = client.Post(
        "/api/v1/vault/switch",
        kRuntimeCookie,
        nlohmann::json{{"vault_name", "B"}}.dump(),
        "application/json");
    ASSERT_TRUE(failed);
    EXPECT_EQ(failed->status, 500);
    EXPECT_NE(failed->body.find("Restart is required"), std::string::npos);
    EXPECT_EQ(runtime->current_vault().name, "A");
    EXPECT_EQ(file_bytes(pair.command.config_directory / "app.toml"), original);
    EXPECT_THROW(
        (void)runtime->export_configuration(), WorkspaceRestartRequiredError);
    httplib::Client after("127.0.0.1", port);
    EXPECT_FALSE(after.Get("/api/v1/bootstrap", kRuntimeCookie));
    runtime->shutdown();
}

TEST(ApplicationRuntime, MirroringSwitchesRootsAndSurvivesStoredSessions) {
    TwoVaultRuntime pair(true);
    seed_lobby_session(pair.database_b, "Stored on B");
    auto runtime = ApplicationRuntime::open(pair.command, "private-test-token");
    const int port = runtime->start();
    httplib::Client client("127.0.0.1", port);
    const std::string on_a = create_lobby_session(client, "Mirrored A");
    ASSERT_FALSE(on_a.empty());
    EXPECT_TRUE(std::filesystem::exists(
        pair.mirror_a / "The Lobby" / "Mirrored A.md"));

    runtime->switch_vault("B");
    EXPECT_TRUE(std::filesystem::exists(
        pair.mirror_b / "The Lobby" / "Stored on B.md"));
    httplib::Client after("127.0.0.1", port);
    const std::string on_b = create_lobby_session(after, "Mirrored B");
    ASSERT_FALSE(on_b.empty());
    EXPECT_TRUE(std::filesystem::exists(
        pair.mirror_b / "The Lobby" / "Mirrored B.md"));

    runtime->switch_vault("A");
    EXPECT_TRUE(std::filesystem::exists(
        pair.mirror_a / "The Lobby" / "Mirrored A.md"));
    runtime->shutdown();
}

TEST(ApplicationRuntime, MirrorRebuildFailureKeepsSwitchedVaultRunning) {
    TwoVaultRuntime pair(true);
    auto runtime = ApplicationRuntime::open(pair.command, "private-test-token");
    const int port = runtime->start();
    httplib::Client client("127.0.0.1", port);

    ASSERT_TRUE(std::filesystem::remove(pair.mirror_b));
    std::ofstream(pair.mirror_b) << "not a directory";
    ASSERT_NO_THROW(runtime->switch_vault("B"));

    EXPECT_EQ(runtime->current_vault().name, "B");
    EXPECT_EQ(
        read_toml_file(pair.command.config_directory / "app.toml", "config file")
            ["vault"].value<std::string>(),
        "B");
    const auto bootstrap_b = get_bootstrap(client);
    EXPECT_TRUE(bootstrap_has_persona(bootstrap_b, "beta"));
    EXPECT_FALSE(bootstrap_has_persona(bootstrap_b, "alpha"));
    EXPECT_TRUE(std::filesystem::is_regular_file(pair.mirror_b));

    const std::string created = create_lobby_session(
        client, "After failed mirror rebuild");
    ASSERT_FALSE(created.empty());
    EXPECT_FALSE(std::filesystem::exists(
        pair.mirror_a / "The Lobby" / "After failed mirror rebuild.md"));
    runtime->shutdown();
}

TEST(ApplicationRuntime, PersistenceFailureKeepsSwitchedVaultRunning) {
    TwoVaultRuntime pair;
    auto runtime = ApplicationRuntime::open(pair.command, "private-test-token");
    const int port = runtime->start();
    httplib::Client client("127.0.0.1", port);
    const std::filesystem::path config = pair.command.config_directory;
    std::filesystem::path unavailable = config;
    unavailable += ".unavailable";
    const std::filesystem::path app =
        config / "app.toml";
    const std::string original = file_bytes(app);

    std::filesystem::rename(config, unavailable);
    try {
        runtime->switch_vault("B");
    } catch (...) {
        std::filesystem::rename(unavailable, config);
        throw;
    }
    std::filesystem::rename(unavailable, config);

    EXPECT_EQ(runtime->current_vault().name, "B");
    EXPECT_EQ(file_bytes(app), original);
    const auto bootstrap_b = get_bootstrap(client);
    EXPECT_TRUE(bootstrap_has_persona(bootstrap_b, "beta"));
    EXPECT_FALSE(bootstrap_has_persona(bootstrap_b, "alpha"));
    runtime->shutdown();

    const ConfigurationDirectory after_restart =
        load_configuration_directory(pair.command.config_directory);
    EXPECT_EQ(after_restart.startup_vault, "A");
}

TEST(ApplicationRuntime, MaintenanceAfterSwitchUsesTheCurrentVault) {
    TwoVaultRuntime pair(false, true);
    auto runtime = ApplicationRuntime::open(pair.command, "private-test-token");
    (void)runtime->start();

    runtime->switch_vault("B");
    const WorkspaceConfigTransfer exported = runtime->export_configuration();

    EXPECT_GT(exported.file_count, 0U);
    EXPECT_TRUE(std::filesystem::exists(
        pair.modify_b / "personas" / "beta" / "persona.toml"));
    EXPECT_FALSE(std::filesystem::exists(
        pair.modify_a / "personas" / "alpha" / "persona.toml"));
    runtime->shutdown();
}

httplib::Result post_switch(
    httplib::Client& client,
    std::string_view name,
    const httplib::Headers& headers = kRuntimeCookie) {
    return client.Post(
        "/api/v1/vault/switch",
        headers,
        nlohmann::json{{"vault_name", name}}.dump(),
        "application/json");
}

void expect_error_envelope(
    const httplib::Result& result,
    int status,
    std::string_view code) {
    ASSERT_TRUE(result);
    EXPECT_EQ(result->status, status);
    const auto json = nlohmann::json::parse(result->body);
    ASSERT_TRUE(json.contains("error"));
    EXPECT_EQ(json["error"]["code"].get<std::string>(), code);
    EXPECT_TRUE(json["error"]["message"].is_string());
    EXPECT_FALSE(json["error"]["message"].get<std::string>().empty());
}

TEST(ApplicationRuntime, SwitchRouteSwitchesVaultAndMapsUnknownNames) {
    TwoVaultRuntime pair;
    auto runtime = ApplicationRuntime::open(pair.command, "private-test-token");
    const int port = runtime->start();
    httplib::Client client("127.0.0.1", port);

    const auto switched = post_switch(client, "b");
    ASSERT_TRUE(switched);
    EXPECT_EQ(switched->status, 204) << switched->body;

    const auto bootstrap = get_bootstrap(client);
    EXPECT_EQ(bootstrap.at("vault_name").get<std::string>(), "B");
    EXPECT_EQ(bootstrap.at("vaults"), nlohmann::json::array({"A", "B"}));
    EXPECT_TRUE(bootstrap_has_persona(bootstrap, "beta"));
    EXPECT_EQ(
        read_toml_file(pair.command.config_directory / "app.toml", "config file")
            ["vault"].value<std::string>(),
        "B");

    expect_error_envelope(post_switch(client, "missing"), 400, "bad_request");
    runtime->shutdown();
}

} // namespace
} // namespace cha::web
