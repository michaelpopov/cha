#include "app/application.h"
#include "app/application_config.h"
#include "app/d1_client.h"
#include "storage/event_storage.h"
#include "storage/sync_storage.h"
#include "storage/note_storage.h"
#include "storage/workspace_session_database.h"
#include "support/mock_http_server.h"
#include "support/test_workspace.h"
#include "util/path_name.h"
#include "util/logging.h"
#include "workspace/prompt_catalog.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iterator>
#include <thread>

namespace cha {
namespace {
using app::Application;
using app::ApplicationError;
using app::ApplicationState;
using namespace std::chrono_literals;

using Database = storage::SqliteDatabase;

ApplicationCommand make_d1_command(
    test::TestWorkspace& workspace,
    const std::filesystem::path& database,
    std::string machine = "mac",
    bool password_protected = false) {
    const std::filesystem::path config_directory = workspace.root() / "cha-config";
    std::filesystem::create_directories(config_directory);
    {
        std::ofstream app(config_directory / "app.toml");
        app << "vault = \"Test\"\n";
        if (!machine.empty()) app << "machine = \"" << machine << "\"\n";
        app << "[logging]\nfile = \"runtime.log\"\nlevel = \"off\"\n";
    }
    {
        std::ofstream vault(config_directory / "test.toml");
        vault << "vault_name = \"Test\"\n"
              << "data = " << std::quoted(utf8_path(database)) << "\n";
        if (password_protected) vault << "protected = true\n";
    }
    const ConfigurationDirectory loaded =
        load_configuration_directory(config_directory);
    const VaultDefinition* const found =
        find_vault(loaded.vaults, loaded.startup_vault);
    return {
        .config_directory = loaded.directory,
        .machine = loaded.machine,
        .vaults = loaded.vaults,
        .vault = *found,
        .log_file = loaded.log_file,
        .log_level = loaded.log_level,
        .warnings = loaded.warnings,
    };
}

nlohmann::json batch_ok(std::size_t count) {
    nlohmann::json results = nlohmann::json::array();
    for (std::size_t i = 0; i < count; ++i)
        results.push_back({{"success", true}, {"results", nlohmann::json::array()}});
    return {{"success", true}, {"result", std::move(results)}};
}

nlohmann::json tables_ok(std::vector<std::string> names) {
    nlohmann::json rows = nlohmann::json::array();
    for (auto& name : names) rows.push_back({{"name", std::move(name)}});
    nlohmann::json results = nlohmann::json::array();
    results.push_back({{"success", true}, {"results", std::move(rows)}});
    return {{"success", true}, {"result", std::move(results)}};
}

struct MockD1 {
    std::atomic_bool fail_create{};
    std::atomic_bool fail_insert{};
    std::atomic_bool fail_drop{};
    std::chrono::milliseconds insert_delay{};
    bool local_queue_exists = true;
    bool local_v2_exists = false;
    std::string local_table{"q/Test/mac"};
    std::vector<std::string> other_tables{"q/Test/other"};
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> statements;
    std::vector<std::string> create_headers;
    nlohmann::json incoming = nlohmann::json::array();
    nlohmann::json incoming_v2 = nlohmann::json::array();
    nlohmann::json sent_v2 = nlohmann::json::array();
    nlohmann::json acknowledged_v2 = nlohmann::json::array();

    D1Client::Transport transport() {
        return [this](const std::string&, const std::string& body,
                   const std::vector<std::string>& headers) -> std::pair<long, std::string> {
            const auto batch = nlohmann::json::parse(body).at("batch");
            const auto sql = batch.at(0).at("sql").get<std::string>();
            const auto local_v2 = "q2" + local_table.substr(1);
            bool have_local;
            bool have_v2;
            nlohmann::json rows;
            {
                const std::lock_guard lock(mutex);
                statements.push_back(sql);
                if (sql.starts_with("CREATE TABLE") && !fail_create) {
                    if (sql.find(sync_escape_identifier(local_v2)) != std::string::npos)
                        local_v2_exists = true;
                    create_headers = headers;
                }
                if (sql.starts_with("SELECT rowid")) rows = incoming;
                if (sql.starts_with("SELECT seq")) rows = incoming_v2;
                if (sql.starts_with("DELETE FROM")) {
                    if (sql.find("op_id IN") == std::string::npos) incoming.clear();
                    else {
                        const auto& ids = batch.at(0).at("params");
                        for (const auto& id : ids) acknowledged_v2.push_back(id);
                        for (auto row = incoming_v2.begin(); row != incoming_v2.end();) {
                            if (std::ranges::find(ids, row->at("op_id")) != ids.end())
                                row = incoming_v2.erase(row);
                            else ++row;
                        }
                    }
                }
                if (sql.starts_with("INSERT") && !fail_insert) {
                    const auto& params = batch.at(0).at("params");
                    sent_v2.push_back({{"seq", sent_v2.size() + 1}, {"op_id", params.at(0)},
                        {"pieces", std::stoi(params.at(2).get<std::string>())}, {"record", params.at(3)}});
                }
                have_local = local_queue_exists;
                have_v2 = local_v2_exists;
            }
            changed.notify_all();
            if (sql.starts_with("SELECT name FROM sqlite_master")) {
                auto tables = other_tables;
                if (have_local) tables.push_back(local_table);
                if (have_v2) tables.push_back(local_v2);
                return {200, tables_ok(std::move(tables)).dump()};
            }
            if (sql.starts_with("SELECT rowid") || sql.starts_with("SELECT seq")) {
                if (!(sql.starts_with("SELECT seq") ? have_v2 : have_local)) {
                    return {400, R"({"success":false,"errors":[{"message":"no such table: q"}]})"};
                }
                auto response = batch_ok(1);
                response["result"][0]["results"] = std::move(rows);
                return {200, response.dump()};
            }
            if (sql.starts_with("INSERT")) std::this_thread::sleep_for(insert_delay);
            if ((sql.starts_with("CREATE TABLE") && fail_create)
                || (sql.starts_with("INSERT") && fail_insert)
                || (sql.starts_with("DROP TABLE") && fail_drop)) {
                return {500, R"({"success":false})"};
            }
            return {200, batch_ok(batch.size()).dump()};
        };
    }

    bool wait_for_statement(std::string_view prefix, std::size_t count = 1) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 5s, [&] {
            return std::ranges::count_if(statements,
                [&](const auto& sql) { return sql.starts_with(prefix); })
                >= static_cast<std::ptrdiff_t>(count);
        });
    }

    bool wait_for_reads(std::size_t count = 1) { return wait_for_statement("SELECT rowid", count); }

    std::size_t count(std::string_view prefix) {
        const std::lock_guard lock(mutex);
        return std::ranges::count_if(statements,
            [&](const auto& sql) { return sql.starts_with(prefix); });
    }
};

nlohmann::json ready_row(int seq, std::string_view id, std::string record) {
    return {{"seq", seq}, {"op_id", id}, {"pieces", 1}, {"record", std::move(record)}};
}

SaveD1StorageRequest credentials() {
    return {.display_name = "D1", .account_id = "a", .database_id = "d", .api_token = "t"};
}

std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

SaveR2StorageRequest r2_credentials(int port) {
    return {
        .display_name = "R2", .url = "http://127.0.0.1:" + std::to_string(port) + "/cha-backups/",
        .access_key_id = "k", .secret_key = "s",
    };
}

void seed_d1(const std::filesystem::path& path, std::string_view password = {}) {
    Database db(path, Database::Mode::read_write, password);
    db.prepare(
        "INSERT INTO config (name, content) VALUES (?1, ?2)",
        "system/keys/api_key_1/config.toml",
        "display_name = \"D1\"\ntype = \"D1\"\naccount_id = \"a\"\n"
        "database_id = \"d\"\napi_token = \"t\"\n").run();
}

void queue_work(const std::filesystem::path& path, std::string_view name = "Pending") {
    Database db(path, Database::Mode::read_write);
    sync_queue_append(db, sync_make_record("note.create", {{"path", {name}}, {"item", "note"}}));
}

void flag_note(const std::filesystem::path& path) {
    Database db(path, Database::Mode::read_write);
    const auto note = note_create(db, std::nullopt, NoteItemKind::note, "Flagged");
    db.prepare("UPDATE note_items SET changed=1 WHERE id=?1", note.id).run();
}

TEST(D1CredentialsTest, IncompleteSavedKeyAllowsLocalWorkAndReopen) {
    for (const std::string machine : {"mac", ""}) {
        SCOPED_TRACE(machine);
        test::TestWorkspace workspace;
        const auto path = test::import_test_database(workspace.root());
        seed_d1(path);
        queue_work(path);
        flag_note(path);
        {
            Database db(path, Database::Mode::read_write);
            db.execute("UPDATE config SET content=replace(content, 'api_token = \"t\"', "
                "'api_token = \"\"') WHERE name='system/keys/api_key_1/config.toml'");
        }
        auto command = make_d1_command(workspace, path, machine);
        MockD1 mock;
        auto application = Application::open(command, {}, {}, mock.transport());
        const auto epoch = application->context_epoch();
        const auto saved = application->get_d1_storage(epoch);
        ASSERT_TRUE(saved);
        EXPECT_FALSE(saved->has_api_token);
        const auto note = application->create_note_item({}, NoteItemKind::note, "Local", epoch);
        (void)application->save_note(note.id, nlohmann::json::parse(
            R"({"type":"doc","content":[{"type":"paragraph","content":[{"type":"text","text":"Local edit"}]}]})"), epoch);
        application->request_shutdown();
        ASSERT_TRUE(application->join_shutdown(5s));
        application.reset();
        application = Application::open(command, {}, {}, mock.transport());
        EXPECT_EQ(application->bootstrap().state, ApplicationState::running);
        {
            Database db(path, Database::Mode::read_only);
            EXPECT_EQ(note_get_text(db, note.id).text, "Local edit");
            EXPECT_FALSE(sync_read_d1(db));
            EXPECT_EQ(sync_queue_list(db).size(), 1u);
            auto flags = db.prepare("SELECT name FROM note_items WHERE changed<>0");
            ASSERT_TRUE(flags.step());
            EXPECT_EQ(flags.text(0), "Flagged");
            EXPECT_FALSE(flags.step());
        }
        EXPECT_EQ(mock.count(""), 0u);
        if (!machine.empty()) {
            const auto repaired = application->save_d1_storage(
                credentials(), application->context_epoch());
            EXPECT_EQ(repaired.id, saved->id);
            EXPECT_TRUE(repaired.has_api_token);
        }
    }
}

TEST(D1AppWiringTest, MalformedPieceMetadataSkipsOnlyTheBadRows) {
    test::TestWorkspace workspace;
    const auto path = test::import_test_database(workspace.root());
    seed_d1(path);
    auto command = make_d1_command(workspace, path);
    MockD1 mock;
    mock.local_v2_exists = true;
    const auto folder = [](std::string_view name) {
        return sync_make_record("note.create", {{"path", {name}}, {"item", "folder"}});
    };
    mock.incoming = nlohmann::json::array({
        {{"rowid", 1}, {"piece", 1}, {"pieces", 1}, {"record", folder("Before")}},
        {{"rowid", 2}, {"piece", 1}, {"pieces", 2}, {"record", folder("Incomplete")}},
        {{"rowid", 3}, {"piece", "garbage"}, {"pieces", 1}, {"record", folder("Invalid")}},
        {{"rowid", 4}, {"piece", 1.5}, {"pieces", 1}, {"record", folder("Invalid")}},
        {{"rowid", 5}, {"piece", nullptr}, {"pieces", 1}, {"record", folder("Invalid")}},
        {{"rowid", 6}, {"piece", 1}, {"record", folder("Invalid")}},
        {{"rowid", 7}, {"piece", 1}, {"pieces", 1}, {"record", nlohmann::json::array({0})}},
        {{"rowid", 8}, {"piece", 1}, {"pieces", 1}, {"record", folder("After")}},
    });
    auto application = Application::open(command, {}, {}, mock.transport());
    Database db(path, Database::Mode::read_only);
    EXPECT_TRUE(sync_note_find(db, {"Before"}));
    EXPECT_TRUE(sync_note_find(db, {"After"}));
    EXPECT_FALSE(sync_note_find(db, {"Incomplete"}));
    EXPECT_FALSE(sync_note_find(db, {"Invalid"}));
    EXPECT_EQ(mock.count("DELETE FROM"), 1u);
}

TEST(D1AppWiringTest, FractionalChatMetadataIsSkippedAndAcknowledged) {
    test::TestWorkspace workspace;
    const auto path = test::import_test_database(workspace.root());
    seed_d1(path);
    auto command = make_d1_command(workspace, path);
    MockD1 mock;
    const nlohmann::json turn = {
        {"kind", "chat.entries"}, {"chat", {"lobby", "sync-chat"}}, {"label", "Synced"},
        {"state", 1},
        {"entries", {
            {{"kind", 0}, {"status", 0}, {"participant_id", "u"}, {"display_name", "U"},
             {"addressed_to", "guide"}, {"addressed_to_name", "Guide"}, {"text", "Prompt"}},
            {{"kind", 1}, {"status", 0}, {"participant_id", "guide"}, {"display_name", "Guide"},
             {"text", "Reply"}, {"input_tokens", nullptr}, {"output_tokens", 0}},
        }},
    };
    const auto push = [&](const nlohmann::json& record) {
        mock.incoming.push_back({{"rowid", mock.incoming.size() + 1}, {"piece", 1},
            {"pieces", 1}, {"record", record.dump()}});
    };
    push(turn);
    for (const auto* field : {"created_at", "input_tokens", "output_tokens"}) {
        auto malformed = turn;
        malformed["entries"][1][field] = 1.5;
        push(malformed);
    }
    push({{"kind", "note.create"}, {"path", {"Sentinel"}}, {"item", "folder"}});
    auto application = Application::open(command, {}, {}, mock.transport());
    Database db(path, Database::Mode::read_only);
    auto turns = db.prepare("SELECT count(*) FROM turns");
    ASSERT_TRUE(turns.step());
    EXPECT_EQ(turns.integer(0), 1);
    EXPECT_TRUE(sync_note_find(db, {"Sentinel"}));
    EXPECT_EQ(mock.count("DELETE FROM"), 1u);
}

TEST(D1AppWiringTest, OversizedPageNeverAppliesOrAcknowledges) {
    for (const bool omit_row : {false, true}) {
        SCOPED_TRACE(omit_row);
        test::TestWorkspace workspace;
        const auto path = test::import_test_database(workspace.root());
        seed_d1(path);
        auto command = make_d1_command(workspace, path);
        MockD1 mock;
        for (int id = 1; id <= (omit_row ? 52 : 51); ++id) {
            if (omit_row && id == 4) continue;
            mock.incoming.push_back({{"rowid", id}, {"piece", 1}, {"pieces", 1},
                {"record", sync_make_record("note.create",
                    {{"path", {"Incoming " + std::to_string(id)}}, {"item", "folder"}})}});
        }
        auto application = Application::open(command, {}, {}, mock.transport());
        Database db(path, Database::Mode::read_only);
        auto count = db.prepare("SELECT count(*) FROM note_items WHERE name LIKE 'Incoming %'");
        ASSERT_TRUE(count.step());
        EXPECT_EQ(count.integer(0), 0);
        EXPECT_EQ(mock.count("DELETE FROM"), 0u);
    }
}

TEST(D1CredentialsTest, SaveCreatesTableFirstAndStartsWorker) {
    test::TestWorkspace workspace;
    const auto database = test::import_test_database(workspace.root());
    auto command = make_d1_command(workspace, database);
    MockD1 mock;
    mock.local_queue_exists = false;
    mock.other_tables.clear();
    auto application = Application::open(command, {}, {}, mock.transport());
    auto epoch = application->context_epoch();
    const auto saved = application->save_d1_storage(credentials(), epoch);
    EXPECT_EQ(saved.display_name, "D1");
    EXPECT_TRUE(saved.has_api_token);
    ASSERT_TRUE(mock.wait_for_statement("SELECT seq"));
    application->request_shutdown();
    ASSERT_TRUE(application->join_shutdown(5s));
    application.reset();

    mock.local_v2_exists = false;
    application = Application::open(command, {}, {}, mock.transport());
    epoch = application->context_epoch();
    EXPECT_EQ(mock.count("CREATE TABLE"), 1u);
    EXPECT_EQ(mock.count("SELECT seq"), 2u);

    auto unchanged = credentials();
    unchanged.api_token.reset();
    const auto saved_again = application->save_d1_storage(unchanged, epoch);
    EXPECT_EQ(saved_again.database_id, saved.database_id);
    EXPECT_TRUE(saved_again.has_api_token);
    ASSERT_TRUE(mock.wait_for_statement("SELECT seq", 3));
    EXPECT_EQ(mock.count("CREATE TABLE"), 2u);
    EXPECT_EQ(mock.count("CREATE TABLE IF NOT EXISTS \"q2/Test/mac\""), 2u);
    EXPECT_FALSE(mock.local_queue_exists);
    EXPECT_EQ(mock.create_headers, std::vector<std::string>{"Authorization: Bearer t"});
    application->request_shutdown();
    EXPECT_TRUE(application->join_shutdown(5s));
}

TEST(D1CredentialsTest, SaveFailureLeavesStoredUnchanged) {
    test::TestWorkspace workspace;
    const auto database = test::import_test_database(workspace.root());
    auto command = make_d1_command(workspace, database);
    MockD1 mock;
    auto application = Application::open(command, {}, {}, mock.transport());
    const auto epoch = application->context_epoch();
    (void)application->save_d1_storage(credentials(), epoch);
    ASSERT_TRUE(mock.wait_for_reads());
    mock.fail_create = true;
    auto replacement = credentials();
    replacement.database_id = "replacement";
    EXPECT_THROW((void)application->save_d1_storage(replacement, epoch), ApplicationError);
    const auto saved = application->get_d1_storage(epoch);
    ASSERT_TRUE(saved);
    EXPECT_EQ(saved->database_id, "d");
    application->request_shutdown();
    EXPECT_TRUE(application->join_shutdown(5s));
}

TEST(D1CredentialsTest, OfflineDisconnectClearsLocally) {
    test::TestWorkspace workspace;
    const auto database = test::import_test_database(workspace.root());
    auto command = make_d1_command(workspace, database);
    MockD1 mock;
    mock.other_tables = {"q2/Test/other"};
    const std::string received_id(32, 'a');
    mock.incoming_v2 = {ready_row(1, received_id, sync_make_record("note.create",
        {{"path", {"Received"}}, {"item", "folder"}}))};
    queue_work(database, "Sent");
    auto application = Application::open(command, {}, {}, mock.transport());
    const auto epoch = application->context_epoch();
    (void)application->save_d1_storage(credentials(), epoch);
    ASSERT_TRUE(mock.wait_for_statement("DELETE FROM \"q2/Test/mac\""));
    const auto sent_id = mock.sent_v2.at(0).at("op_id").get<std::string>();
    const auto snapshot = workspace.root() / "snapshot.sqlite3";
    copy_workspace_session_database(database, snapshot);

    queue_work(database);
    flag_note(database);
    std::string pending_id;
    {
        Database db(database, Database::Mode::read_only);
        const auto pending = sync_queue_list(db);
        ASSERT_EQ(pending.size(), 1u);
        pending_id = pending.front().op_id;
        EXPECT_TRUE(sync_seen(db, pending_id));
    }
    mock.fail_drop = true;
    application->disconnect_d1_storage(epoch);
    EXPECT_FALSE(application->get_d1_storage(epoch).has_value());
    EXPECT_EQ(mock.count("DROP TABLE \"q2/Test/mac\""), 1u);
    EXPECT_EQ(mock.count("DROP TABLE \"q/Test/mac\""), 1u);
    {
        Database db(database, Database::Mode::read_only);
        EXPECT_FALSE(sync_has_pending(db));
        EXPECT_FALSE(sync_read_d1(db));
        EXPECT_FALSE(sync_seen(db, pending_id));
        EXPECT_TRUE(sync_seen(db, sent_id));
        EXPECT_TRUE(sync_seen(db, received_id));
    }

    // The saved snapshot lacks the discarded ID. Recovery recreates the local
    // inbox if it has gone missing since the snapshot was taken.
    mock.local_v2_exists = false;
    MockHttpServer backup({http_response("application/toml", file_bytes(command.vault.source)),
        http_response("application/vnd.sqlite3", file_bytes(snapshot))});
    (void)application->save_r2_storage(r2_credentials(backup.port()), epoch);
    backup.start();
    (void)application->download_database(epoch);
    backup.join();
    EXPECT_EQ(mock.count("CREATE TABLE IF NOT EXISTS \"q2/Test/mac\""), 2u);
    EXPECT_TRUE(application->get_d1_storage(application->context_epoch()));
    EXPECT_EQ(backup.requests().size(), 2u);
    application->request_shutdown();
    EXPECT_TRUE(application->join_shutdown(5s));
}

TEST(D1CredentialsTest, FailedDisconnectRetainsCredentialsQueueAndFlags) {
    test::TestWorkspace workspace;
    const auto database = test::import_test_database(workspace.root());
    auto command = make_d1_command(workspace, database);
    MockD1 mock;
    auto application = Application::open(command, {}, {}, mock.transport());
    const auto epoch = application->context_epoch();
    (void)application->save_d1_storage(credentials(), epoch);
    ASSERT_TRUE(mock.wait_for_statement("SELECT seq"));
    queue_work(database);
    flag_note(database);
    force_next_workspace_config_fault(WorkspaceConfigFault::sqlite_commit);
    EXPECT_THROW(application->disconnect_d1_storage(epoch), ApplicationError);
    EXPECT_TRUE(application->get_d1_storage(epoch));
    EXPECT_EQ(mock.count("DROP TABLE"), 0u);
    Database db(database, Database::Mode::read_only);
    const auto pending = sync_queue_list(db);
    ASSERT_EQ(pending.size(), 1u);
    EXPECT_TRUE(sync_seen(db, pending.front().op_id));
    auto flags = db.prepare("SELECT COUNT(*) FROM note_items WHERE changed=1");
    ASSERT_TRUE(flags.step());
    EXPECT_EQ(flags.integer(0), 1);
}

TEST(D1AppWiringTest, AppliesNewForumThenChatBeforeOpenReturns) {
    test::TestWorkspace source;
    source.add_forum("remote", "Remote", "guide");
    const auto source_path = test::import_test_database(source.root());
    Database remote(source_path, Database::Mode::read_only);
    nlohmann::json set_rows = nlohmann::json::array();
    for (const auto& row : read_workspace_config_files(remote)) {
        if (row.name.starts_with("forums/remote/"))
            set_rows.push_back({{"name", row.name}, {"content", row.content}});
    }
    ASSERT_FALSE(set_rows.empty());

    test::TestWorkspace workspace;
    const auto database = test::import_test_database(workspace.root());
    seed_d1(database);
    MockD1 mock;
    const std::string config_id(32, '1');
    const std::string no_op_id(32, '2');
    const std::string rejected_id(32, '3');
    const std::string malformed_id(32, '4');
    const std::string chat_id(32, '5');
    const auto delta = sync_make_record(
        "config.edit", {{"set", set_rows}, {"delete", nlohmann::json::array()}});
    const auto rejected = sync_make_record("config.edit",
        {{"set", nlohmann::json::array()}, {"delete", {"../invalid-path-with-private-text"}}});
    mock.local_v2_exists = true;
    mock.incoming_v2 = {
        ready_row(1, config_id, delta),
        ready_row(2, no_op_id, delta),
        ready_row(3, rejected_id, rejected),
        ready_row(4, malformed_id, "malformed private payload"),
        ready_row(5, chat_id, sync_make_record(
            "chat.rename", {{"chat", {"remote", "session-from-d1"}}, {"label", "Shared chat"}})),
    };
    auto application = Application::open(
        make_d1_command(workspace, database), {}, {}, mock.transport());
    const auto chats = application->list_sessions("remote", application->context_epoch());
    ASSERT_EQ(chats.size(), 1u);
    EXPECT_EQ(chats.front().id, "session-from-d1");
    EXPECT_EQ(chats.front().label, "Shared chat");
    EXPECT_EQ(mock.count("DELETE FROM"), 1u);
    EXPECT_TRUE(mock.incoming_v2.empty());
    EXPECT_EQ(mock.acknowledged_v2,
        nlohmann::json::array({config_id, no_op_id, rejected_id, malformed_id, chat_id}));
    Database db(database, Database::Mode::read_only);
    EXPECT_FALSE(sync_has_pending(db));
    EXPECT_TRUE(sync_seen(db, config_id));
    EXPECT_TRUE(sync_seen(db, no_op_id));
    EXPECT_FALSE(sync_seen(db, rejected_id));
    EXPECT_FALSE(sync_seen(db, malformed_id));
    EXPECT_TRUE(sync_seen(db, chat_id));
    const auto events = application->list_events(application->context_epoch());
    for (const auto& [id, reason] : {std::pair{rejected_id, "configuration change rejected"},
             std::pair{malformed_id, "invalid record"}}) {
        EXPECT_NE(std::ranges::find_if(events, [&](const ActivityEvent& event) {
            return event.severity == EventSeverity::warning
                && event.message == "D1 skipped operation " + id + " (" + reason + ").";
        }), events.end());
    }
}

TEST(D1AppWiringTest, SettingsCreatedForumReceivesPeerChatWithoutLocalOpen) {
    test::TestWorkspace workspace;
    const auto database = test::import_test_database(workspace.root());
    seed_d1(database);
    MockD1 mock;
    auto application = Application::open(
        make_d1_command(workspace, database), {}, {}, mock.transport());
    const auto epoch = application->context_epoch();
    const auto forum = application->create_forum(
        {.display_name = "Shared forum", .persona_id = "reader"}, epoch);
    const auto& forum_id = forum.summary.id;
    {
        Database db(database, Database::Mode::read_only);
        auto row = db.prepare("SELECT 1 FROM forums WHERE forum_id=?1", forum_id);
        EXPECT_TRUE(row.step());
        const auto pending = sync_queue_list(db);
        ASSERT_EQ(pending.size(), 1u);
        const auto edit = nlohmann::json::parse(pending.front().record);
        EXPECT_EQ(edit["kind"], "config.edit");
        EXPECT_TRUE(std::ranges::any_of(edit["set"], [&](const auto& file) {
            return file["name"] == "forums/" + forum_id + "/config.toml";
        }));
    }
    {
        const std::lock_guard lock(mock.mutex);
        mock.incoming = {
            {{"rowid", 1}, {"piece", 1}, {"pieces", 1}, {"record", sync_make_record(
                "chat.rename", {{"chat", {forum_id, "session-from-d1"}}, {"label", "Peer chat"}})}},
        };
    }
    // Saving the same credentials starts a pass immediately on the open vault.
    (void)application->save_d1_storage(credentials(), epoch);
    {
        std::unique_lock lock(mock.mutex);
        ASSERT_TRUE(mock.changed.wait_for(lock, 5s, [&] { return mock.incoming.empty(); }));
    }
    const auto chats = application->list_sessions(forum_id, epoch);
    ASSERT_EQ(chats.size(), 1u);
    EXPECT_EQ(chats.front().id, "session-from-d1");
}

TEST(D1AppWiringTest, InitializesMissingForumBeforeReceivingChats) {
    test::TestWorkspace workspace;
    const auto database = test::import_test_database(workspace.root());
    seed_d1(database);
    {
        // An older vault can lack the forum row. Open adds it before the first receive.
        Database db(database, Database::Mode::read_write);
        db.execute("DELETE FROM forums WHERE forum_id='lobby'");
    }
    MockD1 mock;
    mock.incoming = {
        {{"rowid", 1}, {"piece", 1}, {"pieces", 1}, {"record", sync_make_record(
            "chat.rename", {{"chat", {"lobby", "session-from-d1"}}, {"label", "Incoming chat"}})}},
    };
    auto application = Application::open(
        make_d1_command(workspace, database), {}, {}, mock.transport());
    const auto chats = application->list_sessions("lobby", application->context_epoch());
    ASSERT_EQ(chats.size(), 1u);
    EXPECT_EQ(chats.front().id, "session-from-d1");
    EXPECT_EQ(chats.front().label, "Incoming chat");
}

TEST(D1AppWiringTest, IncomingCharacterSettingsUseOrdinaryValidation) {
    test::TestWorkspace workspace;
    const auto database = test::import_test_database(workspace.root());
    seed_d1(database);
    // Each record names its own chat, so an applied invalid record adds a row.
    const auto character = [](std::string session_id, std::string character_id,
                               nlohmann::json provider, nlohmann::json effort) {
        return sync_make_record("chat.character", {
            {"chat", {"lobby", std::move(session_id)}}, {"label", "Chat"},
            {"character_id", std::move(character_id)},
            {"provider_id", std::move(provider)}, {"reasoning_effort", std::move(effort)}});
    };
    const std::vector<std::string> records{
        character("s1", "missing", "test", nullptr),
        character("s2", "guide", "missing", nullptr),
        character("s3", "guide", nullptr, "invalid"),
        character("s4", "guide", "test", "high"),
    };
    MockD1 mock;
    for (std::size_t i = 0; i < records.size(); ++i) {
        mock.incoming.push_back(
            {{"rowid", i + 1}, {"piece", 1}, {"pieces", 1}, {"record", records[i]}});
    }
    auto application = Application::open(
        make_d1_command(workspace, database), {}, {}, mock.transport());
    EXPECT_EQ(mock.count("DELETE FROM"), 1u);
    Database db(database, Database::Mode::read_only);
    auto rows = db.prepare(
        "SELECT character_id, provider_id, reasoning_effort FROM session_character_settings");
    ASSERT_TRUE(rows.step());
    EXPECT_EQ(rows.text(0), "guide");
    EXPECT_EQ(rows.text(1), "test");
    EXPECT_EQ(rows.text(2), "high");
    EXPECT_FALSE(rows.step());
}

class D1HandoffTest : public testing::TestWithParam<bool> {};

TEST_P(D1HandoffTest, CloseSendsEditsForTheOtherVaultToApplyOnOpen) {
    const bool protected_vault = GetParam();
    const std::string password = protected_vault ? "test-password" : "";
    test::TestWorkspace source;
    test::TestWorkspace receiver;
    const auto source_path = test::import_test_database(source.root());
    const auto receiver_path = receiver.root() / "workspace.sqlite3";
    if (protected_vault) protect_workspace_session_database(source_path, password);
    copy_workspace_session_database(source_path, receiver_path, password, password);
    const auto source_command = make_d1_command(source, source_path, "mac", protected_vault);
    const auto receiver_command = make_d1_command(receiver, receiver_path, "linux", protected_vault);
    MockD1 receive;
    receive.local_table = "q/Test/linux";
    receive.other_tables = {"q2/Test/mac"};
    {
        auto application = Application::open(receiver_command, password, {}, receive.transport());
        (void)application->save_d1_storage(credentials(), application->context_epoch());
        ASSERT_TRUE(receive.wait_for_reads());
        application->request_shutdown();
        ASSERT_TRUE(application->join_shutdown(5s));
    }

    MockD1 send;
    send.other_tables = {"q2/Test/linux"};
    auto application = Application::open(source_command, password, {}, send.transport());
    const auto epoch = application->context_epoch();
    (void)application->save_d1_storage(credentials(), epoch);
    ASSERT_TRUE(send.wait_for_reads());
    (void)application->update_prompt("note-compression", {"Replicated prompt"}, epoch);
    const auto note = application->create_note_item({}, NoteItemKind::note, "Shared note", epoch);
    const auto document = nlohmann::json::parse(R"({
        "type":"doc","content":[{"type":"paragraph","content":[{"type":"text","text":"From A"}]}]
    })");
    (void)application->save_note(note.id, document, epoch);
    const auto key = application->create_api_key(
        {.display_name = "Shared key", .value = "test-key"}, epoch);
    (void)application->save_r2_storage(
        {.display_name = "R2", .url = "http://127.0.0.1:1/backups/",
         .access_key_id = "k", .secret_key = "s"}, epoch);
    const auto chat = application->create_session("lobby", "Shared chat", epoch);
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
        application->open_session("lobby", chat.id, epoch)));
    ASSERT_TRUE(std::holds_alternative<CommandResult>(
        application->submit("lobby", chat.id, RawCommand{"From A"}, epoch)));
    SessionSnapshot source_chat;
    for (const auto deadline = std::chrono::steady_clock::now() + 5s;
         std::chrono::steady_clock::now() < deadline;) {
        source_chat = std::get<SessionSnapshot>(application->snapshot("lobby", chat.id, epoch));
        if (!source_chat.generation.active) break;
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_FALSE(source_chat.generation.active);
    ASSERT_GE(source_chat.transcript.size(), 2u);
    application->request_shutdown();
    ASSERT_TRUE(application->join_shutdown(5s));
    EXPECT_EQ(send.count("INSERT"), 6u);  // Note create and update, three config edits, one turn.
    {
        Database db(source_path, Database::Mode::read_only, password);
        EXPECT_FALSE(sync_has_pending(db));
    }

    receive.incoming_v2 = std::move(send.sent_v2);
    auto reopened = Application::open(receiver_command, password, {}, receive.transport());
    const auto new_epoch = reopened->context_epoch();
    EXPECT_EQ(reopened->store().snapshot()->find_prompt("note-compression")->text, "Replicated prompt");
    {
        Database db(receiver_path, Database::Mode::read_only, password);
        auto row = db.prepare("SELECT content FROM config WHERE name = 'system/prompts/note-compression.md'");
        ASSERT_TRUE(row.step());
        EXPECT_EQ(row.text(0), "Replicated prompt");
    }
    const auto notes = reopened->list_note_items(new_epoch);
    const auto received = std::ranges::find(notes, "Shared note", &NoteItem::name);
    ASSERT_NE(received, notes.end());
    EXPECT_EQ(reopened->get_note(received->id, new_epoch).document, document);
    const auto keys = reopened->list_api_keys(new_epoch);
    EXPECT_NE(std::ranges::find(keys, key.id, &ApiKeyDetail::id), keys.end());
    ASSERT_TRUE(reopened->get_r2_storage(new_epoch));
    EXPECT_EQ(reopened->get_r2_storage(new_epoch)->url, "http://127.0.0.1:1/backups/");
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
        reopened->open_session("lobby", chat.id, new_epoch)));
    const auto received_chat = std::get<SessionSnapshot>(
        reopened->snapshot("lobby", chat.id, new_epoch));
    EXPECT_EQ(received_chat.session_label, "Shared chat");
    ASSERT_EQ(received_chat.transcript.size(), 2u);
    EXPECT_EQ(received_chat.transcript[0].text, "From A");
    EXPECT_EQ(received_chat.transcript[1].status, EntryStatus::complete);
    reopened->request_shutdown();
    ASSERT_TRUE(reopened->join_shutdown(5s));
    EXPECT_EQ(receive.count("DELETE FROM"), 1u);
    EXPECT_EQ(receive.count("INSERT"), 0u);
}

INSTANTIATE_TEST_SUITE_P(VaultProtection, D1HandoffTest, testing::Bool());

TEST(D1R2Test, UploadRefusedWhilePendingAndSucceedsAfterD1Recovers) {
    test::TestWorkspace workspace;
    const auto database = test::import_test_database(workspace.root());
    auto command = make_d1_command(workspace, database);
    MockD1 mock;
    mock.other_tables = {"q2/Test/other"};
    const std::string etag = "HTTP/1.1 200 OK\r\nETag: \"saved\"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    MockHttpServer backup({http_response("text/plain", ""), etag, etag});
    auto application = Application::open(command, {}, {}, mock.transport());
    const auto epoch = application->context_epoch();
    (void)application->save_r2_storage(r2_credentials(backup.port()), epoch);
    (void)application->save_d1_storage(credentials(), epoch);
    ASSERT_TRUE(mock.wait_for_statement("SELECT seq"));
    queue_work(database);
    mock.fail_insert = true;
    backup.start();
    try {
        (void)application->start_database_upload(std::nullopt, epoch);
        FAIL() << "The upload must fail before the snapshot.";
    } catch (const ApplicationError& error) {
        EXPECT_STREQ(error.what(), "Changes are not sent to D1 yet. Try again when D1 can be reached, "
            "or save the D1 credentials again.");
    }
    EXPECT_EQ(application->state(), ApplicationState::running);
    EXPECT_EQ(mock.count("INSERT"), 1u);
    {
        Database db(database, Database::Mode::read_only);
        EXPECT_TRUE(sync_has_pending(db));
    }
    EXPECT_FALSE(backup.wait_for_requests(1, 0ms));

    EXPECT_THROW((void)application->download_database(application->context_epoch()), ApplicationError);
    EXPECT_EQ(application->state(), ApplicationState::running);
    EXPECT_EQ(mock.count("INSERT"), 2u);
    EXPECT_FALSE(backup.wait_for_requests(1, 0ms));

    mock.fail_insert = false;
    const auto upload = application->start_database_upload(
        std::nullopt, application->context_epoch());
    const auto done = std::make_shared<std::promise<void>>();
    auto ready = done->get_future();
    upload->set_ready_callback([done] { done->set_value(); });
    ASSERT_EQ(ready.wait_for(5s), std::future_status::ready);
    const auto result = upload->peek();
    ASSERT_TRUE(result && std::holds_alternative<nlohmann::json>(*result));
    backup.join();
    EXPECT_EQ(backup.requests().size(), 3u);
    EXPECT_EQ(mock.count("INSERT"), 3u);
    {
        Database db(database, Database::Mode::read_only);
        EXPECT_FALSE(sync_has_pending(db));
    }
    application->request_shutdown();
    EXPECT_TRUE(application->join_shutdown(5s));
}

TEST(D1R2Test, DownloadRefusesAnUncollectedNoteFlagBeforeR2) {
    test::TestWorkspace workspace;
    const auto database = test::import_test_database(workspace.root());
    MockHttpServer backup({});
    auto application = Application::open(make_d1_command(workspace, database));
    auto epoch = application->context_epoch();
    (void)application->save_r2_storage(r2_credentials(backup.port()), epoch);
    flag_note(database);

    try {
        (void)application->download_database(epoch);
        FAIL() << "Replacement must refuse the flagged note before downloading.";
    } catch (const ApplicationError& error) {
        EXPECT_STREQ(error.what(), "Changes are not sent to D1 yet. Try again when D1 can be reached, "
            "or save the D1 credentials again.");
    }
    EXPECT_FALSE(backup.wait_for_requests(1, 0ms));
    EXPECT_EQ(application->state(), ApplicationState::running);
    Database db(database, Database::Mode::read_only);
    EXPECT_TRUE(sync_queue_list(db).empty());
    EXPECT_TRUE(sync_has_pending(db));
}

TEST(D1R2Test, ProtectedDownloadChecksReceiptsAndReplaysOnlyNewerInboxWork) {
    test::TestWorkspace workspace;
    const auto database = test::import_test_database(workspace.root());
    const std::string password = "replacement-password";
    const auto command = make_d1_command(workspace, database, "mac", true);
    seed_d1(database);
    protect_workspace_session_database(database, password);

    const auto old_snapshot = workspace.root() / "old.sqlite3";
    copy_workspace_session_database(database, old_snapshot, password, password);
    const std::string received_id(32, 'a');
    const std::string newer_id(32, 'b');
    const auto note_record = [](std::string_view name) {
        return sync_make_record("note.create", {{"path", {name}}, {"item", "folder"}});
    };
    std::string authored_id;
    {
        Database current(database, Database::Mode::read_write, password);
        {
            storage::SqliteTransaction transaction(current);
            ensure_sync_schema(current, true);
            transaction.commit();
        }
        (void)note_create(current, std::nullopt, NoteItemKind::folder, "Authored", "mac");
        authored_id = sync_queue_list(current).front().op_id;
    }
    const auto snapshot = workspace.root() / "covered.sqlite3";
    copy_workspace_session_database(database, snapshot, password, password);
    std::string staged_pending_id;
    {
        Database staged(snapshot, Database::Mode::read_write, password);
        const auto authored = sync_queue_list(staged).front();
        sync_queue_delete(staged, authored.rowid, authored.op_id);
        (void)note_apply_record(staged, nlohmann::json::parse(note_record("Received")), received_id);
        const auto note = note_create(staged, std::nullopt, NoteItemKind::note, "Staged", "mac");
        staged.prepare("UPDATE note_items SET changed=1 WHERE id=?1", note.id).run();
        staged_pending_id = sync_queue_list(staged).front().op_id;
    }
    checkpoint_workspace_session_database(snapshot, password, false);
    const auto definition = file_bytes(command.vault.source);
    MockHttpServer backup({http_response("application/toml", definition),
        http_response("application/vnd.sqlite3", file_bytes(old_snapshot)),
        http_response("application/toml", definition),
        http_response("application/vnd.sqlite3", file_bytes(snapshot))});
    {
        const auto key = r2_credentials(backup.port());
        auto store = WorkspaceConfigStore::open(database, password);
        store->apply_r2_storage_create({.id = "api_key_2", .display_name = key.display_name,
            .url = key.url, .access_key_id = key.access_key_id, .secret_key = *key.secret_key});
    }

    MockD1 mock;
    mock.local_v2_exists = true;
    mock.other_tables = {"q2/Test/other"};
    mock.incoming_v2 = {ready_row(1, received_id, note_record("Received"))};
    auto application = Application::open(command, password, {}, mock.transport());
    ASSERT_EQ(mock.acknowledged_v2, nlohmann::json::array({received_id}));
    ASSERT_EQ(mock.sent_v2.size(), 1u);
    EXPECT_EQ(mock.sent_v2.at(0).at("op_id"), authored_id);
    mock.incoming_v2 = {ready_row(1, authored_id, note_record("Authored")),
        ready_row(2, received_id, note_record("Received")),
        ready_row(3, newer_id, note_record("Newer"))};
    const auto inbox = mock.incoming_v2;
    const auto d1_requests = mock.count("");
    checkpoint_workspace_session_database(database, password, false);
    const auto current_bytes = file_bytes(database);
    auto database_backup = database;
    database_backup += ".bac";
    auto vault_backup = command.vault.source;
    vault_backup += ".bac";
    std::ofstream(database_backup) << "old database backup";
    std::ofstream(vault_backup) << "old definition backup";

    backup.start();
    try {
        (void)application->download_database(application->context_epoch());
        ADD_FAILURE() << "The old snapshot does not cover the active receipts.";
    } catch (const ApplicationError& error) {
        EXPECT_STREQ(error.what(), "The R2 copy is missing changes from this vault. Upload a fresh copy "
            "from a machine that has received all changes, then try again.");
    }
    EXPECT_EQ(application->state(), ApplicationState::running);
    EXPECT_EQ(file_bytes(database), current_bytes);
    EXPECT_EQ(file_bytes(command.vault.source), definition);
    EXPECT_EQ(file_bytes(database_backup), "old database backup");
    EXPECT_EQ(file_bytes(vault_backup), "old definition backup");
    EXPECT_EQ(mock.count(""), d1_requests);
    EXPECT_EQ(mock.incoming_v2, inbox);

    (void)application->download_database(application->context_epoch());
    backup.join();
    ASSERT_EQ(backup.requests().size(), 4u);
    EXPECT_TRUE(backup.requests()[0].starts_with("GET /cha-backups/workspace.sqlite3.toml "));
    EXPECT_EQ(file_bytes(database_backup), current_bytes);
    EXPECT_EQ(file_bytes(vault_backup), definition);
    EXPECT_EQ(mock.acknowledged_v2, nlohmann::json::array({received_id, authored_id, received_id, newer_id}));
    EXPECT_TRUE(mock.incoming_v2.empty());
    Database installed(database, Database::Mode::read_only, password);
    EXPECT_FALSE(sync_has_pending(installed));
    EXPECT_FALSE(sync_seen(installed, staged_pending_id));
    for (const auto& id : {authored_id, received_id, newer_id}) EXPECT_TRUE(sync_seen(installed, id));
    auto notes = installed.prepare("SELECT name FROM note_items WHERE kind='folder' "
        "AND (name LIKE 'Authored%' OR name LIKE 'Received%' OR name LIKE 'Newer%') ORDER BY name");
    std::vector<std::string> applied;
    while (notes.step()) applied.push_back(notes.text(0));
    EXPECT_EQ(applied, (std::vector<std::string>{"Authored", "Newer", "Received"}));
}

TEST(D1VaultTest, CopyCleansDestinationRetainsSource) {
    test::TestWorkspace workspace;
    const auto source = test::import_test_database(workspace.root());
    seed_d1(source);
    const std::string received_id(32, 'a');
    std::string sent_id;
    std::string pending_id;
    {
        Database original(source, Database::Mode::read_write);
        storage::SqliteTransaction transaction(original);
        ensure_sync_schema(original, true);
        sync_record_receipt(original, received_id);
        sync_queue_append(original, "sent record");
        const auto sent = sync_queue_list(original).front();
        sent_id = sent.op_id;
        sync_queue_delete(original, sent.rowid, sent_id);
        sync_queue_append(original, "pending record");
        pending_id = sync_queue_list(original).front().op_id;
        transaction.commit();
    }
    flag_note(source);
    auto command = make_d1_command(workspace, source);
    MockD1 mock;
    mock.fail_insert = true;
    mock.local_v2_exists = true;
    mock.other_tables = {"q2/Test/other"};
    auto application = Application::open(command, {}, {}, mock.transport());
    const auto copy = application->create_vault(
        {.display_name = "Second", .copy_from = "Test", .password = {}},
        application->context_epoch());
    Database copied(copy.data, Database::Mode::read_only);
    EXPECT_FALSE(sync_has_pending(copied));
    EXPECT_TRUE(sync_read_d1(copied));
    EXPECT_TRUE(sync_seen(copied, sent_id));
    EXPECT_TRUE(sync_seen(copied, received_id));
    EXPECT_FALSE(sync_seen(copied, pending_id));
    Database original(source, Database::Mode::read_only);
    EXPECT_TRUE(sync_has_pending(original));
    EXPECT_TRUE(sync_seen(original, pending_id));
    application->request_shutdown();
    EXPECT_TRUE(application->join_shutdown(5s));
}

TEST(D1VaultTest, SyncsSummaryRecordsAndPreservesAudioUntilAnEdit) {
    test::TestWorkspace source;
    test::TestWorkspace receiver;
    const auto source_path = test::import_test_database(source.root());
    const auto receiver_path = receiver.root() / "workspace.sqlite3";
    const auto compressed = note_document_from_text("Compressed");
    const auto l1 = note_document_from_text("Level one");
    const auto l2 = note_document_from_text("Level two");
    const auto edited = note_document_from_text("Edited level one");
    const auto overview = note_document_from_text("Overview");
    const auto updated_overview = note_document_from_text("Updated overview");
    NoteItem folder;
    NoteItem chapter;
    {
        Database db(source_path, Database::Mode::read_write);
        create_event_history_schema(db);
        folder = note_create(db, {}, NoteItemKind::folder, "Book");
        chapter = note_create(db, folder.id, NoteItemKind::note, "Chapter");
        note_save_compressed(db, chapter.id, "Compressed");
        note_commit_level(db, chapter.id, 2, l2, "Saved.");
    }
    seed_d1(source_path);
    copy_workspace_session_database(source_path, receiver_path, {}, {});
    {
        Database db(receiver_path, Database::Mode::read_write);
        auto audio = db.prepare(
            "INSERT INTO note_audio (note_id, ordinal, mime_type, audio) VALUES (?, 0, 'audio/mp4', ?)",
            chapter.id);
        audio.bind_blob(2, "clip");
        audio.run();
    }
    const auto source_command = make_d1_command(source, source_path, "mac");
    const auto receiver_command = make_d1_command(receiver, receiver_path, "linux");

    for (const bool editing : {false, true}) {
        MockD1 send;
        send.local_v2_exists = true;
        send.other_tables = {"q/Test/linux", "q2/Test/linux"};
        {
            auto app = Application::open(source_command, {}, {}, send.transport());
            {
                Database db(source_path, Database::Mode::read_write);
                if (editing) {
                    const auto version = note_get(db, chapter.id, NoteTextMode::l1);
                    (void)note_save(db, version.document_id, edited, "mac");
                    (void)note_commit_folder_summary(db, folder.id, updated_overview, "mac");
                } else {
                    note_commit_level(db, chapter.id, 1, l1, "Saved.", "mac");
                    (void)note_set_summary_excluded(db, chapter.id, true, "mac");
                    (void)note_commit_folder_summary(db, folder.id, overview, "mac");
                }
            }
            app->request_shutdown();
            ASSERT_TRUE(app->join_shutdown(5s));
        }

        if (editing) {
            Database db(receiver_path, Database::Mode::read_write);
            db.prepare("DELETE FROM note_items WHERE parent_id=? AND folder_summary=1", folder.id).run();
            ASSERT_EQ(db.changes(), 1);
            EXPECT_NE(std::ranges::find_if(send.sent_v2, [](const auto& row) {
                const auto record = nlohmann::json::parse(row.at("record").template get<std::string>());
                return record.at("kind") == "note.update" && record.value("folder_summary", false);
            }), send.sent_v2.end());
        }
        MockD1 receive;
        receive.local_table = "q/Test/linux";
        receive.local_v2_exists = true;
        receive.other_tables = {"q/Test/mac", "q2/Test/mac"};
        receive.incoming_v2 = send.sent_v2;
        auto app = Application::open(receiver_command, {}, {}, receive.transport());
        Database db(receiver_path, Database::Mode::read_only);
        const auto items = note_list(db);
        const auto received = std::ranges::find(items, chapter.id, &NoteItem::id);
        ASSERT_NE(received, items.end());
        EXPECT_TRUE(received->summary_excluded);
        EXPECT_EQ(note_get(db, chapter.id, NoteTextMode::l1).document, editing ? edited : l1);
        EXPECT_EQ(note_get(db, chapter.id, NoteTextMode::l2).document, l2);
        EXPECT_EQ(note_get(db, chapter.id, NoteTextMode::compressed).document, compressed);
        EXPECT_EQ(note_audio_info(db, chapter.id)["count"], editing ? 0 : 1);
        if (!editing) EXPECT_EQ(note_audio_clip(db, chapter.id, 0).bytes, "clip");
        const auto summary = std::ranges::find(items, true, &NoteItem::folder_summary);
        ASSERT_NE(summary, items.end());
        EXPECT_EQ(summary->parent_id, folder.id);
        EXPECT_EQ(note_get(db, summary->id).document, editing ? updated_overview : overview);
        Database source_db(source_path, Database::Mode::read_only);
        EXPECT_FALSE(sync_has_pending(source_db));
        for (const auto& row : send.sent_v2) {
            const auto id = row.at("op_id").get<std::string>();
            EXPECT_TRUE(sync_seen(source_db, id));
            EXPECT_TRUE(sync_seen(db, id));
        }
        app->request_shutdown();
        ASSERT_TRUE(app->join_shutdown(5s));
    }
}

TEST(D1AppWiringTest, FirstReceiveAfterSwitchWritesOnlyToTheDestinationVault) {
    for (const std::string password : {"", "second-password"}) {
        SCOPED_TRACE(password.empty() ? "unprotected" : "protected");
        test::TestWorkspace workspace;
        const auto database = test::import_test_database(workspace.root());
        MockD1 mock;
        mock.local_table = "q/Second/mac";
        mock.local_v2_exists = true;
        auto application = Application::open(
            make_d1_command(workspace, database), {}, {}, mock.transport());
        const auto epoch = application->context_epoch();
        (void)application->create_session("lobby", "Old chat", epoch);
        const auto second = application->create_vault(
            {.display_name = "Second", .password = password}, epoch);
        seed_d1(second.data, password);

        test::TestWorkspace peer;
        peer.add_forum("remote", "Remote", "guide");
        const auto peer_database = test::import_test_database(peer.root());
        Database peer_db(peer_database, Database::Mode::read_only);
        nlohmann::json rows = nlohmann::json::array();
        for (const auto& row : read_workspace_config_files(peer_db)) {
            if (row.name.starts_with("forums/remote/"))
                rows.push_back({{"name", row.name}, {"content", row.content}});
        }
        mock.incoming_v2 = {
            ready_row(1, std::string(32, 'a'), sync_make_record(
                "config.edit", {{"set", rows}, {"delete", nlohmann::json::array()}})),
            ready_row(2, std::string(32, 'b'), sync_make_record(
                "chat.entries", {{"chat", {"lobby", "incoming"}}, {"label", "Incoming chat"},
                    {"state", 1}, {"entries", {
                        {{"kind", 0}, {"participant_id", "reader"}, {"display_name", "Reader"},
                            {"addressed_to", "guide"}, {"addressed_to_name", "Guide"},
                            {"text", "Destination vault question"}, {"status", 0}},
                        {{"kind", 1}, {"participant_id", "guide"}, {"display_name", "Guide"},
                            {"text", "Destination vault answer"}, {"status", 0}},
                    }}})),
            ready_row(3, std::string(32, 'c'), sync_make_record(
                "chat.rename", {{"chat", {"remote", "remote-chat"}}, {"label", "Remote chat"}})),
        };
        const auto switched = application->switch_vault("Second", password, epoch);
        ASSERT_EQ(switched.state, ApplicationState::running);
        ASSERT_EQ(application->list_sessions("lobby", switched.context_epoch).size(), 1u);
        ASSERT_EQ(application->list_sessions("remote", switched.context_epoch).size(), 1u);
        const auto transcript =
            application->export_session("lobby", "incoming", switched.context_epoch).markdown;
        EXPECT_TRUE(transcript.starts_with("<!-- CHA session: Incoming chat -->\n"));
        EXPECT_NE(transcript.find("Destination vault question"), std::string::npos);
        EXPECT_NE(transcript.find("Destination vault answer"), std::string::npos);
        EXPECT_EQ(application->export_session("remote", "remote-chat", switched.context_epoch).markdown,
            "<!-- CHA session: Remote chat -->\n");
        Database original(database, Database::Mode::read_only);
        auto sessions = original.prepare("SELECT label FROM sessions");
        ASSERT_TRUE(sessions.step());
        EXPECT_EQ(sessions.text(0), "Old chat");
        EXPECT_FALSE(sessions.step());
    }
}

TEST(D1AppWiringTest, SwitchFlushesOldVaultAndUsesNewVaultPassword) {
    test::TestWorkspace workspace;
    const auto old_path = test::import_test_database(workspace.root());
    MockD1 mock;
    mock.other_tables = {"q2/Test/other", "q2/Second/mac", "q2/Second/other"};
    auto application = Application::open(
        make_d1_command(workspace, old_path), {}, {}, mock.transport());
    const auto epoch = application->context_epoch();
    (void)application->save_d1_storage(credentials(), epoch);
    ASSERT_TRUE(mock.wait_for_statement("SELECT seq"));

    const auto second = application->create_vault(
        {.display_name = "Second", .copy_from = "Test", .password = "second-password"}, epoch);
    queue_work(old_path);
    {
        Database db(second.data, Database::Mode::read_write, "second-password");
        sync_queue_append(db, R"({"kind":"chat.clear"})");
        // Reopen at the switch adds the missing forum row before the first receive.
        db.execute("DELETE FROM forums WHERE forum_id='lobby'");
    }
    {
        const std::lock_guard lock(mock.mutex);
        mock.incoming_v2 = {ready_row(1, std::string(32, 'a'), sync_make_record(
            "chat.rename", {{"chat", {"lobby", "session-from-d1"}}, {"label", "Peer chat"}}))};
    }
    const auto result = application->switch_vault("Second", "second-password", epoch);
    EXPECT_EQ(result.state, ApplicationState::running);
    EXPECT_EQ(mock.count("INSERT"), 2u);
    EXPECT_EQ(mock.count("DELETE FROM"), 1u);
    const auto chats = application->list_sessions("lobby", application->context_epoch());
    ASSERT_EQ(chats.size(), 1u);
    EXPECT_EQ(chats.front().id, "session-from-d1");
    Database old_db(old_path, Database::Mode::read_only);
    EXPECT_FALSE(sync_has_pending(old_db));
    Database new_db(second.data, Database::Mode::read_only, "second-password");
    EXPECT_FALSE(sync_has_pending(new_db));
    EXPECT_TRUE(sync_seen(new_db, std::string(32, 'a')));
    application->request_shutdown();
    EXPECT_TRUE(application->join_shutdown(5s));
}

TEST(D1ShutdownTest, FinalSendFinishesAfterTenSecondShutdownGrace) {
    test::TestWorkspace workspace;
    const auto database = test::import_test_database(workspace.root());
    MockD1 mock;
    mock.insert_delay = 11s;
    mock.other_tables = {"q2/Test/other"};
    auto application = Application::open(
        make_d1_command(workspace, database), {}, {}, mock.transport());
    (void)application->save_d1_storage(credentials(), application->context_epoch());
    ASSERT_TRUE(mock.wait_for_reads());
    flag_note(database);
    application->request_shutdown();
    EXPECT_TRUE(application->join_shutdown(10s));
    application.reset();
    Database db(database, Database::Mode::read_only);
    EXPECT_EQ(mock.count("INSERT"), 1u);
    EXPECT_FALSE(sync_has_pending(db));
}

TEST(D1ShutdownTest, FailedSendStopsAndRetainsTheUnsentQueue) {
    test::TestWorkspace workspace;
    const auto database = test::import_test_database(workspace.root());
    MockD1 mock;
    mock.other_tables = {"q2/Test/other"};
    auto application = Application::open(
        make_d1_command(workspace, database), {}, {}, mock.transport());
    (void)application->save_d1_storage(credentials(), application->context_epoch());
    ASSERT_TRUE(mock.wait_for_statement("SELECT seq"));
    queue_work(database, "First");
    queue_work(database, "Second");
    mock.fail_insert = true;
    application->request_shutdown();
    EXPECT_TRUE(application->join_shutdown(5s));
    application.reset();
    Database db(database, Database::Mode::read_only);
    EXPECT_EQ(mock.count("INSERT"), 1u);
    const auto pending = sync_queue_list(db);
    ASSERT_EQ(pending.size(), 2u);
    for (const auto& row : pending) EXPECT_TRUE(sync_seen(db, row.op_id));
}

} // namespace
} // namespace cha
