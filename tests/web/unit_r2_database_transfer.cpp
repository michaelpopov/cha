#include "web/r2_database_transfer.h"

#include "session/session_lease.h"
#include "session/workspace_session_database.h"
#include "support/mock_http_server.h"
#include "support/test_workspace.h"
#include "web/application_config.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <string>

namespace cha::web {
namespace {

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

R2StorageKey storage(std::string url) {
    return {
        .id = "api_key_1",
        .display_name = "R2",
        .url = std::move(url),
        .access_key_id = "test-access-key",
        .secret_key = "test-secret-key",
    };
}

std::filesystem::path write_vault(
    const std::filesystem::path& database,
    std::string_view name = "Test") {
    const std::filesystem::path path = database.parent_path() / "test.toml";
    std::ofstream output(path);
    output << "vault_name = " << std::quoted(std::string(name)) << "\n"
           << "data = " << std::quoted(database.string()) << "\n";
    return path;
}

TEST(R2DatabaseTransfer, UploadsDatabaseAndVaultDefinitionWithSignedPuts) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(
            workspace.root(), workspace.root() / "workspace copy.sqlite3");
    const std::filesystem::path vault = write_vault(database);
    const std::string expected_database = file_bytes(database);
    const std::string expected_vault = file_bytes(vault);
    MockHttpServer server({
        http_response("application/xml", ""),
        http_response("application/xml", ""),
    });
    const R2StorageKey key = storage(mock_url(server.port()));
    server.start();

    const R2DatabaseTransfer result =
        upload_database_to_r2(database, vault, key);
    server.join();

    EXPECT_EQ(
        result.byte_count, expected_database.size() + expected_vault.size());
    ASSERT_EQ(server.requests().size(), 2U);
    const std::string& vault_request = server.requests()[0];
    const std::string& database_request = server.requests()[1];
    EXPECT_TRUE(vault_request.starts_with(
        "PUT /cha-backups/workspace%20copy.sqlite3.toml HTTP/1.1"));
    EXPECT_EQ(request_body(vault_request), expected_vault);
    EXPECT_TRUE(database_request.starts_with(
        "PUT /cha-backups/workspace%20copy.sqlite3 HTTP/1.1"));
    EXPECT_NE(
        database_request.find(
            "Authorization: AWS4-HMAC-SHA256 Credential=test-access-key/"),
        std::string::npos);
    EXPECT_NE(
        database_request.find(
            "SignedHeaders=host;x-amz-content-sha256;x-amz-date"),
        std::string::npos);
    EXPECT_NE(
        database_request.find("x-amz-content-sha256:"),
        std::string::npos);
    EXPECT_NE(database_request.find("x-amz-date:"), std::string::npos);
    EXPECT_EQ(request_body(database_request), expected_database);
}

TEST(R2DatabaseTransfer, ListsRootSqliteDatabasesAcrossPages) {
    const std::string first =
        "<ListBucketResult>"
        "<IsTruncated>true</IsTruncated>"
        "<Contents><Key>zeta.sqlite3</Key></Contents>"
        "<Contents><Key>notes.txt</Key></Contents>"
        "<NextContinuationToken>next+token=</NextContinuationToken>"
        "</ListBucketResult>";
    const std::string second =
        "<ListBucketResult>"
        "<IsTruncated>false</IsTruncated>"
        "<Contents><Key>Alpha%20Vault.sqlite3</Key></Contents>"
        "<Contents><Key>folder%2Fhidden.sqlite3</Key></Contents>"
        "<Contents><Key>zeta.sqlite3.toml</Key></Contents>"
        "</ListBucketResult>";
    MockHttpServer server({
        http_response("application/xml", first),
        http_response("application/xml", second),
    });
    const R2StorageKey key = storage(mock_url(server.port()));
    server.start();

    const std::vector<std::string> names = list_r2_database_names(key);
    server.join();

    EXPECT_EQ(names, (std::vector<std::string>{"Alpha Vault", "zeta"}));
    ASSERT_EQ(server.requests().size(), 2U);
    EXPECT_TRUE(server.requests()[0].starts_with(
        "GET /cha-backups?encoding-type=url&list-type=2 HTTP/1.1"));
    EXPECT_TRUE(server.requests()[1].starts_with(
        "GET /cha-backups?continuation-token=next%2Btoken%3D&"
        "encoding-type=url&list-type=2 HTTP/1.1"));
    EXPECT_NE(
        server.requests()[0].find("Authorization: AWS4-HMAC-SHA256"),
        std::string::npos);
}

TEST(R2DatabaseTransfer, DownloadsANewDatabaseWithoutReplacingAnything) {
    test::TestWorkspace remote_workspace;
    remote_workspace.add_persona("remote", "Remote Persona");
    const std::filesystem::path remote =
        test::import_test_database(remote_workspace.root());
    const std::string remote_bytes = file_bytes(remote);
    const std::filesystem::path destination =
        remote_workspace.root() / "downloaded.sqlite3";
    const std::filesystem::path vault =
        remote_workspace.root() / "vault-2.toml";
    const std::string remote_vault =
        "vault_name = \"Remote Archive\"\n"
        "data = \"/another/computer/archive.sqlite3\"\n";
    std::filesystem::path stale_wal = destination;
    stale_wal += "-wal";
    write_bytes(stale_wal, "stale");
    MockHttpServer server({
        http_response("application/toml", remote_vault),
        http_response("application/vnd.sqlite3", remote_bytes),
    });
    const R2StorageKey key = storage(mock_url(server.port()));
    server.start();

    const R2DatabaseTransfer result = download_new_database_from_r2(
        destination, vault, "archive.sqlite3", key);
    server.join();

    EXPECT_EQ(result.byte_count, remote_vault.size() + remote_bytes.size());
    EXPECT_EQ(file_bytes(destination), remote_bytes);
    const VaultDefinition downloaded = load_vault_definition_file(
        remote_workspace.root(), vault);
    EXPECT_EQ(downloaded.name, "Remote Archive");
    EXPECT_EQ(downloaded.data, std::filesystem::weakly_canonical(destination));
    EXPECT_EQ(
        inspect_workspace_session_database(destination),
        WorkspaceDatabaseState::valid_v2);
    EXPECT_FALSE(std::filesystem::exists(stale_wal));
    std::filesystem::path backup = destination;
    backup += ".bac";
    EXPECT_FALSE(std::filesystem::exists(backup));
    ASSERT_EQ(server.requests().size(), 2U);
    EXPECT_TRUE(server.requests()[0].starts_with(
        "GET /cha-backups/archive.sqlite3.toml HTTP/1.1"));
    EXPECT_TRUE(server.requests()[1].starts_with(
        "GET /cha-backups/archive.sqlite3 HTTP/1.1"));
}

TEST(R2DatabaseTransfer, RejectsProtectedDownloadsWithoutPublishingThem) {
    test::TestWorkspace workspace;
    const std::filesystem::path destination =
        workspace.root() / "protected.sqlite3";
    const std::filesystem::path vault = workspace.root() / "vault-2.toml";
    const std::string remote_vault =
        "vault_name = \"Protected\"\n"
        "data = \"/another/computer/protected.sqlite3\"\n"
        "protected = true\n";
    MockHttpServer server({
        http_response("application/toml", remote_vault),
        http_response("application/vnd.sqlite3", "truncated"),
    });
    const R2StorageKey key = storage(mock_url(server.port()));
    server.start();

    EXPECT_THROW(
        (void)download_new_database_from_r2(
            destination, vault, "protected.sqlite3", key),
        std::runtime_error);
    server.join();

    EXPECT_FALSE(std::filesystem::exists(destination));
    EXPECT_FALSE(std::filesystem::exists(vault));
}

TEST(R2DatabaseTransfer, CreatesALocalDefinitionWhenR2HasOnlyTheDatabase) {
    test::TestWorkspace workspace;
    const std::filesystem::path remote =
        test::import_test_database(workspace.root());
    const std::string remote_bytes = file_bytes(remote);
    const std::filesystem::path destination =
        workspace.root() / "philosophy.sqlite3";
    const std::filesystem::path vault = workspace.root() / "vault-2.toml";
    MockHttpServer server({
        "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
        "Connection: close\r\n\r\n",
        http_response("application/vnd.sqlite3", remote_bytes),
    });
    const R2StorageKey key = storage(mock_url(server.port()));
    server.start();

    const R2DatabaseTransfer result = download_new_database_from_r2(
        destination, vault, "philosophy.sqlite3", key);
    server.join();

    EXPECT_EQ(result.byte_count, remote_bytes.size());
    EXPECT_EQ(file_bytes(destination), remote_bytes);
    const VaultDefinition downloaded = load_vault_definition_file(
        workspace.root(), vault);
    EXPECT_EQ(downloaded.name, "philosophy");
    EXPECT_EQ(downloaded.data, std::filesystem::weakly_canonical(destination));
    ASSERT_EQ(server.requests().size(), 2U);
    EXPECT_TRUE(server.requests()[0].starts_with(
        "GET /cha-backups/philosophy.sqlite3.toml HTTP/1.1"));
    EXPECT_TRUE(server.requests()[1].starts_with(
        "GET /cha-backups/philosophy.sqlite3 HTTP/1.1"));
}

TEST(R2DatabaseTransfer, DownloadsBothFilesAndReplacesOlderBackups) {
    test::TestWorkspace local_workspace;
    test::TestWorkspace remote_workspace;
    remote_workspace.add_persona("remote", "Remote Persona");
    const std::filesystem::path local =
        test::import_test_database(local_workspace.root());
    const std::filesystem::path remote =
        test::import_test_database(remote_workspace.root());
    const std::filesystem::path vault = write_vault(local);
    const std::string local_bytes = file_bytes(local);
    const std::string local_vault = file_bytes(vault);
    const std::string remote_bytes = file_bytes(remote);
    const std::string remote_vault =
        "vault_name = \"Test\"\n"
        "data = \"/another/computer/workspace.sqlite3\"\n";
    std::filesystem::path database_backup = local;
    database_backup += ".bac";
    std::filesystem::path vault_backup = vault;
    vault_backup += ".bac";
    write_bytes(database_backup, "older database backup");
    write_bytes(vault_backup, "older vault backup");

    MockHttpServer server({
        http_response("application/toml", remote_vault),
        http_response("application/vnd.sqlite3", remote_bytes),
    });
    const R2StorageKey key = storage(mock_url(server.port()));
    server.start();

    const R2DatabaseTransfer result =
        download_database_from_r2(local, vault, key);
    server.join();

    EXPECT_EQ(result.byte_count, remote_bytes.size() + remote_vault.size());
    EXPECT_EQ(file_bytes(local), remote_bytes);
    const VaultDefinition downloaded = load_vault_definition_file(
        local_workspace.root(), vault);
    EXPECT_EQ(downloaded.name, "Test");
    EXPECT_EQ(downloaded.data, std::filesystem::weakly_canonical(local));
    EXPECT_EQ(file_bytes(database_backup), local_bytes);
    EXPECT_EQ(file_bytes(vault_backup), local_vault);
    EXPECT_EQ(
        inspect_workspace_session_database(local),
        WorkspaceDatabaseState::valid_v2);
    ASSERT_EQ(server.requests().size(), 2U);
    EXPECT_TRUE(server.requests()[0].starts_with(
        "GET /cha-backups/workspace.sqlite3.toml HTTP/1.1"));
    EXPECT_TRUE(server.requests()[1].starts_with(
        "GET /cha-backups/workspace.sqlite3 HTTP/1.1"));
}

TEST(R2DatabaseTransfer, InvalidDownloadLeavesBothFilesAndBackupsUntouched) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    const std::filesystem::path vault = write_vault(database);
    const std::string original_database = file_bytes(database);
    const std::string original_vault = file_bytes(vault);
    std::filesystem::path database_backup = database;
    database_backup += ".bac";
    std::filesystem::path vault_backup = vault;
    vault_backup += ".bac";
    write_bytes(database_backup, "existing database backup");
    write_bytes(vault_backup, "existing vault backup");

    MockHttpServer server({
        http_response("application/toml", original_vault),
        http_response("application/octet-stream", "not a sqlite database"),
    });
    const R2StorageKey key = storage(mock_url(server.port()));
    server.start();

    EXPECT_THROW(
        (void)download_database_from_r2(database, vault, key),
        std::runtime_error);
    server.join();

    EXPECT_EQ(file_bytes(database), original_database);
    EXPECT_EQ(file_bytes(vault), original_vault);
    EXPECT_EQ(file_bytes(database_backup), "existing database backup");
    EXPECT_EQ(file_bytes(vault_backup), "existing vault backup");
}

TEST(R2DatabaseTransfer, ExplainsMissingVaultObjectFromLegacyUploads) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    const std::filesystem::path vault = write_vault(database);
    MockHttpServer server({
        "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
        "Connection: close\r\n\r\n",
    });
    const R2StorageKey key = storage(mock_url(server.port()));
    server.start();

    try {
        (void)download_database_from_r2(database, vault, key);
        FAIL() << "expected a missing vault object to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find("legacy database-only upload"),
            std::string::npos);
    }
    server.join();
}

TEST(R2DatabaseTransfer, RejectsInvalidR2KeyAndAnActiveLease) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    const std::filesystem::path vault = write_vault(database);
    R2StorageKey invalid = storage("");

    try {
        (void)upload_database_to_r2(database, vault, invalid);
        FAIL() << "expected invalid R2 URL to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("url"), std::string::npos);
    }

    SessionLease lease = SessionLease::acquire(database, "held by test");
    EXPECT_THROW(
        (void)download_database_from_r2(database, vault, invalid),
        SessionBusyError);
}

} // namespace
} // namespace cha::web
