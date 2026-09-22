#include "app/application.h"

#include "storage/session_repository.h"
#include "support/test_workspace.h"
#include "util/toml_file.h"
#include "app/current_vault.h"
#include "workspace/builtins.h"
#include "workspace/workspace_config_store.h"

#include <gtest/gtest.h>

#include <chrono>
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
        if (recent.session_label == "Stored on A") saw_stored_on_a = true;
    }
    EXPECT_TRUE(saw_stored_on_a);
    EXPECT_EQ(created.id, created.id);
    const auto table = read_toml_file(
        pair.command.config_directory / "app.toml", "config file");
    EXPECT_EQ(table["vault"].value<std::string>(), "A");
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
    expect_stale([&] { (void)application->upload_database(stale_epoch); });
    expect_stale([&] { (void)application->download_database(stale_epoch); });
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
    const auto snapshot = application->vault_snapshot();
    EXPECT_GE(snapshot.vaults.size(), 3U);
    const auto epoch = application->context_epoch();
    const auto same = application->switch_vault("a", {}, application->context_epoch());
    EXPECT_EQ(same.context_epoch, epoch);
    EXPECT_EQ(application->current_vault().get().name, "A");
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
