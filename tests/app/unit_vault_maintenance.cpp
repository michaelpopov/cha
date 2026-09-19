#include "app/application.h"

#include "support/test_workspace.h"
#include "util/toml_file.h"
#include "web/current_vault.h"
#include "workspace/builtins.h"
#include "workspace/workspace_config_store.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace cha::app {
namespace {

using namespace std::chrono_literals;
using cha::web::ApplicationCommand;
using cha::web::ConfigurationDirectory;

using cha::web::ErrorCode;
using cha::web::VaultCreate;
using cha::web::VaultDefinition;
using cha::web::find_vault;
using cha::web::load_configuration_directory;

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
    const auto created = application->create_session("lobby", "Stored on A");
    EXPECT_EQ(application->current_vault().get().name, "A");

    const auto first = application->context_epoch();
    const auto switched = application->switch_vault("B");
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

    const auto back = application->switch_vault("A");
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

TEST(ApplicationVault, StaleCreateDoesNotRunAgainstTheSwitchedVault) {
    TwoVaults pair;
    auto application = Application::open(pair.command);
    const auto epoch = application->context_epoch();
    (void)application->switch_vault("B");
    try {
        (void)application->create_session("lobby", "Stale", epoch);
        FAIL() << "stale create should fail";
    } catch (const ApplicationError& error) {
        EXPECT_EQ(error.code, ErrorCode::vault_changed);
    }
    const auto created = application->create_session(
        "lobby", "On B", application->context_epoch());
    EXPECT_FALSE(created.id.empty());
}

TEST(ApplicationVault, OverlappingSessionIdsStayOnTheirVault) {
    TwoVaults pair;
    auto application = Application::open(pair.command);
    const auto on_a = application->create_session("lobby", "Same label");
    ASSERT_TRUE(std::holds_alternative<cha::web::OpenSessionSuccess>(
        application->open_session("lobby", on_a.id)));
    const auto epoch_a = application->context_epoch();
    (void)application->switch_vault("B");
    const auto on_b = application->create_session("lobby", "Same label");
    ASSERT_TRUE(std::holds_alternative<cha::web::OpenSessionSuccess>(
        application->open_session("lobby", on_b.id)));

    const auto stale_open = application->open_session(
        "lobby", on_a.id, epoch_a);
    ASSERT_TRUE(std::holds_alternative<ErrorCode>(stale_open));
    EXPECT_EQ(std::get<ErrorCode>(stale_open), ErrorCode::vault_changed);

    (void)application->switch_vault("A");
    const auto boot = application->bootstrap();
    bool saw_a = false;
    for (const auto& recent : boot.presentation.recent_sessions) {
        if (recent.session_id == on_a.id) saw_a = true;
    }
    EXPECT_TRUE(saw_a);
}

TEST(ApplicationVault, CreateListAndSameVaultSwitch) {
    TwoVaults pair;
    auto application = Application::open(pair.command);
    const auto created = application->create_vault(VaultCreate{
        .display_name = "Copied",
        .copy_from = "A",
        .password = {},
    });
    EXPECT_EQ(created.name, "Copied");
    const auto snapshot = application->vault_snapshot();
    EXPECT_GE(snapshot.vaults.size(), 3U);
    const auto epoch = application->context_epoch();
    const auto same = application->switch_vault("a");
    EXPECT_EQ(same.context_epoch, epoch);
    EXPECT_EQ(application->current_vault().get().name, "A");
}

TEST(ApplicationVault, SaveFileRejectsStaleEpochAndReplacesAtomically) {
    TwoVaults pair;
    auto application = Application::open(pair.command);
    const auto destination = pair.workspace_a.root() / "saved.txt";
    std::ofstream(destination) << "keep-me";
    const auto epoch = application->context_epoch();
    (void)application->switch_vault("B");
    EXPECT_THROW(
        application->save_file(epoch, destination, "new"),
        ApplicationError);
    std::ifstream existing(destination);
    std::string kept(
        (std::istreambuf_iterator<char>(existing)),
        std::istreambuf_iterator<char>());
    EXPECT_EQ(kept, "keep-me");

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
    const auto switched = application->switch_vault("B");
    ASSERT_EQ(notices.size(), 1U);
    EXPECT_EQ(notices.front().first, switched.context_epoch);
    EXPECT_NE(notices.front().first, first);
    EXPECT_EQ(notices.front().second, ApplicationState::running);

    const auto count = notices.size();
    application->set_context_changed(record);
    EXPECT_EQ(notices.size(), count);

    const auto same = application->switch_vault("B");
    EXPECT_EQ(same.context_epoch, switched.context_epoch);
    EXPECT_EQ(notices.size(), count);

    const auto back = application->switch_vault("A");
    ASSERT_EQ(notices.size(), count + 1);
    EXPECT_EQ(notices.back().first, back.context_epoch);
    EXPECT_GT(back.context_epoch, switched.context_epoch);
    EXPECT_EQ(notices.back().second, ApplicationState::running);
}

TEST(ApplicationVault, StaleVaultMutationFailsVaultChanged) {
    TwoVaults pair;
    auto application = Application::open(pair.command);
    const auto epoch = application->context_epoch();
    (void)application->switch_vault("B");
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
    auto application = Application::open(pair.command);
    const auto caps = application->capabilities();
    EXPECT_FALSE(caps.can_modify);
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
