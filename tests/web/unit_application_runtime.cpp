#include "web/application_runtime.h"

#include "support/test_workspace.h"
#include "support/mock_http_server.h"
#include "util/environment.h"
#include "util/private_filesystem.h"
#include "workspace/workspace_config_store.h"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

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
    const std::filesystem::path& database) {
    const std::filesystem::path application_root =
        workspace.root() / "runtime-assets";
    std::filesystem::create_directories(application_root / "web");
    std::ofstream(application_root / "web" / "index.html")
        << "<!doctype html><title>CHA</title>";
    return {
        .database = database,
        .root = application_root,
        .host = "127.0.0.1",
        .port = 0,
        .log_file = workspace.root() / "runtime.log",
        .log_level = "off",
    };
}

std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

std::filesystem::path openai_auth_path(const std::filesystem::path& database) {
    std::filesystem::path path = database;
    path += ".openai-auth.json";
    return path;
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

void write_runtime_auth(const std::filesystem::path& database) {
    create_private_file(
        openai_auth_path(database),
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
    ApplicationCommand command = make_command(workspace, database);
    command.modify = modify;
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
    ApplicationCommand command = make_command(local_workspace, database);
    command.modify = modified_workspace.root();
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
    auto runtime = ApplicationRuntime::open(
        make_command(workspace, database), "private-test-token");
    const int port = runtime->start();
    httplib::Client client("127.0.0.1", port);

    const auto rejected = client.Get("/api/v1/openai/auth");
    ASSERT_TRUE(rejected);
    EXPECT_EQ(rejected->status, 404);
    EXPECT_EQ(rejected->get_header_value("Cache-Control"), "no-store");
    EXPECT_FALSE(contains_runtime_secret(rejected->body));

    const auto status = client.Get("/api/v1/openai/auth", kRuntimeCookie);
    expect_auth_snapshot(status, "signed_out");
    EXPECT_FALSE(std::filesystem::exists(openai_auth_path(database)));

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
    write_runtime_auth(database);
    const std::string original = file_bytes(openai_auth_path(database));
    const std::filesystem::path modify = workspace.root() / "modify";
    const std::filesystem::path mirror = workspace.root() / "mirror";
    std::filesystem::create_directories(mirror);
    ApplicationCommand command = make_command(workspace, database);
    command.modify = modify;
    command.mirror = mirror;
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
    EXPECT_EQ(file_bytes(openai_auth_path(database)), original);
    EXPECT_EQ(file_bytes(database).find(runtime_access), std::string::npos);

    std::filesystem::remove(openai_auth_path(database));
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
    write_runtime_auth(database);
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
    expect_auth_snapshot(
        client.Get("/api/v1/openai/auth", kRuntimeCookie), "connected");

    r2.start();
    const R2DatabaseTransfer transferred = runtime->upload_database();
    r2.join();
    EXPECT_GT(transferred.byte_count, 0U);
    ASSERT_EQ(r2.requests().size(), 1U);
    EXPECT_FALSE(contains_runtime_secret(r2.requests().front()));
    EXPECT_TRUE(std::filesystem::is_regular_file(openai_auth_path(database)));
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
    write_runtime_auth(local);
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
    httplib::Client client("127.0.0.1", port);
    r2.start();
    (void)runtime->download_database();
    r2.join();
    EXPECT_TRUE(std::filesystem::is_regular_file(openai_auth_path(local)));
    expect_auth_snapshot(
        client.Get("/api/v1/openai/auth", kRuntimeCookie), "connected");
    runtime->shutdown();
}

TEST(ApplicationRuntime, DisconnectRemovesSyntheticCredentials) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    write_runtime_auth(database);
    auto runtime = ApplicationRuntime::open(
        make_command(workspace, database), "private-test-token");
    const int port = runtime->start();
    httplib::Client client("127.0.0.1", port);
    const auto disconnected = client.Post(
        "/api/v1/openai/auth/disconnect",
        kRuntimeCookie,
        "{}",
        "application/json");
    expect_auth_snapshot(disconnected, "signed_out");
    EXPECT_FALSE(std::filesystem::exists(openai_auth_path(database)));
    runtime->shutdown();
}

TEST(ApplicationRuntime, InvalidAuthFileStartsSignedOut) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    create_private_file(
        openai_auth_path(database),
        std::string("{\"access_token\":\"") + runtime_access + "\"}");
    auto runtime = ApplicationRuntime::open(
        make_command(workspace, database), "private-test-token");
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

} // namespace
} // namespace cha::web
