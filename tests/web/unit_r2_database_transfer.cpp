#include "web/r2_database_transfer.h"

#include "session/session_lease.h"
#include "session/workspace_session_database.h"
#include "support/mock_http_server.h"
#include "support/test_workspace.h"
#include "util/environment.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

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

class R2Environment {
public:
    explicit R2Environment(std::string url)
        : url_("CHA_R2_URL"),
          access_("CHA_R2_ACCESS_KEY_ID"),
          secret_("CHA_R2_SECRET_ACCESS_KEY") {
        EXPECT_TRUE(set_environment_variable("CHA_R2_URL", url));
        EXPECT_TRUE(set_environment_variable(
            "CHA_R2_ACCESS_KEY_ID", "test-access-key"));
        EXPECT_TRUE(set_environment_variable(
            "CHA_R2_SECRET_ACCESS_KEY", "test-secret-key"));
    }

private:
    ScopedEnvironmentVariable url_;
    ScopedEnvironmentVariable access_;
    ScopedEnvironmentVariable secret_;
};

std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

void write_bytes(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream(path, std::ios::binary) << bytes;
}

std::string mock_url(int port) {
    return "http://127.0.0.1:" + std::to_string(port)
        + "/cha-backups/";
}

TEST(R2DatabaseTransfer, UploadsAStableDatabaseWithSignedPut) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(
            workspace.root(), workspace.root() / "workspace copy.sqlite3");
    const std::string expected = file_bytes(database);
    MockHttpServer server({http_response("application/xml", "")});
    R2Environment environment(mock_url(server.port()));
    server.start();

    const R2DatabaseTransfer result = upload_database_to_r2(database);
    server.join();

    EXPECT_EQ(result.byte_count, expected.size());
    ASSERT_EQ(server.requests().size(), 1U);
    const std::string& request = server.requests().front();
    EXPECT_TRUE(request.starts_with(
        "PUT /cha-backups/workspace%20copy.sqlite3 HTTP/1.1"));
    EXPECT_NE(
        request.find(
            "Authorization: AWS4-HMAC-SHA256 Credential=test-access-key/"),
        std::string::npos);
    EXPECT_NE(request.find("SignedHeaders=host;x-amz-content-sha256;x-amz-date"),
              std::string::npos);
    EXPECT_NE(request.find("x-amz-content-sha256:"), std::string::npos);
    EXPECT_NE(request.find("x-amz-date:"), std::string::npos);
    EXPECT_EQ(request_body(request), expected);
}

TEST(R2DatabaseTransfer, DownloadsValidDatabaseAndReplacesOlderBackup) {
    test::TestWorkspace local_workspace;
    test::TestWorkspace remote_workspace;
    remote_workspace.add_persona("remote", "Remote Persona");
    const std::filesystem::path local =
        test::import_test_database(local_workspace.root());
    const std::filesystem::path remote =
        test::import_test_database(remote_workspace.root());
    const std::string local_bytes = file_bytes(local);
    const std::string remote_bytes = file_bytes(remote);
    std::filesystem::path backup = local;
    backup += ".bac";
    write_bytes(backup, "older backup");

    MockHttpServer server({http_response(
        "application/vnd.sqlite3", remote_bytes)});
    R2Environment environment(mock_url(server.port()));
    server.start();

    const R2DatabaseTransfer result = download_database_from_r2(local);
    server.join();

    EXPECT_EQ(result.byte_count, remote_bytes.size());
    EXPECT_EQ(file_bytes(local), remote_bytes);
    EXPECT_EQ(file_bytes(backup), local_bytes);
    EXPECT_EQ(
        inspect_workspace_session_database(local),
        WorkspaceDatabaseState::valid_v2);
    ASSERT_EQ(server.requests().size(), 1U);
    EXPECT_TRUE(server.requests().front().starts_with(
        "GET /cha-backups/workspace.sqlite3 HTTP/1.1"));
    EXPECT_NE(
        server.requests().front().find(
            "Authorization: AWS4-HMAC-SHA256 Credential=test-access-key/"),
        std::string::npos);
}

TEST(R2DatabaseTransfer, InvalidDownloadLeavesDatabaseAndBackupUntouched) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    const std::string original = file_bytes(database);
    std::filesystem::path backup = database;
    backup += ".bac";
    write_bytes(backup, "existing backup");

    MockHttpServer server({http_response(
        "application/octet-stream", "not a sqlite database")});
    R2Environment environment(mock_url(server.port()));
    server.start();

    EXPECT_THROW(
        (void)download_database_from_r2(database),
        std::runtime_error);
    server.join();

    EXPECT_EQ(file_bytes(database), original);
    EXPECT_EQ(file_bytes(backup), "existing backup");
}

TEST(R2DatabaseTransfer, RejectsMissingCredentialsAndAnActiveLease) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    ScopedEnvironmentVariable url("CHA_R2_URL");
    ScopedEnvironmentVariable access("CHA_R2_ACCESS_KEY_ID");
    ScopedEnvironmentVariable secret("CHA_R2_SECRET_ACCESS_KEY");
    ASSERT_TRUE(unset_environment_variable("CHA_R2_URL"));
    ASSERT_TRUE(unset_environment_variable("CHA_R2_ACCESS_KEY_ID"));
    ASSERT_TRUE(unset_environment_variable("CHA_R2_SECRET_ACCESS_KEY"));

    try {
        (void)upload_database_to_r2(database);
        FAIL() << "expected missing environment to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find("CHA_R2_URL"),
            std::string::npos);
    }

    SessionLease lease = SessionLease::acquire(database, "held by test");
    EXPECT_THROW(
        (void)download_database_from_r2(database),
        SessionBusyError);
}

} // namespace
} // namespace cha::web
