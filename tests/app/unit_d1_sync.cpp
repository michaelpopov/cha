#include "app/application.h"
#include "app/application_config.h"
#include "app/d1_client.h"
#include "storage/sync_storage.h"
#include "storage/note_storage.h"
#include "storage/workspace_session_database.h"
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
    std::string machine = "mac") {
    const std::filesystem::path config_directory = workspace.root() / "cha-config";
    std::filesystem::create_directories(config_directory);
    {
        std::ofstream app(config_directory / "app.toml");
        app << "vault = \"Test\"\n"
            << "machine = \"" << machine << "\"\n"
            << "[logging]\nfile = \"runtime.log\"\nlevel = \"off\"\n";
    }
    {
        std::ofstream vault(config_directory / "test.toml");
        vault << "vault_name = \"Test\"\n"
              << "data = " << std::quoted(utf8_path(database)) << "\n";
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
    std::vector<std::string> other_tables{"q/Test/other"};
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> statements;
    std::vector<std::string> create_headers;
    std::size_t reads{};
    nlohmann::json incoming = nlohmann::json::array();

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
                have_local = local_queue_exists;
            }
            changed.notify_all();
            if (sql.starts_with("SELECT name FROM sqlite_master")) {
                auto tables = other_tables;
                if (have_local) tables.push_back("q/Test/mac");
                return {200, tables_ok(std::move(tables)).dump()};
            }
            if (sql.starts_with("SELECT rowid")) {
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

TEST(D1R2Test, UploadRefusedWhilePendingAndVaultReopens) {
    test::TestWorkspace workspace;
    const auto database = test::import_test_database(workspace.root());
    auto command = make_d1_command(workspace, database);
    MockD1 mock;
    auto application = Application::open(command, {}, {}, mock.transport());
    const auto epoch = application->context_epoch();
    (void)application->save_r2_storage(
        {.display_name = "R2", .url = "http://127.0.0.1:1/cha-backups/",
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
    Database db(database, Database::Mode::read_only);
    EXPECT_TRUE(sync_has_pending(db));
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
