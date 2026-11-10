#include "app/application.h"
#include "app/application_config.h"
#include "app/d1_client.h"
#include "storage/sync_storage.h"
#include "storage/note_storage.h"
#include "storage/workspace_session_database.h"
#include "support/mock_http_server.h"
#include "support/test_workspace.h"
#include "util/path_name.h"

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
    std::string local_table{"q/Test/mac"};
    std::vector<std::string> other_tables{"q/Test/other"};
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> statements;
    std::vector<std::string> create_headers;
    std::size_t reads{};
    nlohmann::json incoming = nlohmann::json::array();
    nlohmann::json sent = nlohmann::json::array();

    D1Client::Transport transport() {
        return [this](const std::string&, const std::string& body,
                   const std::vector<std::string>& headers) -> std::pair<long, std::string> {
            const auto batch = nlohmann::json::parse(body).at("batch");
            const auto sql = batch.at(0).at("sql").get<std::string>();
            bool have_local;
            nlohmann::json rows;
            {
                const std::lock_guard lock(mutex);
                statements.push_back(sql);
                if (sql.starts_with("CREATE TABLE") && !fail_create) {
                    local_queue_exists = true;
                    create_headers = headers;
                }
                if (sql.starts_with("SELECT rowid")) {
                    ++reads;
                    rows = incoming;
                }
                if (sql.starts_with("DELETE FROM")) incoming.clear();
                if (sql.starts_with("INSERT") && !fail_insert) {
                    const auto& params = batch.at(0).at("params");
                    sent.push_back({
                        {"rowid", sent.size() + 1},
                        {"piece", std::stoi(params.at(0).get<std::string>())},
                        {"pieces", std::stoi(params.at(1).get<std::string>())},
                        {"record", params.at(2)},
                    });
                }
                have_local = local_queue_exists;
            }
            changed.notify_all();
            if (sql.starts_with("SELECT name FROM sqlite_master")) {
                auto tables = other_tables;
                if (have_local) tables.push_back(local_table);
                return {200, tables_ok(std::move(tables)).dump()};
            }
            if (sql.starts_with("SELECT rowid")) {
                if (!have_local) {
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

    bool wait_for_reads(std::size_t count = 1) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 5s, [&] { return reads >= count; });
    }

    std::size_t count(std::string_view prefix) {
        const std::lock_guard lock(mutex);
        return std::ranges::count_if(statements,
            [&](const auto& sql) { return sql.starts_with(prefix); });
    }
};

SaveD1StorageRequest credentials() {
    return {.display_name = "D1", .account_id = "a", .database_id = "d", .api_token = "t"};
}

void seed_d1(const std::filesystem::path& path) {
    Database db(path, Database::Mode::read_write);
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
    auto application = Application::open(command, {}, {}, mock.transport());
    const auto epoch = application->context_epoch();
    const auto saved = application->save_d1_storage(credentials(), epoch);
    EXPECT_EQ(saved.display_name, "D1");
    EXPECT_TRUE(saved.has_api_token);
    ASSERT_TRUE(mock.wait_for_reads());

    auto unchanged = credentials();
    unchanged.api_token.reset();
    const auto saved_again = application->save_d1_storage(unchanged, epoch);
    EXPECT_EQ(saved_again.database_id, saved.database_id);
    EXPECT_TRUE(saved_again.has_api_token);
    ASSERT_TRUE(mock.wait_for_reads(2));
    EXPECT_EQ(mock.count("CREATE TABLE"), 2u);
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
    auto application = Application::open(command, {}, {}, mock.transport());
    const auto epoch = application->context_epoch();
    (void)application->save_d1_storage(credentials(), epoch);
    ASSERT_TRUE(mock.wait_for_reads());
    queue_work(database);
    flag_note(database);
    mock.fail_drop = true;
    application->disconnect_d1_storage(epoch);
    EXPECT_FALSE(application->get_d1_storage(epoch).has_value());
    EXPECT_EQ(mock.count("DROP TABLE"), 1u);
    Database db(database, Database::Mode::read_only);
    EXPECT_FALSE(sync_has_pending(db));
    EXPECT_FALSE(sync_read_d1(db));
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
    ASSERT_TRUE(mock.wait_for_reads());
    queue_work(database);
    flag_note(database);
    force_next_workspace_config_fault(WorkspaceConfigFault::sqlite_commit);
    EXPECT_THROW(application->disconnect_d1_storage(epoch), ApplicationError);
    EXPECT_TRUE(application->get_d1_storage(epoch));
    EXPECT_EQ(mock.count("DROP TABLE"), 0u);
    Database db(database, Database::Mode::read_only);
    EXPECT_EQ(sync_queue_list(db).size(), 1u);
    auto flags = db.prepare("SELECT COUNT(*) FROM note_items WHERE changed=1");
    ASSERT_TRUE(flags.step());
    EXPECT_EQ(flags.integer(0), 1);
}

TEST(D1AppWiringTest, DaemonStartupRecreatesQueueAndRetriesPendingWork) {
    test::TestWorkspace workspace;
    const auto database = test::import_test_database(workspace.root());
    auto command = make_d1_command(workspace, database);
    command.chaweb_host = true;
    seed_d1(database);
    queue_work(database);
    MockD1 mock;
    mock.local_queue_exists = false;
    auto application = Application::open(command, {}, {}, mock.transport());
    EXPECT_EQ(application->state(), ApplicationState::running);
    EXPECT_EQ(mock.count("CREATE TABLE"), 1u);
    EXPECT_EQ(mock.count("INSERT"), 1u);
    Database db(database, Database::Mode::read_only);
    EXPECT_FALSE(sync_has_pending(db));
    application->request_shutdown();
    EXPECT_TRUE(application->join_shutdown(5s));
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
    mock.incoming = {
        {{"rowid", 1}, {"piece", 1}, {"pieces", 1}, {"record", sync_make_record(
            "config.edit", {{"set", set_rows}, {"delete", nlohmann::json::array()}})}},
        {{"rowid", 2}, {"piece", 1}, {"pieces", 1}, {"record", sync_make_record(
            "chat.rename", {{"chat", {"remote", "session-from-d1"}}, {"label", "Shared chat"}})}},
    };
    auto application = Application::open(
        make_d1_command(workspace, database), {}, {}, mock.transport());
    const auto chats = application->list_sessions("remote", application->context_epoch());
    ASSERT_EQ(chats.size(), 1u);
    EXPECT_EQ(chats.front().id, "session-from-d1");
    EXPECT_EQ(mock.count("DELETE FROM"), 1u);
    Database db(database, Database::Mode::read_only);
    EXPECT_FALSE(sync_has_pending(db));
}

TEST(D1AppWiringTest, IncomingChatIsWrittenToTheSessionMirror) {
    test::TestWorkspace workspace;
    const auto database = test::import_test_database(workspace.root());
    seed_d1(database);
    auto command = make_d1_command(workspace, database);
    const auto mirror = workspace.root() / "mirror";
    command.vault.mirror = mirror;
    command.vaults.front() = command.vault;
    MockD1 mock;
    mock.incoming = {
        {{"rowid", 1}, {"piece", 1}, {"pieces", 1}, {"record", sync_make_record(
            "chat.rename", {{"chat", {"lobby", "session-from-d1"}}, {"label", "Incoming chat"}})}},
    };
    auto application = Application::open(command, {}, {}, mock.transport());
    bool found = false;
    for (const auto& item : std::filesystem::recursive_directory_iterator(mirror)) {
        if (item.path().filename().string().find("Incoming chat") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
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
    receive.other_tables = {"q/Test/mac"};
    {
        auto application = Application::open(receiver_command, password, {}, receive.transport());
        (void)application->save_d1_storage(credentials(), application->context_epoch());
        ASSERT_TRUE(receive.wait_for_reads());
        application->request_shutdown();
        ASSERT_TRUE(application->join_shutdown(5s));
    }

    MockD1 send;
    send.other_tables = {"q/Test/linux"};
    auto application = Application::open(source_command, password, {}, send.transport());
    const auto epoch = application->context_epoch();
    (void)application->save_d1_storage(credentials(), epoch);
    ASSERT_TRUE(send.wait_for_reads());
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
    EXPECT_EQ(send.count("INSERT"), 5u);  // Note create and update, two config edits, one turn.
    {
        Database db(source_path, Database::Mode::read_only, password);
        EXPECT_FALSE(sync_has_pending(db));
    }

    receive.incoming = std::move(send.sent);
    auto reopened = Application::open(receiver_command, password, {}, receive.transport());
    const auto new_epoch = reopened->context_epoch();
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
    const std::string etag = "HTTP/1.1 200 OK\r\nETag: \"saved\"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    MockHttpServer backup({http_response("text/plain", ""), etag, etag});
    auto application = Application::open(command, {}, {}, mock.transport());
    const auto epoch = application->context_epoch();
    (void)application->save_r2_storage(
        {.display_name = "R2", .url = "http://127.0.0.1:" + std::to_string(backup.port()) + "/cha-backups/",
         .access_key_id = "k", .secret_key = "s"}, epoch);
    (void)application->save_d1_storage(credentials(), epoch);
    ASSERT_TRUE(mock.wait_for_reads());
    queue_work(database);
    mock.fail_insert = true;
    try {
        (void)application->start_database_upload(std::nullopt, epoch);
        FAIL() << "The upload must fail before the snapshot.";
    } catch (const ApplicationError& error) {
        EXPECT_STREQ(error.what(), "Changes are not sent to D1 yet. Try again when D1 can be reached.");
    }
    EXPECT_EQ(application->state(), ApplicationState::running);
    EXPECT_EQ(mock.count("INSERT"), 1u);
    {
        Database db(database, Database::Mode::read_only);
        EXPECT_TRUE(sync_has_pending(db));
    }
    EXPECT_FALSE(backup.wait_for_requests(1, 0ms));

    mock.fail_insert = false;
    backup.start();
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
    EXPECT_EQ(mock.count("INSERT"), 2u);
    {
        Database db(database, Database::Mode::read_only);
        EXPECT_FALSE(sync_has_pending(db));
    }
    application->request_shutdown();
    EXPECT_TRUE(application->join_shutdown(5s));
}

TEST(D1VaultTest, CopyCleansDestinationRetainsSource) {
    test::TestWorkspace workspace;
    const auto source = test::import_test_database(workspace.root());
    seed_d1(source);
    queue_work(source);
    flag_note(source);
    auto command = make_d1_command(workspace, source);
    MockD1 mock;
    mock.fail_insert = true;
    auto application = Application::open(command, {}, {}, mock.transport());
    const auto copy = application->create_vault(
        {.display_name = "Second", .copy_from = "Test", .password = {}},
        application->context_epoch());
    Database copied(copy.data, Database::Mode::read_only);
    EXPECT_FALSE(sync_has_pending(copied));
    EXPECT_TRUE(sync_read_d1(copied));
    Database original(source, Database::Mode::read_only);
    EXPECT_TRUE(sync_has_pending(original));
    application->request_shutdown();
    EXPECT_TRUE(application->join_shutdown(5s));
}

TEST(D1AppWiringTest, SwitchFlushesOldVaultAndUsesNewVaultPassword) {
    test::TestWorkspace workspace;
    const auto old_path = test::import_test_database(workspace.root());
    MockD1 mock;
    mock.other_tables.insert(mock.other_tables.end(), {"q/Second/mac", "q/Second/other"});
    auto application = Application::open(
        make_d1_command(workspace, old_path), {}, {}, mock.transport());
    const auto epoch = application->context_epoch();
    (void)application->save_d1_storage(credentials(), epoch);
    ASSERT_TRUE(mock.wait_for_reads());

    const auto second = application->create_vault(
        {.display_name = "Second", .copy_from = "Test", .password = "second-password"}, epoch);
    queue_work(old_path);
    {
        Database db(second.data, Database::Mode::read_write, "second-password");
        sync_queue_append(db, R"({"kind":"chat.clear"})");
    }
    const auto result = application->switch_vault("Second", "second-password", epoch);
    EXPECT_EQ(result.state, ApplicationState::running);
    EXPECT_EQ(mock.count("INSERT"), 2u);
    Database old_db(old_path, Database::Mode::read_only);
    EXPECT_FALSE(sync_has_pending(old_db));
    Database new_db(second.data, Database::Mode::read_only, "second-password");
    EXPECT_FALSE(sync_has_pending(new_db));
    application->request_shutdown();
    EXPECT_TRUE(application->join_shutdown(5s));
}

TEST(D1ShutdownTest, FinalSendFinishesAfterTenSecondShutdownGrace) {
    test::TestWorkspace workspace;
    const auto database = test::import_test_database(workspace.root());
    MockD1 mock;
    mock.insert_delay = 11s;
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
    auto application = Application::open(
        make_d1_command(workspace, database), {}, {}, mock.transport());
    (void)application->save_d1_storage(credentials(), application->context_epoch());
    ASSERT_TRUE(mock.wait_for_reads());
    queue_work(database, "First");
    queue_work(database, "Second");
    mock.fail_insert = true;
    application->request_shutdown();
    EXPECT_TRUE(application->join_shutdown(5s));
    application.reset();
    Database db(database, Database::Mode::read_only);
    EXPECT_EQ(mock.count("INSERT"), 1u);
    EXPECT_EQ(sync_queue_list(db).size(), 2u);
}

} // namespace
} // namespace cha
