#include "web/application_runtime.h"

#include "support/test_workspace.h"
#include "support/mock_http_server.h"
#include "util/environment.h"
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

} // namespace
} // namespace cha::web
