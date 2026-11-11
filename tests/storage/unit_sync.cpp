#include "app/application_config.h"
#include "app/d1_client.h"
#include "storage/note_storage.h"
#include "storage/sqlite_storage.h"
#include "storage/sync_storage.h"
#include "storage/workspace_session_database.h"
#include "support/mock_http_server.h"
#include "support/test_workspace.h"
#include "util/base64.h"
#include "workspace/workspace_config_store.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace cha {
namespace {

using Database = storage::SqliteDatabase;

constexpr std::string_view sync_machine = "mac";

std::string d1_key_toml(
    std::string_view display = "D1",
    std::string_view account = "account-1",
    std::string_view database = "database-1",
    std::string_view token = "token-1") {
    std::string content = "display_name = \"";
    content += display;
    content += "\"\ntype = \"D1\"\naccount_id = \"";
    content += account;
    content += "\"\ndatabase_id = \"";
    content += database;
    content += "\"\napi_token = \"";
    content += token;
    content += "\"\n";
    return content;
}

void add_d1_key(
    Database& database,
    std::string_view id = "api_key_1",
    std::string_view account = "account-1",
    std::string_view db = "database-1",
    std::string_view token = "token-1") {
    database.prepare(
        "INSERT INTO config (name, content) VALUES (?1, ?2)",
        "system/keys/" + std::string(id) + "/config.toml",
        d1_key_toml("D1", account, db, token)).run();
}

nlohmann::json text_document(std::string text = "Hello") {
    return {
        {"type", "doc"},
        {"content", {{{"type", "paragraph"},
            {"content", {{{"type", "text"}, {"text", std::move(text)}}}}}}}};
}

nlohmann::json queue_json(Database& database) {
    auto rows = sync_queue_list(database);
    nlohmann::json out = nlohmann::json::array();
    for (const auto& row : rows) out.push_back(nlohmann::json::parse(row.record));
    return out;
}

bool has_sync_table(Database& database, std::string_view name) {
    auto check = database.prepare(
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?1", name);
    return check.step();
}

bool has_changed_column(Database& database) {
    auto columns = database.prepare("PRAGMA table_info(note_items)");
    while (columns.step()) {
        if (columns.text(1) == "changed") return true;
    }
    return false;
}

class SyncDatabaseTest : public testing::Test {
protected:
    void SetUp() override {
        path_ = workspace_.root() / "sync.sqlite3";
        create_empty_workspace_session_database(path_);
        database_ = std::make_unique<Database>(path_, Database::Mode::read_write);
    }

    void TearDown() override {
        database_.reset();
        std::error_code error;
        for (const std::string_view suffix : {"", "-journal", "-wal", "-shm"}) {
            std::filesystem::remove(
                std::filesystem::path(std::string(path_) + std::string(suffix)), error);
        }
    }

    test::TestWorkspace workspace_;
    std::filesystem::path path_;
    std::unique_ptr<Database> database_;
};

TEST(SyncMachine, Names) {
    EXPECT_TRUE(valid_machine_name("mac"));
    EXPECT_TRUE(valid_machine_name("mac-1"));
    EXPECT_FALSE(valid_machine_name(""));
    EXPECT_FALSE(valid_machine_name("a/b"));
    EXPECT_FALSE(valid_machine_name("/"));
    EXPECT_TRUE(same_machine_name("Mac", "mAc"));
    EXPECT_FALSE(same_machine_name("mac", "windows"));
}

TEST_F(SyncDatabaseTest, FreshDatabaseHasSyncSchema) {
    EXPECT_TRUE(has_sync_table(*database_, "sync_queue"));
    EXPECT_TRUE(has_changed_column(*database_));
    EXPECT_FALSE(sync_has_pending(*database_));
    EXPECT_TRUE(sync_queue_list(*database_).empty());
}

TEST_F(SyncDatabaseTest, MigrationPreservesContentAndInitializesFlags) {
    database_.reset();
    (void)import_workspace_configuration(
        workspace_.root(), path_, WorkspaceConfigLease::acquire);
    database_ = std::make_unique<Database>(path_, Database::Mode::read_write);
    const auto folder = note_create(*database_, std::nullopt, NoteItemKind::folder, "Books");
    database_.reset();
    // Simulate an older vault that predates the sync schema.
    {
        Database legacy(path_, Database::Mode::read_write);
        legacy.execute("DROP TABLE sync_queue");
        legacy.execute("ALTER TABLE note_items DROP COLUMN changed");
        EXPECT_FALSE(has_changed_column(legacy));
    }
    auto store = WorkspaceConfigStore::open(path_);
    database_ = std::make_unique<Database>(path_, Database::Mode::read_write);
    ASSERT_TRUE(has_sync_table(*database_, "sync_queue"));
    const auto items = store->list_note_items();
    EXPECT_TRUE(std::any_of(items.begin(), items.end(), [](const NoteItem& item) {
        return item.name == "Books";
    }));
    EXPECT_TRUE(sync_queue_list(*database_).empty());
    auto check = database_->prepare("SELECT changed FROM note_items WHERE id=?1", folder.id);
    ASSERT_TRUE(check.step());
    EXPECT_EQ(check.integer(0), 0);
    store.reset();
    // Reopening twice keeps working.
    auto reopened = WorkspaceConfigStore::open(path_);
    const auto reopened_items = reopened->list_note_items();
    EXPECT_TRUE(std::any_of(reopened_items.begin(), reopened_items.end(),
        [](const NoteItem& item) { return item.name == "Books"; }));
}

TEST_F(SyncDatabaseTest, AbortedOperationRollsBackItsRecord) {
    add_d1_key(*database_);
    const auto folder = note_create(
        *database_, std::nullopt, NoteItemKind::folder, "Books", sync_machine);
    database_->execute(
        "CREATE TRIGGER fail_rename BEFORE UPDATE OF name ON note_items "
        "BEGIN SELECT RAISE(ABORT, 'forced rename failure'); END");
    // Rename enqueues the old path before performing the update.
    EXPECT_THROW((void)note_rename(*database_, folder.id, "Renamed", sync_machine), NoteError);
    EXPECT_EQ(sync_note_find(*database_, {"Books"}), folder.id);
    ASSERT_EQ(queue_json(*database_).size(), 1u);
    EXPECT_EQ(queue_json(*database_)[0]["kind"], "note.create");
}

TEST_F(SyncDatabaseTest, DisconnectedOperationsWriteNoRecordsOrFlags) {
    add_d1_key(*database_);
    const auto folder =
        note_create(*database_, std::nullopt, NoteItemKind::folder, "Books", "");
    EXPECT_TRUE(sync_queue_list(*database_).empty());
    const auto note = note_create(*database_, folder.id, NoteItemKind::note, "Note", "");
    (void)note_save(*database_, note.id, text_document(), "");
    auto check = database_->prepare("SELECT changed FROM note_items WHERE id=?1", note.id);
    ASSERT_TRUE(check.step());
    EXPECT_EQ(check.integer(0), 0);
    EXPECT_TRUE(sync_queue_list(*database_).empty());
}

TEST_F(SyncDatabaseTest, MachineWithoutCredentialsWritesNothing) {
    const auto folder = note_create(
        *database_, std::nullopt, NoteItemKind::folder, "Other", sync_machine);
    (void)folder;
    EXPECT_TRUE(sync_queue_list(*database_).empty());
    EXPECT_FALSE(sync_connected(*database_, sync_machine));
}

TEST_F(SyncDatabaseTest, QueueRoundTripAndClear) {
    {
        storage::SqliteTransaction transaction(*database_);
        sync_queue_append(*database_, R"({"kind":"note.delete"})");
        sync_queue_append(*database_, R"({"kind":"config.edit"})");
        transaction.commit();
    }
    EXPECT_TRUE(sync_has_pending(*database_));
    auto rows = sync_queue_list(*database_);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_LT(rows[0].rowid, rows[1].rowid);
    {
        storage::SqliteTransaction transaction(*database_);
        sync_queue_delete(*database_, rows[0].rowid);
        transaction.commit();
    }
    rows = sync_queue_list(*database_);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].record, R"({"kind":"config.edit"})");
    const auto note = note_create(*database_, std::nullopt, NoteItemKind::note, "Flagged");
    database_->prepare("UPDATE note_items SET changed=1 WHERE id=?1", note.id).run();
    EXPECT_TRUE(sync_has_pending(*database_));
    {
        storage::SqliteTransaction transaction(*database_);
        sync_clear_outgoing(*database_);
        transaction.commit();
    }
    EXPECT_FALSE(sync_has_pending(*database_));
}

TEST_F(SyncDatabaseTest, QueueNameHelpers) {
    EXPECT_EQ(sync_queue_table("Vault", "mac"), "q/Vault/mac");
    EXPECT_EQ(sync_escape_identifier("a\"b"), "\"a\"\"b\"");
    const auto parsed = sync_parse_queue_table("q/Vault/mac");
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->vault, "Vault");
    EXPECT_EQ(parsed->machine, "mac");
    EXPECT_FALSE(sync_parse_queue_table("q/only"));
    EXPECT_FALSE(sync_parse_queue_table("x/Vault/mac"));
    EXPECT_FALSE(sync_parse_queue_table("q/a/b/c"));
    EXPECT_TRUE(sync_same_vault("Research", "research"));
    EXPECT_FALSE(sync_same_vault("a%b", "acb"));
    EXPECT_TRUE(sync_same_machine("MAC", "mac"));
}

TEST_F(SyncDatabaseTest, CredentialLookup) {
    EXPECT_FALSE(sync_read_d1(*database_));
    EXPECT_FALSE(sync_connected(*database_, sync_machine));
    add_d1_key(*database_);
    const auto credentials = sync_read_d1(*database_);
    ASSERT_TRUE(credentials);
    EXPECT_EQ(credentials->key_id, "api_key_1");
    EXPECT_EQ(credentials->account_id, "account-1");
    EXPECT_EQ(credentials->database_id, "database-1");
    EXPECT_EQ(credentials->api_token, "token-1");
    EXPECT_TRUE(sync_connected(*database_, sync_machine));
    EXPECT_TRUE(sync_connected(*database_, sync_machine));
    EXPECT_FALSE(sync_connected(*database_, ""));
    EXPECT_FALSE(sync_connected(*database_, "a/b"));
}

TEST_F(SyncDatabaseTest, MissingTokenIsDisconnected) {
    add_d1_key(*database_, "api_key_1", "account-1", "database-1", "");
    EXPECT_FALSE(sync_read_d1(*database_));
    EXPECT_FALSE(sync_connected(*database_, sync_machine));
}

TEST_F(SyncDatabaseTest, RecordPiecesSplitAtUtf8Boundaries) {
    const std::string text(100, 'x');
    const auto pieces = sync_split_record(text);
    ASSERT_EQ(pieces.size(), 1u);
    EXPECT_EQ(pieces.front(), text);
    std::string wide;
    while (wide.size() < sync_piece_bytes + 10) wide += "\u00e9";
    const auto split = sync_split_record(wide);
    ASSERT_EQ(split.size(), 2u);
    EXPECT_EQ(split[0] + split[1], wide);
    EXPECT_EQ(split[0].size() % 2, 0u);
}

TEST(SyncDelta, FiltersD1RowsSeparately) {
    const std::vector<ConfigFile> old_rows = {
        {"system/keys/api_key_1/config.toml", d1_key_toml()},
        {"system/keys/config.toml", "next_id = 2\n"},
        {"characters/guide/config.toml", "display_name = \"Guide\"\n"},
    };
    const std::vector<ConfigFile> new_rows = {
        {"system/keys/api_key_1/config.toml",
         "display_name = \"Models\"\ntype = \"models\"\nvalue = \"s\"\ncloud_store = \"none\"\n"},
        {"system/keys/config.toml", "next_id = 2\n"},
        {"characters/guide/config.toml", "display_name = \"Guide\"\n"},
    };
    const ConfigDelta delta = sync_config_delta(old_rows, new_rows);
    ASSERT_EQ(delta.set.size(), 1u);
    EXPECT_EQ(delta.set.front().name, "system/keys/api_key_1/config.toml");
    EXPECT_TRUE(delta.names.empty());
}

TEST(SyncDelta, D1OnlyChangeEmitsNothing) {
    const std::vector<ConfigFile> old_rows = {
        {"characters/guide/config.toml", "display_name = \"Guide\"\n"},
    };
    const std::vector<ConfigFile> new_rows = {
        {"characters/guide/config.toml", "display_name = \"Guide\"\n"},
        {"system/keys/api_key_7/config.toml", d1_key_toml()},
    };
    const ConfigDelta delta = sync_config_delta(old_rows, new_rows);
    EXPECT_TRUE(delta.set.empty());
    EXPECT_TRUE(delta.names.empty());
}

struct RecordedRequest {
    std::string url;
    std::string body;
    std::vector<std::string> headers;
};

D1Client::Credentials test_credentials() {
    return {"account-1", "database-1", "token-1"};
}

std::string d1_success(const std::string& results) {
    return R"({"success":true,"errors":[],"messages":[],"result":[{"success":true,"results":)"
        + results + "}]}";
}

TEST(D1Client, BuildsBatchesWithBoundParameters) {
    std::vector<RecordedRequest> requests;
    D1Client client(test_credentials(),
        [&](const std::string& url, const std::string& body,
            const std::vector<std::string>& headers) {
            requests.push_back({url, body, headers});
            auto response = nlohmann::json::parse(d1_success("[]"));
            const auto batch = nlohmann::json::parse(body).at("batch");
            if (batch.size() > 1) {
                const auto statement_result = response["result"].front();
                response["result"].push_back(statement_result);
            }
            return std::pair{200L, response.dump()};
        });
    EXPECT_FALSE(client.create_queue_table("q/Vault/mac"));
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(
        requests[0].url,
        "https://api.cloudflare.com/client/v4/accounts/account-1/d1/database/database-1/query");
    const auto created = nlohmann::json::parse(requests[0].body);
    EXPECT_TRUE(created["batch"][0]["sql"].get<std::string>().find("\"q/Vault/mac\"") != std::string::npos);
    EXPECT_TRUE(
        std::find(
            requests[0].headers.begin(), requests[0].headers.end(),
            "Authorization: Bearer token-1")
        != requests[0].headers.end());
    requests.clear();
    EXPECT_FALSE(client.insert_piece({"q/Vault/a", "q/Vault/b"}, 1, 2, "record"));
    const auto batch = nlohmann::json::parse(requests[0].body);
    ASSERT_EQ(batch["batch"].size(), 2u);
    EXPECT_EQ(batch["batch"][0]["params"], nlohmann::json::array({"1", "2", "record"}));
    EXPECT_TRUE(batch["batch"][1]["sql"].get<std::string>().find("\"q/Vault/b\"") != std::string::npos);
}

TEST(D1Client, QuotesSpecialCharacterNames) {
    std::vector<RecordedRequest> requests;
    D1Client client(test_credentials(),
        [&](const std::string& url, const std::string& body,
            const std::vector<std::string>& headers) {
            requests.push_back({url, body, headers});
            return std::pair{200L, d1_success("[]")};
        });
    EXPECT_FALSE(client.create_queue_table("q/Va%ult/ma\"c"));
    const auto body = nlohmann::json::parse(requests[0].body);
    EXPECT_TRUE(body["batch"][0]["sql"].get<std::string>().find("\"q/Va%ult/ma\"\"c\"") != std::string::npos);
}

TEST(D1Client, FindsOnlyOtherMachinesForTheExactVault) {
    D1Client client(test_credentials(),
        [](const std::string&, const std::string&, const std::vector<std::string>&) {
            return std::pair{200L, d1_success(R"([
                {"name":"q/Work%_/MAC"}, {"name":"q/work%_/Phone"},
                {"name":"q/WorkXY/Other"}, {"name":"q/Work%_/one/two"}, {"name":"misc"}
            ])")};
        });
    const auto [queues, error] = client.list_vault_queues("Work%_", "mac");
    ASSERT_FALSE(error);
    EXPECT_EQ(queues.local, "q/Work%_/MAC");
    EXPECT_EQ(queues.recipients, std::vector<std::string>{"q/work%_/Phone"});
}

TEST(D1Client, ClassifiesFailures) {
    const auto failure = [](const std::string& body, long status = 200L) {
        D1Client client(test_credentials(),
            [=](const std::string&, const std::string&, const std::vector<std::string>&) {
                return std::pair{status, body};
            });
        return client.list_tables().second;
    };
    EXPECT_EQ(failure("not json")->failure, D1Client::Failure::protocol);
    EXPECT_EQ(failure(R"({"success":false,"errors":[{"message":"nope"}]})")->failure,
        D1Client::Failure::protocol);
    EXPECT_EQ(
        failure(R"({"success":true,"result":[{"success":false,"error":[{"message":"Exceeded maximum DB size."}]}]})")
            ->failure,
        D1Client::Failure::full_database);
    EXPECT_EQ(
        failure(R"({"success":true,"result":[{"success":false,"error":[{"message":"no such table: q/V/m"}]}]})")
            ->failure,
        D1Client::Failure::missing_table);
    EXPECT_EQ(
        failure(R"({"success":true,"result":[{"success":false,"error":[{"message":"D1 query result exceeded the maximum response size limit of 50 MB"}]}]})")
            ->failure,
        D1Client::Failure::result_too_large);
    EXPECT_EQ(
        failure(R"({"success":false,"errors":[{"message":"D1_ERROR: Exceeded maximum DB size."}]})", 400)
            ->failure,
        D1Client::Failure::full_database);
    EXPECT_EQ(
        failure(R"({"success":false,"errors":[{"message":"no such table: Exceeded maximum DB size."}]})", 400)
            ->failure,
        D1Client::Failure::missing_table);
    EXPECT_EQ(
        failure(R"({"success":false,"errors":[{"message":"Your account has exceeded D1's free tier daily row write limit."}]})", 429)
            ->failure,
        D1Client::Failure::quota_exceeded);
    EXPECT_EQ(failure("{}", 429)->failure, D1Client::Failure::rate_limited);
    EXPECT_EQ(failure("{}", 401)->failure, D1Client::Failure::authentication);
    EXPECT_EQ(failure("{}", 500)->failure, D1Client::Failure::http_status);
    D1Client throwing(test_credentials(),
        [](const std::string&, const std::string&, const std::vector<std::string>&)
            -> std::pair<long, std::string> {
            throw std::runtime_error("connection refused");
        });
    EXPECT_EQ(throwing.list_tables().second->failure, D1Client::Failure::transport);
}

TEST(D1Client, RejectsUnconfirmedBatchesAndMalformedRows) {
    std::string response;
    D1Client client(test_credentials(),
        [&](const std::string&, const std::string&, const std::vector<std::string>&) {
            return std::pair{200L, response};
        });
    for (const auto& body : {std::string("{}"), d1_success("[]"),
            std::string(R"({"success":true,"result":[{"success":true},{}]})")}) {
        response = body;
        const auto error = client.insert_piece({"q/V/a", "q/V/b"}, 1, 1, "record");
        ASSERT_TRUE(error);
        EXPECT_EQ(error->failure, D1Client::Failure::protocol);
    }
    response = d1_success(R"([{"rowid":"bad","piece":1,"pieces":1,"record":"r"}])");
    const auto [rows, error] = client.read_queue("q/V/a", 50);
    EXPECT_TRUE(rows.empty());
    ASSERT_TRUE(error);
    EXPECT_EQ(error->failure, D1Client::Failure::protocol);
    // Row IDs must follow the read position in increasing order.
    for (const auto* ids : {"[-1,6]", "[0,6]", "[5,5]", "[6,5]"}) {
        nlohmann::json list = nlohmann::json::array();
        for (const auto& rowid : nlohmann::json::parse(ids))
            list.push_back({{"rowid", rowid}, {"piece", 1}, {"pieces", 1}, {"record", "r"}});
        response = d1_success(list.dump());
        const auto [unordered, order_error] = client.read_queue("q/V/a", 50);
        EXPECT_TRUE(unordered.empty()) << ids;
        ASSERT_TRUE(order_error) << ids;
        EXPECT_EQ(order_error->failure, D1Client::Failure::protocol) << ids;
    }
    response = d1_success(R"([{"rowid":2,"piece":1,"pieces":1,"record":"r"}])");
    EXPECT_TRUE(client.read_queue("q/V/a", 50, 2).second);
}

TEST(D1Client, ReadsRowsAndCounts) {
    D1Client client(test_credentials(),
        [](const std::string&, const std::string& body, const std::vector<std::string>&) {
            const auto batch = nlohmann::json::parse(body);
            const std::string sql = batch["batch"][0]["sql"];
            if (sql.find("COUNT(*)") != std::string::npos)
                return std::pair{200L, d1_success(R"([{"count":7}])")};
            if (sql.find("sqlite_master") != std::string::npos)
                return std::pair{200L, d1_success(R"([{"name":"q/V/a"}])")};
            return std::pair{
                200L, d1_success(R"([{"rowid":4294967299,"piece":1,"pieces":1,"record":"r"}])")};
        });
    const auto [tables, list_error] = client.list_tables();
    EXPECT_FALSE(list_error);
    ASSERT_EQ(tables.size(), 1u);
    EXPECT_EQ(tables.front(), "q/V/a");
    const auto [rows, read_error] = client.read_queue("q/V/a", 50, 2);
    EXPECT_FALSE(read_error);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows.front().rowid, 4294967299LL);
    EXPECT_EQ(rows.front().record, "r");
    const auto [counts, count_error] = client.count_rows({"q/V/a"});
    EXPECT_FALSE(count_error);
    EXPECT_EQ(counts, std::vector<std::int64_t>{7});
    EXPECT_FALSE(client.delete_through("q/V/a", 3));
    EXPECT_FALSE(client.drop_table("q/V/a"));
}

TEST(D1Client, LoopbackExercisesTheCurlPath) {
    const std::string body = d1_success(R"([{"name":"q/V/a"},{"name":"q/V/b"}])");
    MockHttpServer mock(
        {"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
         + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body});
    mock.start();
    D1Client client(test_credentials());
    client.set_endpoint_for_tests("http://127.0.0.1:" + std::to_string(mock.port()) + "/query");
    const auto [tables, error] = client.list_tables();
    mock.join();
    EXPECT_FALSE(error);
    ASSERT_EQ(tables.size(), 2u);
    ASSERT_EQ(mock.requests().size(), 1u);
    EXPECT_TRUE(mock.requests().front().starts_with("POST /query HTTP/1.1"));
    EXPECT_NE(mock.requests().front().find("Authorization: Bearer token-1"), std::string::npos);
    const auto sent = nlohmann::json::parse(
        mock.requests().front().substr(mock.requests().front().find("\r\n\r\n") + 4));
    EXPECT_TRUE(sent.contains("batch"));
}
class SyncNotesTest : public SyncDatabaseTest {
protected:
    void SetUp() override {
        SyncDatabaseTest::SetUp();
        create_event_history_schema(*database_);
        add_d1_key(*database_);
    }

    nlohmann::json records() { return queue_json(*database_); }
};

TEST_F(SyncNotesTest, StructuralOperationsRecordImmediately) {
    const auto folder =
        note_create(*database_, std::nullopt, NoteItemKind::folder, "Books", sync_machine);
    auto records = this->records();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0]["kind"], "note.create");
    EXPECT_EQ(records[0]["path"], nlohmann::json::array({"Books"}));
    EXPECT_EQ(records[0]["item"], "folder");

    const auto note =
        note_create(*database_, folder.id, NoteItemKind::note, "Note", sync_machine);
    records = this->records();
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[1]["path"], nlohmann::json::array({"Books", "Note"}));
    EXPECT_EQ(records[1]["item"], "note");
    EXPECT_TRUE(records[1].contains("content"));
    EXPECT_TRUE(records[1]["compressed_content"].is_null());

    (void)note_rename(*database_, note.id, "Renamed", sync_machine);
    records = this->records();
    ASSERT_EQ(records.size(), 3u);
    EXPECT_EQ(records[2]["kind"], "note.rename");
    EXPECT_EQ(records[2]["path"], nlohmann::json::array({"Books", "Note"}));
    EXPECT_EQ(records[2]["name"], "Renamed");

    (void)note_set_read_only(*database_, note.id, true, sync_machine);
    EXPECT_EQ(this->records().back()["kind"], "note.read_only");

    note_save_compressed(*database_, note.id, "compressed", sync_machine);
    EXPECT_EQ(this->records().back()["kind"], "note.compress");

    note_delete(*database_, note.id, sync_machine);
    records = this->records();
    EXPECT_EQ(records.back()["kind"], "note.delete");
    EXPECT_EQ(records.back()["path"], nlohmann::json::array({"Books", "Renamed"}));
    // Structural operations set no changed flags.
    auto flags = database_->prepare("SELECT COUNT(*) FROM note_items WHERE changed<>0");
    ASSERT_TRUE(flags.step());
    EXPECT_EQ(flags.integer(0), 0);
}

TEST_F(SyncNotesTest, BothCompressionCommitPathsRecord) {
    const auto note =
        note_create(*database_, std::nullopt, NoteItemKind::note, "Note", sync_machine);
    (void)note_save(*database_, note.id, text_document("source text"), sync_machine);
    EXPECT_TRUE(note_commit_compressed(
        *database_, note.id, "source text", "summary", "Saved.", sync_machine));
    auto records = this->records();
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records.back()["kind"], "note.compress");
    const auto bytes = decode_base64(records.back()["compressed_content"].get<std::string>());
    EXPECT_EQ(note_parse_saved_document(bytes, note.id), note_document_from_text("summary"));
}

TEST_F(SyncNotesTest, IncomingCompressionAcceptsBothFormatsAndClearsMissingOrNullContent) {
    const auto source = note_create(*database_, {}, NoteItemKind::note, "Source", sync_machine);
    note_save_compressed(*database_, source.id, "Current\r\nText", sync_machine);
    const auto outgoing = records();
    const auto& wire = outgoing.back()["compressed_content"];
    auto create = outgoing.front();
    create["path"] = {"Target"};
    create.erase("compressed_content");
    create["compressed_text"] = "Legacy";
    ASSERT_TRUE(note_apply_record(*database_, create));
    const auto target = sync_note_find(*database_, {"Target"});
    ASSERT_TRUE(target);
    const auto compressed_id = database_->last_insert_rowid();
    EXPECT_EQ(note_get(*database_, *target, NoteTextMode::compressed).document, note_document_from_text("Legacy"));

    auto compress = outgoing.back();
    compress["path"] = {"Target"};
    compress["compressed_text"] = "Ignored";
    EXPECT_TRUE(note_apply_record(*database_, compress));
    EXPECT_EQ(note_get(*database_, *target, NoteTextMode::compressed).document, note_document_from_text("Current\r\nText"));
    {
        auto row = database_->prepare("SELECT parent_id FROM note_items WHERE id=? AND compressed=1", compressed_id);
        ASSERT_TRUE(row.step());
        EXPECT_EQ(row.integer(0), *target);
    }
    for (const auto& value : {nlohmann::json(encode_base64("invalid zlib")), nlohmann::json(nullptr)}) {
        compress["compressed_content"] = value;
        EXPECT_THROW((void)note_apply_record(*database_, compress), NoteError);
    }
    compress.erase("compressed_content");
    for (const auto& value : {nlohmann::json(" \n"), nlohmann::json(nullptr)}) {
        compress["compressed_text"] = value;
        EXPECT_THROW((void)note_apply_record(*database_, compress), NoteError);
    }
    EXPECT_EQ(note_get(*database_, *target, NoteTextMode::compressed).document, note_document_from_text("Current\r\nText"));

    auto update = create;
    update["kind"] = "note.update";
    update["compressed_content"] = nullptr;
    EXPECT_TRUE(note_apply_record(*database_, update));
    EXPECT_EQ(note_get(*database_, *target, NoteTextMode::compressed).document_id, *target);
    update.erase("compressed_content");
    EXPECT_TRUE(note_apply_record(*database_, update));
    EXPECT_EQ(note_get(*database_, *target, NoteTextMode::compressed).document, note_document_from_text("Legacy"));
    update.erase("compressed_text");
    EXPECT_TRUE(note_apply_record(*database_, update));
    EXPECT_EQ(note_get(*database_, *target, NoteTextMode::compressed).document_id, *target);
    update["compressed_content"] = wire;
    EXPECT_TRUE(note_apply_record(*database_, update));
    EXPECT_EQ(note_get(*database_, *target, NoteTextMode::compressed).document, note_document_from_text("Current\r\nText"));
    create["path"] = {"Second target"};
    create["compressed_content"] = wire;
    EXPECT_TRUE(note_apply_record(*database_, create));
    const auto second = sync_note_find(*database_, {"Second target"});
    ASSERT_TRUE(second);
    EXPECT_EQ(note_get(*database_, *second, NoteTextMode::compressed).document, note_document_from_text("Current\r\nText"));
    EXPECT_EQ(records(), outgoing);
}

TEST_F(SyncNotesTest, FolderImportQueuesOnlyOriginalsWithTheirCompressedContent) {
    const auto folder = note_create(*database_, {}, NoteItemKind::folder, "Export");
    const auto note = note_create(*database_, folder.id, NoteItemKind::note, "Note");
    note_save_compressed(*database_, note.id, "Summary");
    const auto file = workspace_.root() / "folder.sqlite3";
    note_export_folder(*database_, folder.id, file);
    const auto parent = note_create(*database_, {}, NoteItemKind::folder, "Imported");
    (void)note_import_folder(*database_, parent.id, file, sync_machine);
    const auto queued = records();
    ASSERT_EQ(queued.size(), 2u);
    EXPECT_EQ(queued[0]["path"], nlohmann::json::array({"Imported", "Export"}));
    EXPECT_EQ(queued[1]["path"], nlohmann::json::array({"Imported", "Export", "Note"}));
    const auto bytes = decode_base64(queued[1]["compressed_content"].get<std::string>());
    EXPECT_EQ(note_parse_saved_document(bytes, 0), note_document_from_text("Summary"));
}

TEST_F(SyncNotesTest, TextSaveSetsFlagAndCollectsLatestContent) {
    const auto note =
        note_create(*database_, std::nullopt, NoteItemKind::note, "Note", sync_machine);
    EXPECT_EQ(this->records().size(), 1u);
    (void)note_save(*database_, note.id, text_document("first"), sync_machine);
    (void)note_save(*database_, note.id, text_document("second"), sync_machine);
    // Saves set the flag instead of recording.
    EXPECT_EQ(this->records().size(), 1u);
    EXPECT_EQ(sync_collect_flagged_notes(*database_, sync_machine), 1u);
    auto records = this->records();
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[1]["kind"], "note.update");
    EXPECT_EQ(records[1]["path"], nlohmann::json::array({"Note"}));
    EXPECT_EQ(note_get_text(*database_, note.id).text.find("second"), 0u);
    // A later save is observable again.
    (void)note_save(*database_, note.id, text_document("third"), sync_machine);
    EXPECT_EQ(sync_collect_flagged_notes(*database_, sync_machine), 1u);
    EXPECT_EQ(this->records().size(), 3u);
}

TEST_F(SyncNotesTest, ScrollSaveWritesNeitherRecordNorFlag) {
    const auto note =
        note_create(*database_, std::nullopt, NoteItemKind::note, "Note", sync_machine);
    note_save_scroll_position(*database_, note.id, 12);
    EXPECT_EQ(this->records().size(), 1u);
    EXPECT_EQ(sync_collect_flagged_notes(*database_, sync_machine), 0u);
}

TEST_F(SyncNotesTest, CompressedEditRecordsAnUpdateForTheOriginalPath) {
    const auto note = note_create(*database_, {}, NoteItemKind::note, "Note", sync_machine);
    note_save_compressed(*database_, note.id, "Generated", sync_machine);
    const auto compressed = note_get(*database_, note.id, NoteTextMode::compressed);
    const auto edited = text_document("Manually edited");
    ASSERT_NE(compressed.document_id, note.id);
    EXPECT_EQ(sync_note_find(*database_, {"Note"}), note.id);
    EXPECT_FALSE(sync_note_find(*database_, {"Note", "Note"}));
    EXPECT_THROW((void)sync_note_path(*database_, compressed.document_id), std::runtime_error);
    EXPECT_FALSE(note_save(*database_, compressed.document_id, edited, sync_machine));
    note_save_scroll_position(*database_, compressed.document_id, 35);
    EXPECT_EQ(sync_collect_flagged_notes(*database_, sync_machine), 1u);
    const auto record = records().back();
    EXPECT_EQ(record["kind"], "note.update");
    EXPECT_EQ(record["path"], nlohmann::json::array({"Note"}));
    EXPECT_EQ(note_parse_saved_document(decode_base64(record["compressed_content"].get<std::string>()), 0), edited);
    EXPECT_EQ(record["scroll_top"], 0);
}

TEST_F(SyncNotesTest, BookImportRecordsParentsFirst) {
    const std::vector<NoteImportEntry> notes = {
        {"B", text_document("bee")}, {"A", text_document("ay")}};
    (void)note_import_book(*database_, std::nullopt, "Book", notes, sync_machine);
    const auto records = this->records();
    ASSERT_EQ(records.size(), 3u);
    for (const auto& record : records) EXPECT_EQ(record["kind"], "note.create");
    EXPECT_EQ(records[0]["path"], nlohmann::json::array({"Book"}));
    EXPECT_EQ(records[1]["path"], nlohmann::json::array({"Book", "B"}));
    EXPECT_EQ(records[2]["path"], nlohmann::json::array({"Book", "A"}));
    EXPECT_TRUE(records[1]["read_only"]);
}

TEST_F(SyncNotesTest, FolderImportRecordsEveryItem) {
    const auto source = note_create(*database_, std::nullopt, NoteItemKind::folder, "Src");
    const auto child =
        note_create(*database_, source.id, NoteItemKind::note, "Child", sync_machine);
    (void)note_save(*database_, child.id, text_document("child"), sync_machine);
    const auto export_path = workspace_.root() / "export.cha_folder";
    note_export_folder(*database_, source.id, export_path);
    const auto destination =
        note_create(*database_, std::nullopt, NoteItemKind::folder, "Dest", sync_machine);
    const std::size_t imported_base = this->records().size();
    (void)note_import_folder(*database_, destination.id, export_path, sync_machine);
    const auto records = this->records();
    ASSERT_EQ(records.size(), imported_base + 2u);
    EXPECT_EQ(records[imported_base]["path"], nlohmann::json::array({"Dest", "Src"}));
    EXPECT_EQ(records[imported_base + 1]["path"],
        nlohmann::json::array({"Dest", "Src", "Child"}));
    EXPECT_EQ(records[imported_base + 1]["kind"], "note.create");
}

void make_forum_session(Database& database) {
    database.prepare(
        "INSERT INTO forums(forum_key,forum_id) VALUES(1,'f1')").run();
    database.prepare(
        "INSERT INTO sessions(forum_key,session_id,label,updated_at,history_epoch,"
        "next_entry_id,next_request_id) VALUES(1,'20261109-120000-s','Label',0,1,1,1)").run();
    storage::SqliteTransaction transaction(database);
    note_sync_forums(database, {{"f1", "Forum One"}});
    transaction.commit();
}

nlohmann::json summary_body(std::string text = "Summary") {
    const auto heading = [](std::string title) {
        return nlohmann::json{{"type", "heading"}, {"attrs", {{"level", 1}}},
            {"content", {{{"type", "text"}, {"text", std::move(title)}}}}};
    };
    const auto paragraph = [](std::string body) {
        return nlohmann::json{{"type", "paragraph"},
            {"content", {{{"type", "text"}, {"text", std::move(body)}}}}};
    };
    return {{"type", "doc"},
        {"content",
         {heading("Short summary"), paragraph("Overview. Keywords: topic"),
          heading("Long summary"), paragraph(std::move(text))}}};
}

TEST_F(SyncNotesTest, SummaryCreateRecordsAndReplacementFlags) {
    make_forum_session(*database_);
    const FullSessionId identity{"f1", "20261109-120000-s"};
    const auto target = note_summary_target(*database_, identity);
    const auto summary =
        note_commit_summary(*database_, identity, target, summary_body(),
            std::chrono::system_clock::now(), sync_machine);
    auto records = this->records();
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0]["kind"], "note.create");
    EXPECT_EQ(records[0]["summary_of"],
        nlohmann::json::array({"f1", "20261109-120000-s"}));
    const auto replacement_target = note_summary_target(*database_, identity);
    ASSERT_EQ(replacement_target.target_note_id, summary.id);
    (void)note_commit_summary(*database_, identity, replacement_target, summary_body("New"),
        std::chrono::system_clock::now(), sync_machine);
    EXPECT_EQ(this->records().size(), 1u);
    EXPECT_EQ(sync_collect_flagged_notes(*database_, sync_machine), 1u);
    records = this->records();
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[1]["kind"], "note.update");
}

TEST_F(SyncNotesTest, IncomingCreateResolvesMissingParentsWithWarning) {
    const auto folders =
        note_create(*database_, std::nullopt, NoteItemKind::folder, "A", sync_machine);
    (void)folders;
    // Missing parent is a warned skip.
    EXPECT_THROW(
        (void)note_apply_record(*database_, {{"kind", "note.create"},
            {"path", {"Missing", "Note"}}, {"item", "note"},
            {"content", encode_base64("e30=")}, {"read_only", false}, {"scroll_top", 0}}),
        NoteError);
    // A taken name receives the approved suffix.
    const auto created = note_apply_record(*database_, {{"kind", "note.create"},
        {"path", {"A"}}, {"item", "folder"}});
    EXPECT_TRUE(created);
    const auto items = note_list(*database_);
    EXPECT_TRUE(std::any_of(items.begin(), items.end(), [](const NoteItem& item) {
        return item.name == "A (1)" && item.kind == NoteItemKind::folder;
    }));
}

TEST_F(SyncNotesTest, IncomingUpdateReplacesReadOnlyContentAndClearsAudio) {
    const auto note =
        note_create(*database_, std::nullopt, NoteItemKind::note, "Note", sync_machine);
    (void)note_save(*database_, note.id, text_document("new"), sync_machine);
    ASSERT_EQ(sync_collect_flagged_notes(*database_, sync_machine), 1u);
    nlohmann::json sender_update = queue_json(*database_).back();
    sender_update["scroll_top"] = 4294967296LL;
    // The receiver holds the same note read-only, with audio and an attempt.
    const auto second_path = workspace_.root() / "second.sqlite3";
    create_empty_workspace_session_database(second_path);
    Database second(second_path, Database::Mode::read_write);
    create_event_history_schema(second);
    add_d1_key(second);
    const auto existing = note_create(second, std::nullopt, NoteItemKind::note, "Note");
    (void)note_save(second, existing.id, text_document("old"), sync_machine);
    (void)note_set_read_only(second, existing.id, true);
    {
        auto audio = second.prepare(
            "INSERT INTO note_audio (note_id, ordinal, mime_type, audio) VALUES (?1, 0, 'audio/mp4', ?2)");
        audio.bind(1, existing.id);
        audio.bind_blob(2, "clip");
        audio.run();
    }
    second.prepare(
        "INSERT INTO note_audio_attempts(note_id,note_name,clip_count,service_origin,api_key_id) "
        "VALUES (?,?,?,?,?)",
        existing.id, "Note", 1, "origin", "key").run();
    EXPECT_TRUE(note_apply_record(second, sender_update));
    EXPECT_EQ(note_get_text(second, existing.id).text.find("new"), 0u);
    EXPECT_EQ(note_get(second, existing.id).scroll_top, 4294967296LL);
    auto audio = second.prepare("SELECT COUNT(*) FROM note_audio");
    ASSERT_TRUE(audio.step());
    EXPECT_EQ(audio.integer(0), 0);
    auto attempts = second.prepare("SELECT COUNT(*) FROM note_audio_attempts");
    ASSERT_TRUE(attempts.step());
    EXPECT_EQ(attempts.integer(0), 0);
    // Incoming application writes no records and sets no flags.
    EXPECT_TRUE(sync_queue_list(second).empty());
    auto flags = second.prepare("SELECT COUNT(*) FROM note_items WHERE changed<>0");
    ASSERT_TRUE(flags.step());
    EXPECT_EQ(flags.integer(0), 0);
}

TEST_F(SyncNotesTest, IncomingUpdateCreatesMissingNotesAndManagesFolders) {
    // A missing note is created from the record, even under a managed forum folder.
    make_forum_session(*database_);
    nlohmann::json record = {{"kind", "note.update"},
        {"path", {"Forums", "Forum One", "Restored"}},
        {"content", nullptr}, {"read_only", false}, {"scroll_top", 3}};
    {
        const auto note = note_create(*database_, std::nullopt, NoteItemKind::note, "Tmp");
        (void)note_save(*database_, note.id, text_document("tmp"));
        auto blob = database_->prepare("SELECT content_json FROM note_items WHERE id=?1", note.id);
        ASSERT_TRUE(blob.step());
        record["content"] = encode_base64(blob.blob(0));
    }
    EXPECT_TRUE(note_apply_record(*database_, record));
    const auto found = sync_note_find(*database_, {"Forums", "Forum One", "Restored"});
    ASSERT_TRUE(found);
    // A managed forum folder itself is never touched by incoming records.
    EXPECT_THROW(
        (void)note_apply_record(*database_,
            {{"kind", "note.rename"}, {"path", {"Forums", "Forum One"}}, {"name", "Other"}}),
        NoteError);
    EXPECT_THROW(
        (void)note_apply_record(*database_,
            {{"kind", "note.delete"}, {"path", {"Forums", "Forum One"}}}),
        NoteError);
}

TEST_F(SyncNotesTest, IncomingRecordsRebuildAnnotationsWithLocalIds) {
    const auto folder =
        note_create(*database_, std::nullopt, NoteItemKind::folder, "Notes", sync_machine);
    nlohmann::json highlighted = text_document("x");
    highlighted["content"][0]["content"][0]["marks"] = {{{"type", "highlight"}}};
    const auto note = note_create(*database_, folder.id, NoteItemKind::note, "N", sync_machine);
    (void)note_save(*database_, note.id, highlighted, sync_machine);
    EXPECT_EQ(sync_collect_flagged_notes(*database_, sync_machine), 1u);
    const auto update = queue_json(*database_).back();
    const auto second_path = workspace_.root() / "annotations.sqlite3";
    create_empty_workspace_session_database(second_path);
    Database second(second_path, Database::Mode::read_write);
    create_event_history_schema(second);
    EXPECT_TRUE(note_apply_record(second, {{"kind", "note.create"},
        {"path", {"Notes"}}, {"item", "folder"}}));
    EXPECT_TRUE(note_apply_record(second, update));
    bool annotations = false;
    for (const auto& item : note_list(second)) {
        if (item.kind == NoteItemKind::annotation) annotations = true;
    }
    EXPECT_TRUE(annotations);
}

TEST_F(SyncNotesTest, IncomingCreateRebuildsAnnotationsWithoutWaitingForAnUpdate) {
    auto document = text_document("highlighted");
    document["content"][0]["content"][0]["marks"] = {{{"type", "highlight"}}};
    (void)note_import_book(*database_, std::nullopt, "Book", {{"Chapter", document}}, sync_machine);
    const auto records = this->records();

    const auto second_path = workspace_.root() / "import.sqlite3";
    create_empty_workspace_session_database(second_path);
    Database second(second_path, Database::Mode::read_write);
    for (const auto& record : records) EXPECT_TRUE(note_apply_record(second, record));
    EXPECT_TRUE(sync_note_find(second, {"Book", "ANNOTATIONS"}));
    EXPECT_THROW((void)note_apply_record(second,
        {{"kind", "note.read_only"}, {"path", {"Book", "Chapter"}}, {"read_only", 3}}),
        NoteError);
}

TEST_F(SyncNotesTest, LargeContentRoundTrip) {
    std::mt19937 words(42);
    std::string text;
    text.reserve(3 * 1024 * 1024);
    while (text.size() < 3 * 1024 * 1024) text += static_cast<char>('a' + words() % 26);
    const auto note =
        note_create(*database_, std::nullopt, NoteItemKind::note, "Big", sync_machine);
    (void)note_save(*database_, note.id, text_document(text), sync_machine);
    note_save_compressed(*database_, note.id, text, sync_machine);
    EXPECT_EQ(sync_collect_flagged_notes(*database_, sync_machine), 1u);
    const auto record = queue_json(*database_).back();
    EXPECT_EQ(record["kind"], "note.update");
    const auto pieces = sync_split_record(record.dump());
    EXPECT_GT(pieces.size(), 1u);
    std::string joined;
    for (const auto& piece : pieces) joined += piece;
    const auto second_path = workspace_.root() / "large.sqlite3";
    create_empty_workspace_session_database(second_path);
    Database second(second_path, Database::Mode::read_write);
    create_event_history_schema(second);
    EXPECT_TRUE(note_apply_record(second, nlohmann::json::parse(joined)));
    auto original = database_->prepare("SELECT content_json FROM note_items WHERE id=?1", note.id);
    ASSERT_TRUE(original.step());
    const auto original_bytes = original.blob(0);
    const auto target = sync_note_find(second, {"Big"});
    ASSERT_TRUE(target);
    auto stored = second.prepare("SELECT content_json FROM note_items WHERE id=?1", *target);
    ASSERT_TRUE(stored.step());
    EXPECT_EQ(stored.blob(0), original_bytes);
    EXPECT_EQ(note_get_text(second, *target, true).text, text);
}

void write_key_file(
    const std::filesystem::path& root,
    std::string_view id,
    std::string_view content,
    std::uint64_t next_id) {
    std::filesystem::create_directories(root / "system" / "keys" / std::string(id));
    std::ofstream(root / "system" / "keys" / std::string(id) / "config.toml")
        << content;
    std::ofstream(root / "system" / "keys" / "config.toml")
        << "next_id = " << next_id << "\n";
}

void write_d1_source(
    const std::filesystem::path& root,
    std::string_view id = "api_key_3",
    std::string_view account = "account-A",
    std::string_view database = "database-A",
    std::string_view token = "token-A",
    std::uint64_t next_id = 4) {
    write_key_file(root, id, d1_key_toml("D1", account, database, token), next_id);
}

std::string models_key_toml(std::string_view display = "Models") {
    return "display_name = \"" + std::string(display)
        + "\"\ntype = \"models\"\nvalue = \"secret\"\ncloud_store = \"none\"\n";
}

// Simulates a prior "Save D1 credentials": the rows land directly, exactly
// as the settings screen would write them, before the store opens.
void add_d1_to_database(
    const std::filesystem::path& database,
    std::string_view id = "api_key_3",
    std::string_view account = "account-A",
    std::string_view db = "database-A",
    std::string_view token = "token-A",
    std::uint64_t next_id = 4) {
    Database handle(database, Database::Mode::read_write);
    storage::SqliteTransaction transaction(handle);
    ensure_sync_schema(handle);
    handle.prepare(
        "INSERT INTO config (name, content) VALUES (?1, ?2)",
        "system/keys/" + std::string(id) + "/config.toml",
        d1_key_toml("D1", account, db, token)).run();
    handle.prepare(
        "INSERT INTO config (name, content) VALUES ('system/keys/config.toml', ?1)",
        "next_id = " + std::to_string(next_id) + "\n").run();
    transaction.commit();
}

class SyncConfigTest : public testing::Test {
protected:
    std::filesystem::path fresh_database() {
        const auto directory = workspace_.root() / "data";
        std::filesystem::create_directories(directory);
        return directory / "workspace.sqlite3";
    }

    std::unique_ptr<WorkspaceConfigStore> open_connected(
        const std::filesystem::path& database) {
        auto store = WorkspaceConfigStore::open(database);
        store->set_sync_machine("m1");
        return store;
    }

    static std::map<std::string, std::string> config_map(
        const std::filesystem::path& database, std::string_view password = {}) {
        Database handle(database, Database::Mode::read_only, password);
        std::map<std::string, std::string> rows;
        for (const auto& row : read_workspace_config_files(handle))
            rows[row.name] = row.content;
        return rows;
    }

    test::TestWorkspace workspace_;
};

TEST_F(SyncConfigTest, ProtectedVaultInstallsTheSchemaBeforeRecording) {
    const auto database = fresh_database();
    const std::string password = "test password";
    (void)import_workspace_configuration(
        workspace_.root(), database, WorkspaceConfigLease::acquire, password);
    {
        Database legacy(database, Database::Mode::read_write, password);
        legacy.execute("DROP TABLE sync_queue");
        legacy.execute("ALTER TABLE note_items DROP COLUMN changed");
        add_d1_key(legacy);
    }
    auto store = WorkspaceConfigStore::open(database, password);
    store->set_sync_machine(std::string(sync_machine));
    const auto note = store->create_note_item({}, NoteItemKind::note, "Protected");
    (void)store->save_note(note.id, text_document("unsent"));
    Database check(database, Database::Mode::read_only, password);
    ASSERT_EQ(sync_queue_list(check).size(), 1u);
    auto flag = check.prepare("SELECT changed FROM note_items WHERE id=?", note.id);
    ASSERT_TRUE(flag.step());
    EXPECT_EQ(flag.integer(0), 1);
}

TEST_F(SyncConfigTest, SettingsEditEmitsOneRecordWithoutD1Rows) {
    workspace_.write_provider("second", "host = \"test\"\nport = 2\nmode = \"test\"\nmodel = \"fake\"\n");
    const auto database = fresh_database();
    (void)import_workspace_configuration(
        workspace_.root(), database, WorkspaceConfigLease::acquire);
    add_d1_to_database(database);
    auto store = open_connected(database);
    const auto result = store->apply_character_settings("guide", "second", std::nullopt);
    (void)result;
    Database handle(database, Database::Mode::read_only);
    const auto records = queue_json(handle);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0]["kind"], "config.edit");
    ASSERT_EQ(records[0]["set"].size(), 1u);
    EXPECT_EQ(records[0]["set"][0]["name"], "characters/guide/character.toml");
    EXPECT_TRUE(records[0]["delete"].empty());
    // The destination D1 setup is untouched.
    const auto rows = config_map(database);
    EXPECT_EQ(
        rows.at("system/keys/api_key_3/config.toml"),
        d1_key_toml("D1", "account-A", "database-A", "token-A"));
}

TEST_F(SyncConfigTest, D1DetailExposesTokenPresenceNotItsValue) {
    const auto database = fresh_database();
    (void)import_workspace_configuration(
        workspace_.root(), database, WorkspaceConfigLease::acquire);
    add_d1_to_database(database);
    auto store = open_connected(database);
    const auto read = store->read_config(
        std::vector<std::string>{"system/keys/api_key_3/config.toml"});
    ASSERT_EQ(read.files.size(), 1u);
    EXPECT_EQ(read.files.front().status, WorkspaceConfigReadStatus::key_metadata);
    ASSERT_TRUE(read.files.front().key);
    EXPECT_EQ(read.files.front().key->id, "api_key_3");
    EXPECT_EQ(read.files.front().key->type, "D1");
    EXPECT_TRUE(read.files.front().key->credential_present);
}

TEST_F(SyncConfigTest, CredentialEditsKeepTheTokenAndDoNotEnqueue) {
    const auto database = fresh_database();
    (void)import_workspace_configuration(workspace_.root(), database);
    auto store = open_connected(database);
    store->apply_d1_storage_create({
        .id = "api_key_1", .display_name = "D1", .account_id = "account",
        .database_id = "original", .api_token = "saved-token"});
    store->apply_d1_storage_update({
        .id = "api_key_1", .display_name = "D1", .account_id = "account",
        .database_id = "replacement"});
    Database handle(database, Database::Mode::read_only);
    const auto credentials = sync_read_d1(handle);
    ASSERT_TRUE(credentials);
    EXPECT_EQ(credentials->api_token, "saved-token");
    EXPECT_EQ(credentials->database_id, "replacement");
    EXPECT_TRUE(sync_queue_list(handle).empty());
}

TEST_F(SyncConfigTest, ImportIgnoresInvalidSourceD1AndKeepsTheHighCounter) {
    const auto database = fresh_database();
    (void)import_workspace_configuration(workspace_.root(), database);
    add_d1_to_database(database, "api_key_3", "A", "B", "C", 41);
    write_key_file(workspace_.root(), "api_key_3", models_key_toml(), 22);
    write_key_file(workspace_.root(), "api_key_4", "type = \"D1\"\n", 22);

    (void)import_workspace_configuration(
        workspace_.root(), database, WorkspaceConfigLease::acquire, {}, sync_machine);
    auto store = open_connected(database);
    const auto snapshot = store->snapshot();
    ASSERT_TRUE(snapshot->d1_storage());
    EXPECT_EQ(snapshot->d1_storage()->id, "api_key_41");
    EXPECT_EQ(snapshot->d1_storage()->api_token, "C");
    EXPECT_EQ(snapshot->next_api_key_id(), 42u);
    EXPECT_TRUE(snapshot->find_api_key("api_key_3"));
}

TEST_F(SyncConfigTest, ImportPreservesDestinationD1AgainstAllSourceStates) {
    const auto database = fresh_database();
    (void)import_workspace_configuration(
        workspace_.root(), database, WorkspaceConfigLease::acquire);
    add_d1_to_database(database);
    // A source with no D1 credentials changes nothing about the destination.
    workspace_.write_provider("second", "host = \"test\"\nport = 2\nmode = \"test\"\nmodel = \"fake\"\n");
    (void)import_workspace_configuration(
        workspace_.root(), database, WorkspaceConfigLease::acquire, "", sync_machine);
    auto rows = config_map(database);
    EXPECT_EQ(
        rows.at("system/keys/api_key_3/config.toml"),
        d1_key_toml("D1", "account-A", "database-A", "token-A"));
    Database handle(database, Database::Mode::read_only);
    auto records = queue_json(handle);
    ASSERT_EQ(records.size(), 1u);
    EXPECT_EQ(records[0]["kind"], "config.edit");
    for (const auto& row : records[0]["set"]) {
        EXPECT_FALSE(sync_is_d1_row(
            row["name"].get<std::string>(), row["content"].get<std::string>()));
    }
    // A source with matching or different D1 credentials is ignored the same way.
    write_key_file(
        workspace_.root(), "api_key_9", d1_key_toml("Other", "B", "C", "D"), 10);
    (void)import_workspace_configuration(
        workspace_.root(), database, WorkspaceConfigLease::acquire, "", sync_machine);
    rows = config_map(database);
    EXPECT_EQ(
        rows.at("system/keys/api_key_3/config.toml"),
        d1_key_toml("D1", "account-A", "database-A", "token-A"));
    EXPECT_FALSE(rows.contains("system/keys/api_key_9/config.toml"));
}

TEST_F(SyncConfigTest, AbsentDestinationD1StaysAbsent) {
    const auto database = fresh_database();
    (void)import_workspace_configuration(
        workspace_.root(), database, WorkspaceConfigLease::acquire);
    write_d1_source(workspace_.root());
    (void)import_workspace_configuration(
        workspace_.root(), database, WorkspaceConfigLease::acquire, "", sync_machine);
    const auto rows = config_map(database);
    for (const auto& [name, content] : rows) {
        EXPECT_FALSE(sync_is_d1_row(name, content));
    }
    Database handle(database, Database::Mode::read_only);
    EXPECT_TRUE(sync_queue_list(handle).empty());
    EXPECT_FALSE(sync_connected(handle, sync_machine));
}

TEST_F(SyncConfigTest, ImportCollisionMovesOnlyTheLocalD1Key) {
    const auto database = fresh_database();
    (void)import_workspace_configuration(
        workspace_.root(), database, WorkspaceConfigLease::acquire);
    add_d1_to_database(database);
    {
        auto store = open_connected(database);
        const auto note = store->create_note_item({}, NoteItemKind::note, "Pending");
        (void)store->save_note(note.id, text_document("unsent"));
    }
    // A source ordinary key takes the destination D1 key's path and a provider
    // references it.
    write_key_file(workspace_.root(), "api_key_3", models_key_toml(), 4);
    workspace_.write_provider(
        "keyed",
        "host = \"test\"\nport = 1\nmode = \"test\"\nmodel = \"m\"\napi_key = \"api_key_3\"\n");
    (void)import_workspace_configuration(
        workspace_.root(), database, WorkspaceConfigLease::acquire, "", sync_machine);
    const auto rows = config_map(database);
    // The source key keeps its ID; the provider reference is untouched.
    EXPECT_EQ(rows.at("system/keys/api_key_3/config.toml"), models_key_toml());
    EXPECT_TRUE(rows.at("system/providers/keyed/config.toml").find("api_key_3") != std::string::npos);
    // Only the local D1 rows moved, with values and connection intact.
    std::string relocated;
    for (const auto& [name, content] : rows) {
        if (sync_is_d1_row(name, content)) {
            EXPECT_TRUE(relocated.empty());
            relocated = name;
            EXPECT_EQ(content, d1_key_toml("D1", "account-A", "database-A", "token-A"));
        }
    }
    ASSERT_FALSE(relocated.empty());
    EXPECT_NE(relocated, "system/keys/api_key_3/config.toml");
    // The counter stays valid above every retained and allocated ID.
    const auto next_id = std::stoll(
        rows.at("system/keys/config.toml").substr(std::string("next_id = ").size()));
    for (const auto& [name, content] : rows) {
        (void)content;
        const auto slash = name.rfind('/');
        if (name.starts_with("system/keys/") && slash != std::string::npos) {
            const auto id = name.substr(
                std::string_view("system/keys/").size(),
                slash - std::string_view("system/keys/").size());
            if (id.starts_with("api_key_")) {
                EXPECT_LT(std::stoll(std::string(id.substr(8))), next_id);
            }
        }
    }
    // The emitted record carries the ordinary key and excludes D1 rows.
    Database handle(database, Database::Mode::read_only);
    const auto records = queue_json(handle);
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0]["kind"], "note.create");
    EXPECT_EQ(records[0]["path"], nlohmann::json::array({"Pending"}));
    auto pending = handle.prepare("SELECT changed FROM note_items WHERE name='Pending'");
    ASSERT_TRUE(pending.step());
    EXPECT_EQ(pending.integer(0), 1);
    bool carries_ordinary = false;
    for (const auto& row : records[1]["set"]) {
        EXPECT_FALSE(sync_is_d1_row(
            row["name"].get<std::string>(), row["content"].get<std::string>()));
        if (row["name"] == "system/keys/api_key_3/config.toml") carries_ordinary = true;
    }
    EXPECT_TRUE(carries_ordinary);
    EXPECT_TRUE(sync_connected(handle, sync_machine));
}

TEST_F(SyncConfigTest, MergePreservesDestinationD1OnCollision) {
    const auto database = fresh_database();
    (void)import_workspace_configuration(
        workspace_.root(), database, WorkspaceConfigLease::acquire);
    add_d1_to_database(database);
    test::TestWorkspace other;
    write_key_file(other.root(), "api_key_3", models_key_toml(), 4);
    other.write_provider(
        "keyed",
        "host = \"test\"\nport = 1\nmode = \"test\"\nmodel = \"m\"\napi_key = \"api_key_3\"\n");
    const auto source_db = other.root() / "workspace.sqlite3";
    (void)import_workspace_configuration(
        other.root(), source_db, WorkspaceConfigLease::acquire);
    auto store = open_connected(database);
    const SessionLease lease = SessionLease::acquire(source_db, "test");
    store->merge(source_db, lease);
    const auto rows = config_map(database);
    EXPECT_EQ(rows.at("system/keys/api_key_3/config.toml"), models_key_toml());
    std::string relocated;
    for (const auto& [name, content] : rows) {
        if (sync_is_d1_row(name, content)) {
            EXPECT_TRUE(relocated.empty());
            relocated = name;
            EXPECT_EQ(content, d1_key_toml("D1", "account-A", "database-A", "token-A"));
        }
    }
    EXPECT_FALSE(relocated.empty());
    EXPECT_NE(relocated, "system/keys/api_key_3/config.toml");
}

TEST_F(SyncConfigTest, ReceiveCollisionMovesOnlyTheLocalD1Key) {
    const auto database = fresh_database();
    (void)import_workspace_configuration(
        workspace_.root(), database, WorkspaceConfigLease::acquire);
    add_d1_to_database(database, "api_key_3", "account-W", "database-W", "token-W");
    auto store = open_connected(database);
    // The emitted record from a colliding vault carries the ordinary key at
    // the local D1 path and excludes D1 rows.
    const auto outcome = store->apply_sync_config_edit(
        {{"system/keys/api_key_3/config.toml", models_key_toml("Incoming")},
         {"system/providers/keyed/config.toml",
          "host = \"test\"\nport = 1\nmode = \"test\"\nmodel = \"m\"\napi_key = \"api_key_3\"\n"}},
        {});
    EXPECT_TRUE(outcome.applied);
    const auto rows = config_map(database);
    EXPECT_EQ(rows.at("system/keys/api_key_3/config.toml"), models_key_toml("Incoming"));
    std::string relocated;
    for (const auto& [name, content] : rows) {
        if (sync_is_d1_row(name, content)) {
            EXPECT_TRUE(relocated.empty());
            relocated = name;
            EXPECT_EQ(content, d1_key_toml("D1", "account-W", "database-W", "token-W"));
        }
    }
    EXPECT_FALSE(relocated.empty());
    EXPECT_NE(relocated, "system/keys/api_key_3/config.toml");
    // The local relocation creates no outgoing work: no echo.
    Database handle(database, Database::Mode::read_only);
    EXPECT_TRUE(sync_queue_list(handle).empty());
    EXPECT_TRUE(sync_connected(handle, sync_machine));
}

TEST_F(SyncConfigTest, ReceiveDeletionAtTheLocalD1PathPreservesSetup) {
    const auto database = fresh_database();
    (void)import_workspace_configuration(
        workspace_.root(), database, WorkspaceConfigLease::acquire);
    add_d1_to_database(database);
    auto store = open_connected(database);
    const auto outcome = store->apply_sync_config_edit(
        {}, {"system/keys/api_key_3/config.toml"});
    EXPECT_TRUE(outcome.applied);
    const auto rows = config_map(database);
    EXPECT_EQ(
        rows.at("system/keys/api_key_3/config.toml"),
        d1_key_toml("D1", "account-A", "database-A", "token-A"));
    Database handle(database, Database::Mode::read_only);
    EXPECT_TRUE(sync_queue_list(handle).empty());
}

TEST_F(SyncConfigTest, ReceiveInvalidCandidateIsSkipped) {
    const auto database = fresh_database();
    (void)import_workspace_configuration(
        workspace_.root(), database, WorkspaceConfigLease::acquire);
    add_d1_to_database(database);
    auto store = open_connected(database);
    const auto before = config_map(database);
    const auto fail_if_drained = [] {
        ADD_FAILURE() << "Invalid incoming configuration requested maintenance.";
        return true;
    };
    EXPECT_FALSE(store->apply_sync_config_edit(
        {{"../evil.toml", "x"}}, {}, fail_if_drained).applied);
    EXPECT_FALSE(store->apply_sync_config_edit(
        {{"system/keys/config.toml", "next_id = \"invalid\"\n"}},
        {}, fail_if_drained).applied);
    EXPECT_EQ(config_map(database), before);
    Database handle(database, Database::Mode::read_only);
    EXPECT_TRUE(sync_queue_list(handle).empty());
}

TEST_F(SyncConfigTest, ReceiveMultiForumRemovalPrunesSessionsAtomically) {
    workspace_.add_forum("second", "Second", "guide");
    const auto database = fresh_database();
    (void)import_workspace_configuration(
        workspace_.root(), database, WorkspaceConfigLease::acquire);
    add_d1_to_database(database);
    std::map<std::string, std::int64_t> forum_keys{{"lobby", 1}, {"second", 2}};
    {
        Database setup(database, Database::Mode::read_write);
        for (const auto& [id, key] : forum_keys) {
            setup.prepare("INSERT INTO forums(forum_key,forum_id) VALUES (?1,?2)", key, id).run();
            setup.prepare(
                "INSERT INTO sessions(forum_key,session_id,label,updated_at,history_epoch,"
                "next_entry_id,next_request_id) VALUES (?1,?2,'Chat',0,1,1,1)",
                key, "20261109-120000-" + id).run();
        }
    }
    auto store = open_connected(database);
    std::vector<std::string> removed;
    {
        Database handle(database, Database::Mode::read_only);
        for (const auto& row : read_workspace_config_files(handle)) {
            if (row.name == "forums/second" || row.name.starts_with("forums/second/"))
                removed.push_back(row.name);
        }
    }
    ASSERT_FALSE(removed.empty());
    const auto deferred = store->apply_sync_config_edit({}, removed, [] { return false; });
    EXPECT_TRUE(deferred.retry);
    EXPECT_FALSE(deferred.applied);
    const auto outcome = store->apply_sync_config_edit({}, removed, [&] {
        // Maintenance may acquire the store lock while active sessions drain.
        return !store->list_note_items().empty();
    });
    EXPECT_TRUE(outcome.applied);
    EXPECT_TRUE(outcome.notes_changed);
    const auto rows = config_map(database);
    for (const auto& [name, content] : rows) {
        (void)content;
        EXPECT_FALSE(
            name == "forums/second" || name.starts_with("forums/second/"));
    }
    Database check(database, Database::Mode::read_only);
    auto sessions = check.prepare("SELECT forum_id FROM sessions s JOIN forums f USING(forum_key)");
    std::vector<std::string> remaining;
    while (sessions.step()) remaining.push_back(sessions.text(0));
    EXPECT_EQ(remaining, std::vector<std::string>{"lobby"});
    EXPECT_TRUE(sync_queue_list(check).empty());
}

TEST(SyncMachineConfig, LoadsMachineName) {
    const auto root = std::filesystem::temp_directory_path()
        / ("cha_machine_"
            + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    const auto write_app = [&](std::string_view machine_line) {
        std::ofstream(root / "app.toml")
            << "vault = \"Personal\"\n" << machine_line << "[logging]\nfile = \"cha.log\"\nlevel = \"info\"\n";
        std::ofstream(root / "personal.toml")
            << "vault_name = \"Personal\"\ndata = \"data.sqlite3\"\n";
    };
    write_app("machine = \"mac\"\n");
    EXPECT_EQ(load_configuration_directory(root).machine, "mac");
    write_app("machine = \"bad/name\"\n");
    const auto warned = load_configuration_directory(root);
    EXPECT_TRUE(warned.machine.empty());
    EXPECT_FALSE(warned.warnings.empty());
    write_app("");
    EXPECT_TRUE(load_configuration_directory(root).machine.empty());
    std::error_code error;
    std::filesystem::remove_all(root, error);
}


} // namespace
} // namespace cha
