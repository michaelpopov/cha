#include "app/application.h"
#include "app/vault_operations.h"

#include "storage/session_repository.h"
#include "storage/sqlite_storage.h"
#include "support/test_transcript.h"
#include "support/mock_http_server.h"
#include "support/test_workspace.h"
#include "util/logging.h"
#include "util/toml_file.h"
#include "app/current_vault.h"
#include "workspace/builtins.h"
#include "workspace/workspace_config_store.h"

#include <gtest/gtest.h>

#include <chrono>
#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cha::app {
namespace {

using namespace std::chrono_literals;

ApplicationCommand load_command(
    const std::filesystem::path& config_directory,
    const std::vector<std::pair<test::TestWorkspace*, std::filesystem::path>>& vaults,
    std::string_view active) {
    std::filesystem::create_directories(config_directory);
    {
        std::ofstream app(config_directory / "app.toml");
        app << "vault = " << std::quoted(std::string(active)) << "\n"
            << "[logging]\nfile = \"runtime.log\"\nlevel = \"off\"\n";
    }
    std::size_t index = 0;
    for (const auto& [workspace, database] : vaults) {
        (void)workspace;
        const char letter = static_cast<char>('A' + static_cast<char>(index));
        std::ofstream vault(
            config_directory / (std::string(1, letter) + ".toml"));
        vault << "vault_name = \"" << letter << "\"\n"
              << "data = " << std::quoted(database.string()) << "\n";
        ++index;
    }
    const ConfigurationDirectory loaded = load_configuration_directory(
        config_directory);
    const VaultDefinition* const selected =
        find_vault(loaded.vaults, loaded.startup_vault);
    return {
        .config_directory = loaded.directory,
        .vaults = loaded.vaults,
        .vault = *selected,
        .log_file = loaded.log_file,
        .log_level = loaded.log_level,
        .warnings = loaded.warnings,
    };
}

struct TwoVaults {
    TwoVaults() {
        workspace_a.add_persona("alpha", "Alpha");
        workspace_b.add_persona("beta", "Beta");
        database_a = workspace_a.root() / "a.sqlite3";
        database_b = workspace_b.root() / "b.sqlite3";
        (void)test::import_test_database(workspace_a.root(), database_a);
        (void)test::import_test_database(workspace_b.root(), database_b);
        command = load_command(
            workspace_a.root() / "cha-config",
            {{&workspace_a, database_a}, {&workspace_b, database_b}},
            "A");
    }

    test::TestWorkspace workspace_a;
    test::TestWorkspace workspace_b;
    std::filesystem::path database_a;
    std::filesystem::path database_b;
    ApplicationCommand command;
};

std::string file_contents(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void set_parent(TwoVaults& pair, std::string name = "B") {
    pair.command.vault.parent = name;
    pair.command.vaults.front().parent = std::move(name);
    write_toml_file(pair.command.vault.source, vault::vault_definition_table(pair.command.vault));
}

void set_r2(Application& application, int port) {
    (void)application.save_r2_storage({
        .display_name = "Backups",
        .url = "http://127.0.0.1:" + std::to_string(port) + "/bucket/",
        .access_key_id = "test-access-key",
        .secret_key = std::string("test-secret-key"),
    }, application.context_epoch());
}

TEST(ApplicationVault, PicturesFollowTheCurrentVaultAndRejectStaleEpochs) {
    TwoVaults pair;
    std::ofstream(pair.workspace_a.root() / "characters/guide/PICTURE.png") << "A";
    std::ofstream(pair.workspace_b.root() / "characters/guide/PICTURE.png") << "B";
    (void)test::import_test_database(pair.workspace_a.root(), pair.database_a);
    (void)test::import_test_database(pair.workspace_b.root(), pair.database_b);
    auto application = Application::open(pair.command);
    const auto first_epoch = application->context_epoch();
    const auto first = application->get_character_picture("guide", first_epoch);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->content_base64, "QQ==");
    const auto switched = application->switch_vault("B", {}, first_epoch);
    const auto second = application->get_character_picture("guide", switched.context_epoch);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->content_base64, "Qg==");
    try {
        (void)application->get_character_picture("guide", first_epoch);
        FAIL() << "Expected stale epoch rejection";
    } catch (const ApplicationError& error) {
        EXPECT_EQ(error.code, ErrorCode::vault_changed);
    }
    const auto back = application->switch_vault("A", {}, switched.context_epoch);
    const auto restored = application->get_character_picture("guide", back.context_epoch);
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->content_base64, "QQ==");
}

TEST(ApplicationVault, UploadResumesTheVaultBeforeTransferAndCanFinishAfterAVaultSwitch) {
    TwoVaults pair;
    const std::string response = "HTTP/1.1 200 OK\r\nETag: \"uploaded-etag\"\r\n"
        "Content-Length: 0\r\nConnection: close\r\n\r\n";
    MockHttpServer server({response, response});
    server.pause_before_response(1);
    auto application = Application::open(pair.command);
    set_r2(*application, server.port());
    const auto epoch = application->context_epoch();
    std::atomic<std::uint64_t> notified_epoch{0};
    application->set_context_changed([&](std::uint64_t next, ApplicationState) {
        notified_epoch.store(next);
    });
    server.start();
    auto upload = std::async(std::launch::async, [&] {
        return application->upload_database({}, epoch);
    });
    EXPECT_TRUE(server.wait_for_requests(1, 2s));
    EXPECT_EQ(application->state(), ApplicationState::running);
    EXPECT_GT(application->context_epoch(), epoch);
    EXPECT_EQ(notified_epoch.load(), application->context_epoch());
    auto use_vault = std::async(std::launch::async, [&] {
        const auto session = application->create_session(
            "lobby", "Created during upload", application->context_epoch());
        EXPECT_FALSE(session.id.empty());
        return application->switch_vault("B", {}, application->context_epoch());
    });
    const auto usable = use_vault.wait_for(2s);
    server.resume_responses();
    EXPECT_EQ(usable, std::future_status::ready);
    EXPECT_EQ(use_vault.get().state, ApplicationState::running);
    EXPECT_EQ(upload.get().etag, "uploaded-etag");
    server.join();
    EXPECT_EQ(application->current_vault().get().name, "B");
    EXPECT_FALSE(application->current_vault().get().r2_etag);
    const auto registry = application->vault_snapshot();
    EXPECT_EQ(registry.vaults.front().r2_etag, "uploaded-etag");
    EXPECT_EQ(read_toml_file(pair.command.vault.source, "vault")["r2_etag"].value<std::string>(),
        "uploaded-etag");
    const auto copy_path = pair.workspace_a.root() / "uploaded.sqlite3";
    std::ofstream(copy_path, std::ios::binary) << request_body(server.requests().front());
    storage::SqliteDatabase copy(copy_path, storage::SqliteDatabase::Mode::read_only);
    auto session = copy.prepare("SELECT count(*) FROM sessions WHERE label = 'Created during upload'");
    ASSERT_TRUE(session.step());
    EXPECT_EQ(session.integer(0), 0);
}

TEST(ApplicationVault, FailedCompanionUploadKeepsTheVaultUsableAndRemembersTheDatabaseEtag) {
    TwoVaults pair;
    MockHttpServer server({
        "HTTP/1.1 200 OK\r\nETag: \"uploaded-etag\"\r\n"
            "Content-Length: 0\r\nConnection: close\r\n\r\n",
        "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n"
            "Connection: close\r\n\r\n",
    });
    auto application = Application::open(pair.command);
    set_r2(*application, server.port());
    server.start();
    EXPECT_THROW((void)application->upload_database({}, application->context_epoch()), R2HttpStatusError);
    server.join();
    EXPECT_EQ(application->state(), ApplicationState::running);
    EXPECT_EQ(application->current_vault().get().r2_etag, "uploaded-etag");
    EXPECT_EQ(read_toml_file(pair.command.vault.source, "vault")["r2_etag"].value<std::string>(),
        "uploaded-etag");
    EXPECT_NO_THROW((void)application->create_session(
        "lobby", "After upload failure", application->context_epoch()));
    for (const auto& directory : {pair.workspace_a.root(), pair.command.config_directory}) {
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            EXPECT_EQ(entry.path().filename().string().find(".upload."), std::string::npos);
        }
    }
#ifndef _WIN32
    EXPECT_EQ(std::filesystem::status(pair.command.vault.source).permissions()
        & std::filesystem::perms::all,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
#endif
}

TEST(ApplicationVault, ExportReplacesPreviousContentsAndCanBeRepeated) {
    TwoVaults pair;
    const auto destination = pair.workspace_a.root() / "export";
    pair.command.vault.modify = destination;
    pair.command.vaults.front().modify = destination;
    auto application = Application::open(pair.command);
    std::filesystem::create_directories(destination / "system/providers/old-provider");
    std::filesystem::create_directories(destination / "characters/old-character");
    std::filesystem::create_directories(destination / "forums/old-forum/members/old-character");
    std::ofstream(destination / "system/providers/old-provider/config.toml") << "invalid old config";
    std::ofstream(destination / ".old-export") << "stale file";

    const auto exported = application->export_configuration(application->context_epoch());
    EXPECT_GT(exported.file_count, 0U);
    EXPECT_FALSE(std::filesystem::exists(destination / "characters/old-character"));
    EXPECT_FALSE(std::filesystem::exists(destination / "system/providers/old-provider"));
    EXPECT_FALSE(std::filesystem::exists(destination / ".old-export"));
    EXPECT_TRUE(Workspace::load(destination).find_persona("alpha"));
    EXPECT_EQ(application->export_configuration(application->context_epoch()).file_count,
        exported.file_count);
}

TEST(ApplicationVault, ExportRemovesSymlinksWithoutRemovingTheirTargets) {
    test::TestWorkspace workspace;
    const auto destination = workspace.root() / "export";
    const auto outside = workspace.root() / "outside";
    std::filesystem::create_directories(destination / "system/providers/old-provider");
    std::filesystem::create_directories(outside);
    std::ofstream(outside / "keep.txt") << "outside the export";
    std::error_code error;
    std::filesystem::create_directory_symlink(outside, destination / "link", error);
    if (error) {
        GTEST_SKIP() << "symlinks are not supported: " << error.message();
    }

    EXPECT_NO_THROW(vault::clear_existing_export(destination));
    EXPECT_FALSE(std::filesystem::exists(destination));
    EXPECT_TRUE(std::filesystem::exists(outside / "keep.txt"));
}

TEST(ApplicationVault, SwitchAwayAndBackRestoresStoredSessions) {
    TwoVaults pair;
    auto application = Application::open(pair.command);
    const auto created = application->create_session(
        "lobby", "Stored on A", application->context_epoch());
    EXPECT_EQ(application->current_vault().get().name, "A");

    const auto first = application->context_epoch();
    const auto switched = application->switch_vault("B", {}, application->context_epoch());
    EXPECT_EQ(application->current_vault().get().name, "B");
    EXPECT_GT(switched.context_epoch, first);
    EXPECT_EQ(switched.state, ApplicationState::running);
    EXPECT_EQ(application->check_context(first), ErrorCode::vault_changed);

    const auto boot_b = application->bootstrap();
    bool saw_stored_on_b = false;
    for (const auto& recent : boot_b.presentation.recent_sessions) {
        if (recent.session_label == "Stored on A") saw_stored_on_b = true;
    }
    EXPECT_FALSE(saw_stored_on_b);

    const auto back = application->switch_vault("A", {}, application->context_epoch());
    EXPECT_EQ(application->current_vault().get().name, "A");
    EXPECT_GT(back.context_epoch, switched.context_epoch);
    const auto boot_a = application->bootstrap();
    bool saw_stored_on_a = false;
    for (const auto& recent : boot_a.presentation.recent_sessions) {
        if (recent.session_label == "Stored on A") {
            saw_stored_on_a = true;
            EXPECT_EQ(recent.session_id, created.id);
        }
    }
    EXPECT_TRUE(saw_stored_on_a);
    const auto table = read_toml_file(
        pair.command.config_directory / "app.toml", "config file");
    EXPECT_EQ(table["vault"].value<std::string>(), "A");
}

TEST(ApplicationVault, SwitchBackRecoversPendingSessions) {
    TwoVaults pair;
    auto application = Application::open(pair.command);
    const auto unused = application->create_session("lobby", "", application->context_epoch());
    const auto rejected = application->create_session("lobby", "", application->context_epoch());
    const auto used = application->create_session("lobby", "", application->context_epoch());
    const auto named = application->create_session("lobby", "Named empty session", application->context_epoch());
    // Seed the durable states left by a rejected prompt and interrupted naming.
    for (const auto& id : {rejected.id, used.id}) {
        storage::SqliteDatabase database(pair.database_a, storage::SqliteDatabase::Mode::read_write);
        auto row = database.prepare("SELECT session_key FROM sessions WHERE session_id = ?1", id);
        ASSERT_TRUE(row.step());
        const auto key = row.integer(0);
        ASSERT_FALSE(row.step());
        SessionJournal journal(pair.database_a, key);
        journal.retain();
        if (id == used.id) {
            journal.record_entry(test::human_entry(1, {"human", "You"}, {"-", "Notes"}, "Keep me"));
        }
    }
    const auto before = application->bootstrap().presentation.recent_sessions;
    EXPECT_TRUE(std::ranges::none_of(before, [&](const auto& row) { return row.session_id == used.id; }));

    ASSERT_EQ(application->switch_vault("B", {}, application->context_epoch()).state, ApplicationState::running);
    ASSERT_EQ(application->switch_vault("A", {}, application->context_epoch()).state, ApplicationState::running);

    const auto listed = application->list_sessions("lobby", application->context_epoch());
    EXPECT_TRUE(std::ranges::none_of(listed, [&](const auto& row) {
        return row.id == unused.id || row.id == rejected.id;
    }));
    EXPECT_TRUE(std::ranges::any_of(listed, [&](const auto& row) { return row.id == named.id; }));
    const auto recent = application->bootstrap().presentation.recent_sessions;
    EXPECT_TRUE(std::ranges::any_of(recent, [&](const auto& row) {
        return row.session_id == used.id && row.session_label == "New session";
    }));
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
        application->open_session("lobby", used.id, application->context_epoch())));
    const auto snapshot = std::get<SessionSnapshot>(
        application->snapshot("lobby", used.id, application->context_epoch()));
    EXPECT_FALSE(snapshot.recent_pending);
    ASSERT_EQ(snapshot.transcript.size(), 1u);
    EXPECT_EQ(snapshot.transcript.front().text, "Keep me");
}

TEST(ApplicationVault, SwitchMigratesLegacyKeysWithoutHoldingTheStoreLock) {
    TwoVaults pair;
    auto application = Application::open(pair.command);
    std::ofstream(pair.command.config_directory / "api-keys.json")
        << R"({"version":1,"keys":{"api_key_1":{"display_name":"Legacy","value":"test-key"}}})";

    const auto switched = application->switch_vault("B", {}, application->context_epoch());

    EXPECT_EQ(switched.state, ApplicationState::running);
    const auto keys = application->list_api_keys(application->context_epoch());
    ASSERT_EQ(keys.size(), 1U);
    EXPECT_EQ(keys.front().display_name, "Legacy");
    EXPECT_TRUE(keys.front().has_value);
    EXPECT_EQ(application->switch_vault("A", {}, application->context_epoch()).state,
        ApplicationState::running);
}

TEST(ApplicationVault, ZeroAndStaleEpochsCannotCreateInTheSwitchedVault) {
    TwoVaults pair;
    auto application = Application::open(pair.command);
    const auto epoch = application->context_epoch();
    (void)application->switch_vault("B", {}, application->context_epoch());
    const auto before = application->list_sessions(
        "lobby", application->context_epoch()).size();
    for (const auto invalid_epoch : {std::uint64_t{0}, epoch}) {
        SCOPED_TRACE(invalid_epoch);
        EXPECT_EQ(application->check_context(invalid_epoch), ErrorCode::vault_changed);
        try {
            (void)application->create_session("lobby", "Rejected", invalid_epoch);
            FAIL() << "create with an invalid epoch should fail";
        } catch (const ApplicationError& error) {
            EXPECT_EQ(error.code, ErrorCode::vault_changed);
        }
    }
    EXPECT_EQ(application->list_sessions(
        "lobby", application->context_epoch()).size(), before);
    const auto created = application->create_session(
        "lobby", "On B", application->context_epoch());
    EXPECT_FALSE(created.id.empty());
}

TEST(ApplicationVault, MaintenanceRejectsAStaleVaultEpochUnderTheLifecycleLock) {
    TwoVaults pair;
    auto application = Application::open(pair.command);
    const auto stale_epoch = application->context_epoch();
    (void)application->switch_vault("B", {}, stale_epoch);

    auto expect_stale = [&](auto operation) {
        try {
            operation();
            FAIL() << "maintenance with a stale epoch should fail";
        } catch (const ApplicationError& error) {
            EXPECT_EQ(error.code, ErrorCode::vault_changed);
        }
    };
    expect_stale([&] {
        (void)application->upload_database("expected-etag", stale_epoch);
    });
    expect_stale([&] { (void)application->check_database_upload(stale_epoch); });
    expect_stale([&] { (void)application->download_database(stale_epoch); });
    expect_stale([&] { (void)application->merge_parent_vault({}, stale_epoch); });
    expect_stale([&] { (void)application->import_configuration(stale_epoch); });
    expect_stale([&] { (void)application->export_configuration(stale_epoch); });
    EXPECT_EQ(application->current_vault().get().name, "B");
}

TEST(ApplicationVault, OverlappingSessionIdsStayOnTheirVault) {
    TwoVaults pair;
    auto application = Application::open(pair.command);
    const auto on_a = application->create_session(
        "lobby", "Same label", application->context_epoch());
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
        application->open_session("lobby", on_a.id, application->context_epoch())));
    const auto epoch_a = application->context_epoch();
    (void)application->switch_vault("B", {}, application->context_epoch());
    const auto on_b = application->create_session(
        "lobby", "Same label", application->context_epoch());
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
        application->open_session("lobby", on_b.id, application->context_epoch())));

    for (const auto invalid_epoch : {std::uint64_t{0}, epoch_a}) {
        SCOPED_TRACE(invalid_epoch);
        const auto rejected_open = application->open_session(
            "lobby", on_b.id, invalid_epoch);
        ASSERT_TRUE(std::holds_alternative<ErrorCode>(rejected_open));
        EXPECT_EQ(std::get<ErrorCode>(rejected_open), ErrorCode::vault_changed);

        const auto rejected_submit = application->submit_async(
            "lobby", on_b.id, StopCommand{}, invalid_epoch);
        ASSERT_TRUE(std::holds_alternative<ErrorCode>(rejected_submit));
        EXPECT_EQ(std::get<ErrorCode>(rejected_submit), ErrorCode::vault_changed);

        EXPECT_EQ(application->delete_session("lobby", on_b.id, invalid_epoch),
            ErrorCode::vault_changed);
        application->close_session("lobby", on_b.id, invalid_epoch);
        EXPECT_TRUE(std::holds_alternative<SessionSnapshot>(
            application->snapshot("lobby", on_b.id, application->context_epoch())));
    }

    (void)application->switch_vault("A", {}, application->context_epoch());
    const auto boot = application->bootstrap();
    bool saw_a = false;
    for (const auto& recent : boot.presentation.recent_sessions) {
        if (recent.session_id == on_a.id) saw_a = true;
    }
    EXPECT_TRUE(saw_a);
}

TEST(ApplicationVault, MergeUsesMaintenanceAndRetiresLiveSessions) {
    TwoVaults pair;
    auto application = Application::open(pair.command);
    const auto created = application->create_session(
        "lobby", "Before merge", application->context_epoch());
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
        application->open_session("lobby", created.id, application->context_epoch())));
    const auto old_epoch = application->context_epoch();

    const auto merged = application->merge_vault("B", {}, application->context_epoch());

    EXPECT_EQ(merged.state, ApplicationState::running);
    EXPECT_GT(merged.context_epoch, old_epoch);
    EXPECT_EQ(application->current_vault().get().name, "A");
    EXPECT_EQ(application->get_persona("beta", application->context_epoch()).summary.display_name,
        "Beta");
    EXPECT_EQ(application->check_context(old_epoch), ErrorCode::vault_changed);
    const auto snapshot = application->snapshot(
        "lobby", created.id, merged.context_epoch);
    ASSERT_TRUE(std::holds_alternative<ErrorCode>(snapshot));
    EXPECT_EQ(std::get<ErrorCode>(snapshot), ErrorCode::session_not_live);
}

TEST(ApplicationVault, ParentMergeDownloadsFreshConfigurationAndPreservesChildIdentityAndSessions) {
    TwoVaults pair;
    set_parent(pair);
    test::TestWorkspace remote;
    remote.add_persona("remote", "Remote");
    const auto remote_database = test::import_test_database(remote.root());
    MockHttpServer server({
        http_response("application/toml", "vault_name = \"B\"\ndata = \"b.sqlite3\"\n"),
        http_response("application/vnd.sqlite3", file_contents(remote_database)),
    });
    auto application = Application::open(pair.command);
    set_r2(*application, server.port());
    const auto created = application->create_session("lobby", "Child note", application->context_epoch());
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
        application->open_session("lobby", created.id, application->context_epoch())));
    const auto old_epoch = application->context_epoch();
    const auto child_definition = file_contents(pair.command.vault.source);
    server.start();
    const auto result = application->merge_parent_vault({}, old_epoch);
    server.join();

    EXPECT_GT(result.context_epoch, old_epoch);
    EXPECT_EQ(result.state, ApplicationState::running);
    EXPECT_EQ(application->current_vault().get().name, "A");
    EXPECT_EQ(application->current_vault().get().parent, "B");
    EXPECT_EQ(file_contents(pair.command.vault.source), child_definition);
    EXPECT_EQ(application->get_persona("remote", result.context_epoch).summary.display_name, "Remote");
    EXPECT_EQ(application->get_persona("alpha", result.context_epoch).summary.display_name, "Alpha");
    const auto sessions = application->list_sessions("lobby", result.context_epoch);
    EXPECT_TRUE(std::ranges::any_of(sessions, [&](const auto& session) { return session.id == created.id; }));
    const auto snapshot = application->snapshot("lobby", created.id, result.context_epoch);
    EXPECT_EQ(std::get<ErrorCode>(snapshot), ErrorCode::session_not_live);
    EXPECT_EQ(application->check_context(old_epoch), ErrorCode::vault_changed);
    EXPECT_EQ(file_contents(pair.database_b), file_contents(remote_database));
    ASSERT_EQ(server.requests().size(), 2U);
    EXPECT_TRUE(server.requests()[0].starts_with("GET /bucket/b.sqlite3.toml HTTP/1.1"));
    EXPECT_TRUE(server.requests()[1].starts_with("GET /bucket/b.sqlite3 HTTP/1.1"));
}

TEST(ApplicationVault, ParentDownloadFailuresLeaveChildAndLiveSessionsUnchanged) {
    for (const std::string failure : {"missing", "invalid", "wrong-name"}) {
        SCOPED_TRACE(failure);
        TwoVaults pair;
        set_parent(pair);
        const auto parent_before = file_contents(pair.database_b);
        const auto parent_definition = file_contents(pair.command.vaults.back().source);
        const auto response = failure == "missing"
            ? "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
            : http_response("application/toml", "vault_name = \""
                + std::string(failure == "wrong-name" ? "Other" : "B") + "\"\ndata = \"b.sqlite3\"\n");
        MockHttpServer server(failure == "missing" ? std::vector<std::string>{response}
            : std::vector<std::string>{response, http_response("application/vnd.sqlite3",
                failure == "invalid" ? "invalid database" : parent_before)});
        auto application = Application::open(pair.command);
        set_r2(*application, server.port());
        const auto created = application->create_session("lobby", "Child note", application->context_epoch());
        ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
            application->open_session("lobby", created.id, application->context_epoch())));
        const auto epoch = application->context_epoch();
        server.start();
        EXPECT_THROW((void)application->merge_parent_vault({}, epoch), std::runtime_error);
        server.join();
        EXPECT_EQ(application->context_epoch(), epoch);
        EXPECT_EQ(application->state(), ApplicationState::running);
        EXPECT_TRUE(std::holds_alternative<SessionSnapshot>(application->snapshot("lobby", created.id, epoch)));
        EXPECT_EQ(application->get_persona("alpha", epoch).summary.display_name, "Alpha");
        EXPECT_EQ(file_contents(pair.database_b), parent_before);
        EXPECT_EQ(file_contents(pair.command.vaults.back().source), parent_definition);
    }
}

TEST(ApplicationVault, ParentMergeDownloadsAProtectedParentWithItsPassword) {
    TwoVaults pair;
    set_parent(pair);
    auto application = Application::open(pair.command);
    (void)application->update_vault("B", {.display_name = "B", .password = "secret"},
        application->context_epoch());
    MockHttpServer server({
        http_response("application/toml", "vault_name = \"B\"\ndata = \"b.sqlite3\"\nprotected = true\n"),
        http_response("application/vnd.sqlite3", file_contents(pair.database_b)),
    });
    set_r2(*application, server.port());
    server.start();
    const auto result = application->merge_parent_vault("secret", application->context_epoch());
    server.join();
    EXPECT_EQ(result.state, ApplicationState::running);
    EXPECT_EQ(application->get_persona("beta", result.context_epoch).summary.display_name, "Beta");
    EXPECT_EQ(application->current_vault().get().parent, "B");
    EXPECT_FALSE(application->current_vault().get().password_protected);
}

TEST(ApplicationVault, CancellingAStalledParentDownloadPreservesTheCurrentContext) {
    TwoVaults pair;
    set_parent(pair);
    MockHttpServer server({http_response("application/toml",
        "vault_name = \"B\"\ndata = \"b.sqlite3\"\n")});
    server.pause_before_response(1);
    auto application = Application::open(pair.command);
    set_r2(*application, server.port());
    const auto epoch = application->context_epoch();
    const auto before = file_contents(pair.database_b);
    std::atomic<bool> cancelled{false};
    server.start();
    auto download = std::async(std::launch::async, [&] {
        return application->merge_parent_vault({}, epoch, [&] { return cancelled.load(); });
    });
    EXPECT_TRUE(server.wait_for_requests(1, 2s));
    cancelled = true;
    const auto finished = download.wait_for(2s);
    server.resume_responses();
    EXPECT_EQ(finished, std::future_status::ready);
    EXPECT_THROW((void)download.get(), std::runtime_error);
    server.join();
    EXPECT_EQ(application->context_epoch(), epoch);
    EXPECT_EQ(application->state(), ApplicationState::running);
    EXPECT_EQ(file_contents(pair.database_b), before);
}

TEST(ApplicationVault, ParentMergeRejectsMissingParentSelfParentAndMissingR2WithoutStoppingSessions) {
    for (const std::string parent : {"", "Missing", "A", "B"}) {
        SCOPED_TRACE(parent);
        TwoVaults pair;
        if (!parent.empty()) set_parent(pair, parent);
        auto application = Application::open(pair.command);
        const auto epoch = application->context_epoch();
        const auto created = application->create_session("lobby", "Child note", epoch);
        ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
            application->open_session("lobby", created.id, epoch)));
        if (parent == "Missing") {
            EXPECT_THROW((void)application->merge_parent_vault({}, epoch), UnknownVaultError);
        } else if (parent == "A") {
            EXPECT_THROW((void)application->merge_parent_vault({}, epoch), std::invalid_argument);
        } else {
            try {
                (void)application->merge_parent_vault({}, epoch);
                FAIL() << "Expected a parent merge configuration error";
            } catch (const ApplicationError& error) {
                EXPECT_EQ(error.code, ErrorCode::invalid_argument);
                EXPECT_EQ(std::string(error.what()), parent.empty()
                    ? "The current vault has no parent" : "The current vault has no R2 key");
            }
        }
        EXPECT_EQ(application->context_epoch(), epoch);
        EXPECT_EQ(application->state(), ApplicationState::running);
        EXPECT_TRUE(std::holds_alternative<SessionSnapshot>(application->snapshot("lobby", created.id, epoch)));
    }
}

TEST(ApplicationVault, MergeSynchronizationFailureMarksApplicationUnavailable) {
    TwoVaults pair;
    auto application = Application::open(pair.command);
    const auto old_epoch = application->context_epoch();
    force_next_forum_sync_failure();

    EXPECT_THROW(
        (void)application->merge_vault("B", {}, application->context_epoch()),
        WorkspaceRestartRequiredError);

    EXPECT_EQ(application->state(), ApplicationState::unavailable);
    EXPECT_GT(application->context_epoch(), old_epoch);
    EXPECT_EQ(
        application->check_context(application->context_epoch()),
        ErrorCode::application_unavailable);
}

TEST(ApplicationVault, ProtectionSetupFailureRestoresRunningContext) {
    TwoVaults pair;
    auto application = Application::open(pair.command);
    const auto old_epoch = application->context_epoch();
    bool resumed = false;
    application->set_resource_hooks({
        .pause = [](bool) { throw std::runtime_error("pause failed"); },
        .resume = [&] { resumed = true; },
    });

    EXPECT_THROW(
        (void)application->update_vault(
            "A", {.display_name = "A", .password = "secret"}, application->context_epoch()),
        std::runtime_error);

    EXPECT_TRUE(resumed);
    EXPECT_EQ(application->state(), ApplicationState::running);
    EXPECT_GT(application->context_epoch(), old_epoch);
    EXPECT_FALSE(application->vault_snapshot().active.password_protected);
    EXPECT_FALSE(application->create_session(
        "lobby", "After recovery", application->context_epoch()).id.empty());

    application->set_resource_hooks({});
    const auto protected_vault = application->update_vault(
        "A", {.display_name = "A", .password = "secret"},
        application->context_epoch());
    EXPECT_TRUE(protected_vault.password_protected);
    EXPECT_EQ(application->state(), ApplicationState::running);
    EXPECT_EQ(application->active_password(), "secret");
}

TEST(ApplicationVault, CreateListAndSameVaultSwitch) {
    TwoVaults pair;
    auto application = Application::open(pair.command);
    const auto created = application->create_vault(VaultCreate{
        .display_name = "Copied",
        .copy_from = "A",
        .password = {},
    }, application->context_epoch());
    EXPECT_EQ(created.name, "Copied");
    EXPECT_EQ(created.parent, "A");
    EXPECT_EQ(read_toml_file(created.source, "vault")["parent"].value<std::string>(), "A");
    const auto snapshot = application->vault_snapshot();
    EXPECT_GE(snapshot.vaults.size(), 3U);
    const auto epoch = application->context_epoch();
    const auto same = application->switch_vault("a", {}, application->context_epoch());
    EXPECT_EQ(same.context_epoch, epoch);
    EXPECT_EQ(application->current_vault().get().name, "A");
}

TEST(ApplicationVault, NewVaultHasNoParentAndRenamingPreservesExistingParentReferences) {
    TwoVaults pair;
    auto application = Application::open(pair.command);
    const auto fresh = application->create_vault({.display_name = "Fresh"}, application->context_epoch());
    EXPECT_FALSE(fresh.parent);
    EXPECT_FALSE(read_toml_file(fresh.source, "vault").contains("parent"));
    const auto child = application->create_vault({.display_name = "Child", .copy_from = "B"},
        application->context_epoch());
    (void)application->update_vault("Child", {.display_name = "Renamed child"}, application->context_epoch());
    EXPECT_EQ(read_toml_file(child.source, "vault")["parent"].value<std::string>(), "B");
    (void)application->update_vault("B", {.display_name = "Renamed base"}, application->context_epoch());
    EXPECT_EQ(read_toml_file(child.source, "vault")["parent"].value<std::string>(), "B");
    const auto registry = application->vault_snapshot();
    EXPECT_EQ(find_vault(registry.vaults, "Renamed child")->parent, "B");
}

TEST(ApplicationVault, SaveFileRejectsStaleEpochAndReplacesAtomically) {
    TwoVaults pair;
    auto application = Application::open(pair.command);
    const auto destination = pair.workspace_a.root() / "saved.txt";
    std::ofstream(destination) << "keep-me";
    const auto epoch = application->context_epoch();
    (void)application->switch_vault("B", {}, application->context_epoch());
    EXPECT_THROW(
        application->save_file(epoch, destination, "new"),
        ApplicationError);
    std::ifstream existing(destination);
    std::string kept(
        (std::istreambuf_iterator<char>(existing)),
        std::istreambuf_iterator<char>());
    EXPECT_EQ(kept, "keep-me");
    existing.close();

    application->save_file(
        application->context_epoch(), destination, "replaced");
    std::ifstream updated(destination);
    std::string body(
        (std::istreambuf_iterator<char>(updated)),
        std::istreambuf_iterator<char>());
    EXPECT_EQ(body, "replaced");
}

TEST(ApplicationVault, ContextChangedCoalescesAndReportsTheNewEpoch) {
    TwoVaults pair;
    auto application = Application::open(pair.command);
    std::vector<std::pair<std::uint64_t, ApplicationState>> notices;
    auto record = [&](std::uint64_t epoch, ApplicationState state) {
        notices.emplace_back(epoch, state);
    };
    application->set_context_changed(record);
    EXPECT_TRUE(notices.empty());

    const auto first = application->context_epoch();
    const auto switched = application->switch_vault("B", {}, application->context_epoch());
    ASSERT_EQ(notices.size(), 1U);
    EXPECT_EQ(notices.front().first, switched.context_epoch);
    EXPECT_NE(notices.front().first, first);
    EXPECT_EQ(notices.front().second, ApplicationState::running);

    const auto count = notices.size();
    application->set_context_changed(record);
    EXPECT_EQ(notices.size(), count);

    const auto same = application->switch_vault("B", {}, application->context_epoch());
    EXPECT_EQ(same.context_epoch, switched.context_epoch);
    EXPECT_EQ(notices.size(), count);

    const auto back = application->switch_vault("A", {}, application->context_epoch());
    ASSERT_EQ(notices.size(), count + 1);
    EXPECT_EQ(notices.back().first, back.context_epoch);
    EXPECT_GT(back.context_epoch, switched.context_epoch);
    EXPECT_EQ(notices.back().second, ApplicationState::running);
}

TEST(ApplicationVault, ContextCheckDoesNotWaitForMaintenanceLifecycleLock) {
    TwoVaults pair;
    pair.command.vault.modify = pair.workspace_a.root() / "modify";
    for (auto& vault : pair.command.vaults) {
        if (vault.name == "A") vault.modify = pair.command.vault.modify;
    }
    auto application = Application::open(pair.command);
    const auto old_epoch = application->context_epoch();
    std::mutex mutex;
    std::condition_variable changed;
    bool paused = false;
    bool release = false;
    application->set_resource_hooks({
        .pause = [&](bool) {
            std::unique_lock lock(mutex);
            paused = true;
            changed.notify_all();
            changed.wait(lock, [&] { return release; });
        },
        .resume = [] {},
    });

    auto switching = std::async(std::launch::async, [&] {
        return application->switch_vault("B", {}, old_epoch);
    });
    bool reached_pause = false;
    {
        std::unique_lock lock(mutex);
        reached_pause = changed.wait_for(lock, 2s, [&] { return paused; });
    }
    auto checked = std::async(std::launch::async, [&] {
        return application->check_context(old_epoch);
    });
    auto bootstrapped = std::async(std::launch::async, [&] {
        return application->bootstrap();
    });
    auto capabilities = std::async(std::launch::async, [&] {
        return application->capabilities();
    });
    const auto check_status = checked.wait_for(1s);
    const auto bootstrap_status = bootstrapped.wait_for(1s);
    const auto capabilities_status = capabilities.wait_for(1s);
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();

    EXPECT_TRUE(reached_pause);
    EXPECT_EQ(check_status, std::future_status::ready);
    EXPECT_EQ(bootstrap_status, std::future_status::ready);
    EXPECT_EQ(capabilities_status, std::future_status::ready);
    EXPECT_EQ(checked.get(), ErrorCode::vault_changed);
    EXPECT_FALSE(capabilities.get().can_modify);
    const auto maintenance = bootstrapped.get();
    EXPECT_EQ(maintenance.state, ApplicationState::maintenance);
    EXPECT_EQ(maintenance.context_epoch, old_epoch);
    EXPECT_EQ(maintenance.presentation.vault_name, "A");
    EXPECT_FALSE(maintenance.capabilities.can_modify);
    EXPECT_EQ(switching.get().state, ApplicationState::running);
}

TEST(ApplicationVault, SubscriptionCompletionCarriesItsEndpoint) {
    TwoVaults pair;
    (void)test::import_test_database(pair.workspace_a.root(), pair.database_a);
    pair.command.test_shutdown_grace_ms = 1500;
    auto application = Application::open(pair.command);
    const auto epoch = application->context_epoch();
    const auto created = application->create_session("lobby", "Callback race", epoch);
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
        application->open_session("lobby", created.id, epoch)));

    const auto result = application->subscribe(
        "lobby",
        created.id,
        SubscribeCommand{"view", epoch, "sub"},
        epoch);
    const auto* subscribed = std::get_if<SubscribeResult>(&result);
    ASSERT_NE(subscribed, nullptr);
    EXPECT_TRUE(subscribed->session);

    auto switching = std::async(std::launch::async, [&] {
        return application->switch_vault("B", {}, epoch);
    });
    EXPECT_NO_THROW(EXPECT_EQ(switching.get().state, ApplicationState::running));
    application->request_shutdown();
    EXPECT_TRUE(application->join_shutdown(2s));
}

TEST(ApplicationVault, MaintenanceTimeoutRecoversWhileTheRuntimeRemainsBlocked) {
    TwoVaults pair;
    pair.workspace_a.add_character("writer", "Writer");
    const auto member = pair.workspace_a.root() / "forums/lobby/members/writer";
    std::filesystem::create_directories(member);
    std::ofstream(member / "character.toml") << "# member\n";
    (void)test::import_test_database(pair.workspace_a.root(), pair.database_a);
    pair.command.test_shutdown_grace_ms = 50;
    auto application = Application::open(pair.command);
    const auto epoch = application->context_epoch();
    const auto created = application->create_session("lobby", "Blocked runtime", epoch);
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
        application->open_session("lobby", created.id, epoch)));

    // Persistence of this accepted command blocks the runtime ahead of the
    // maintenance reservation, without taking the application lifecycle lock.
    std::optional<WorkspaceConfigStore::MaintenanceGuard> edit_lock(
        application->store().reserve_maintenance());
    const auto changing = application->submit_async(
        "lobby", created.id, SetDefaultCharacterCommand{"writer"}, epoch);
    ASSERT_TRUE(std::holds_alternative<std::shared_ptr<CommandReply>>(changing));
    auto switching = std::async(std::launch::async, [&] {
        try {
            (void)application->switch_vault("B", {}, epoch);
            return false;
        } catch (const std::runtime_error&) {
            return true;
        }
    });
    const auto status = switching.wait_for(500ms);
    const auto recovered_state = application->state();
    const auto recovered_epoch = application->context_epoch();
    edit_lock.reset();

    EXPECT_EQ(status, std::future_status::ready);
    EXPECT_TRUE(switching.get());
    EXPECT_EQ(recovered_state, ApplicationState::running);
    EXPECT_GT(recovered_epoch, epoch);
    EXPECT_EQ(application->current_vault().get().name, "A");
    EXPECT_EQ(application->check_context(epoch), ErrorCode::vault_changed);
    const auto& reply = std::get<std::shared_ptr<CommandReply>>(changing);
    ASSERT_TRUE(reply->wait_for(2s));
    EXPECT_TRUE(std::holds_alternative<SessionSnapshot>(
        application->snapshot("lobby", created.id, application->context_epoch())));
    EXPECT_EQ(application->switch_vault("B", {}, application->context_epoch()).state,
        ApplicationState::running);
}

TEST(ApplicationVault, MaintenanceCompletionDoesNotUndoShutdown) {
    TwoVaults pair;
    auto application = Application::open(pair.command);
    std::mutex mutex;
    std::condition_variable changed;
    int pauses = 0;
    int resumes = 0;
    bool release = false;
    application->set_resource_hooks({
        .pause = [&](bool) {
            std::unique_lock lock(mutex);
            ++pauses;
            changed.notify_all();
            if (pauses == 1) {
                changed.wait(lock, [&] { return release; });
            }
        },
        .resume = [&] {
            std::lock_guard lock(mutex);
            ++resumes;
        },
    });

    auto switching = std::async(std::launch::async, [&] {
        return application->switch_vault("B", {}, application->context_epoch());
    });
    bool reached_pause = false;
    {
        std::unique_lock lock(mutex);
        reached_pause = changed.wait_for(lock, 2s, [&] { return pauses == 1; });
    }
    EXPECT_TRUE(reached_pause);
    application->request_shutdown();
    auto maintenance = std::async(std::launch::async, [&] {
        application->wait_for_maintenance();
    });
    EXPECT_EQ(maintenance.wait_for(50ms), std::future_status::timeout);
    const auto started = std::chrono::steady_clock::now();
    EXPECT_FALSE(application->join_shutdown(50ms));
    EXPECT_LT(std::chrono::steady_clock::now() - started, 500ms);
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();

    EXPECT_EQ(maintenance.wait_for(2s), std::future_status::ready);
    maintenance.get();
    const auto switched = switching.get();
    EXPECT_EQ(switched.state, ApplicationState::unavailable);
    EXPECT_EQ(application->state(), ApplicationState::unavailable);
    EXPECT_FALSE(application->running());
    {
        std::lock_guard lock(mutex);
        EXPECT_EQ(resumes, 0);
        EXPECT_GE(pauses, 2);
    }
    EXPECT_TRUE(application->join_shutdown(2s));
}

TEST(ApplicationVault, StaleVaultMutationFailsVaultChanged) {
    TwoVaults pair;
    auto application = Application::open(pair.command);
    const auto epoch = application->context_epoch();
    (void)application->switch_vault("B", {}, application->context_epoch());
    try {
        (void)application->create_vault(
            VaultCreate{.display_name = "Stale"}, epoch);
        FAIL() << "stale vault create should fail";
    } catch (const ApplicationError& error) {
        EXPECT_EQ(error.code, ErrorCode::vault_changed);
    }
}

TEST(ApplicationVault, CapabilitiesFollowModifyAndR2OnTheActiveVault) {
    TwoVaults pair;
    pair.command.vault.modify = pair.workspace_a.root() / "modify";
    for (auto& vault : pair.command.vaults) {
        if (vault.name == "A") vault.modify = pair.command.vault.modify;
    }
    auto application = Application::open(pair.command);
    const auto caps = application->capabilities();
    EXPECT_TRUE(caps.can_modify);
    EXPECT_TRUE(application->bootstrap().capabilities.can_modify);

    (void)application->switch_vault("B", {}, application->context_epoch());
    EXPECT_FALSE(application->capabilities().can_modify);
    EXPECT_FALSE(application->bootstrap().capabilities.can_modify);

    (void)application->switch_vault("A", {}, application->context_epoch());
    EXPECT_TRUE(application->capabilities().can_modify);
}

TEST(ApplicationVault, SwitchPreservesFileLoggingAndDropsUndo) {
    shutdown_diagnostic_logging();
    const auto directory = std::filesystem::temp_directory_path()
        / ("cha_vault_maintenance_log_"
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(directory);
    initialize_diagnostic_logging(directory / "cha.log", "off");
    struct Guard {
        std::filesystem::path directory;
        ~Guard() {
            shutdown_diagnostic_logging();
            std::error_code ignored;
            std::filesystem::remove_all(directory, ignored);
        }
    } guard{directory};

    TwoVaults pair;
    auto application = Application::open(pair.command);
    set_diagnostic_log_level("debug");
    log_info("vault switch marker");
    const std::string path = "characters/guide/character.toml";
    const auto before = application->store().read_config(std::vector<std::string>{path});
    std::string renamed = before.files.at(0).content;
    const auto name = renamed.find("Guide");
    ASSERT_NE(name, std::string::npos);
    renamed.replace(name, std::string("Guide").size(), "Guide renamed");
    const auto applied = application->store().apply_config(
        application->store().config_revision(),
        std::vector<WorkspaceConfigChange>{{
            .path = path,
            .operation = WorkspaceConfigOperation::replace,
            .content = renamed,
        }});
    ASSERT_TRUE(applied.committed);
    EXPECT_EQ(
        application->switch_vault("B", {}, application->context_epoch()).state,
        ApplicationState::running);
    EXPECT_EQ(diagnostic_log_level(), "debug");
    std::ifstream log(directory / "cha.log");
    const std::string contents{std::istreambuf_iterator<char>(log), {}};
    EXPECT_NE(contents.find("vault switch marker"), std::string::npos);
    EXPECT_EQ(
        application->switch_vault("A", {}, application->context_epoch()).state,
        ApplicationState::running);
    const auto after = application->store().read_config(std::vector<std::string>{path});
    EXPECT_EQ(after.files.at(0).content, renamed);
    const auto undo = application->store().undo_config(application->store().config_revision());
    EXPECT_FALSE(undo.committed);
    EXPECT_EQ(undo.error, WorkspaceConfigApplyError::unavailable_undo);
}

TEST(ApplicationVault, BootstrapDuringUnavailableDoesNotReadClosedHandles) {
    TwoVaults pair;
    auto application = Application::open(pair.command);
    application->mark_unusable();
    const auto boot = application->bootstrap();
    EXPECT_EQ(boot.state, ApplicationState::unavailable);
    EXPECT_EQ(boot.presentation.vault_name, "A");
    EXPECT_TRUE(boot.presentation.forums.empty());
}

} // namespace
} // namespace cha::app
