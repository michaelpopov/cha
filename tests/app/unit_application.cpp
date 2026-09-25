#include "app/application.h"
#include "providers/api_key_store.h"

#include "support/test_workspace.h"
#include "support/mock_http_server.h"
#include "runtime/live_session.h"
#include "workspace/builtins.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <string>
#include <thread>
#include <variant>

namespace cha::app {
namespace {

using namespace std::chrono_literals;

ApplicationCommand make_command(
    const test::TestWorkspace& workspace,
    const std::filesystem::path& database) {
    const std::filesystem::path config_directory =
        workspace.root() / "cha-config";
    std::filesystem::create_directories(config_directory);
    {
        std::ofstream app(config_directory / "app.toml");
        app << "vault = \"Test\"\n"
            << "[logging]\nfile = \"runtime.log\"\nlevel = \"off\"\n";
    }
    {
        std::ofstream vault(config_directory / "test.toml");
        vault << "vault_name = \"Test\"\n"
              << "data = " << std::quoted(database.string()) << "\n";
    }
    const ConfigurationDirectory loaded = load_configuration_directory(
        config_directory);
    const VaultDefinition* const vault =
        find_vault(loaded.vaults, loaded.startup_vault);
    std::vector<VaultDefinition> vaults = loaded.vaults;
    VaultDefinition selected = *vault;
    vaults.front() = selected;
    return {
        .config_directory = loaded.directory,
        .vaults = std::move(vaults),
        .vault = std::move(selected),
        .log_file = loaded.log_file,
        .log_level = loaded.log_level,
        .warnings = loaded.warnings,
    };
}

std::shared_ptr<const app::SessionOutputItem> next_output(
    LiveSession& session,
    std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto item = session.take_output()) return item;
        std::this_thread::sleep_for(1ms);
    }
    return {};
}

TEST(RuntimeSettings, DefaultsHavePositiveBatchSizes) {
    const RuntimeSettings settings;
    EXPECT_GT(settings.command_batch_size, 0U);
    EXPECT_GT(settings.event_batch_size, 0U);
    EXPECT_TRUE(settings.monotonic_event_sequence);
}

TEST(Application, StartsWithoutAListenerAndBootstraps) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    auto application = Application::open(make_command(workspace, database));
    EXPECT_TRUE(application->running());
    EXPECT_EQ(application->state(), ApplicationState::running);
    EXPECT_GE(application->context_epoch(), 1U);
    EXPECT_EQ(application->check_context(0), ErrorCode::vault_changed);

    const ApplicationBootstrap boot = application->bootstrap();
    EXPECT_EQ(boot.state, ApplicationState::running);
    EXPECT_EQ(boot.presentation.initial_forum_id, entrance_id);
    EXPECT_EQ(boot.presentation.initial_session_id, welcome_id);
    EXPECT_EQ(boot.presentation.entrance_forum_id, entrance_id);
    EXPECT_FALSE(boot.presentation.forums.empty());
    EXPECT_EQ(boot.presentation.vault_name, "Test");
}

TEST(Application, IndependentOwnersKeepWorkspaceSessionsAndCredentialsIsolated) {
    test::TestWorkspace first_fixture;
    first_fixture.write_character_config(
        "display_name = \"First guide\"\nprovider = \"test\"\n");
    auto first = Application::open(make_command(
        first_fixture, test::import_test_database(first_fixture.root())));
    const auto first_epoch = first->context_epoch();
    const auto original = first->store().snapshot();
    const auto first_session = first->create_session("lobby", "First session", first_epoch);
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
        first->open_session("lobby", first_session.id, first_epoch)));

    test::TestWorkspace second_fixture;
    second_fixture.write_character_config(
        "display_name = \"Second guide\"\nprovider = \"test\"\n");
    auto second = Application::open(make_command(
        second_fixture, test::import_test_database(second_fixture.root())));
    const auto second_epoch = second->context_epoch();
    const auto second_session = second->create_session("lobby", "Second session", second_epoch);
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
        second->open_session("lobby", second_session.id, second_epoch)));

    EXPECT_EQ(first->get_character("guide", first_epoch).summary.display_name, "First guide");
    EXPECT_EQ(second->get_character("guide", second_epoch).summary.display_name, "Second guide");
    const auto first_snapshot = first->snapshot("lobby", first_session.id, first_epoch);
    const auto second_snapshot = second->snapshot("lobby", second_session.id, second_epoch);
    ASSERT_TRUE(std::holds_alternative<SessionSnapshot>(first_snapshot));
    ASSERT_TRUE(std::holds_alternative<SessionSnapshot>(second_snapshot));
    EXPECT_EQ(std::get<SessionSnapshot>(first_snapshot).characters.front().display_name,
        "First guide");
    EXPECT_EQ(std::get<SessionSnapshot>(second_snapshot).characters.front().display_name,
        "Second guide");

    const auto first_key = first->create_api_key(
        {.display_name = "First key", .value = "first-secret"}, first_epoch);
    const auto second_key = second->create_api_key(
        {.display_name = "Second key", .value = "second-secret"}, second_epoch);
    ASSERT_EQ(first_key.id, second_key.id);
    EXPECT_EQ(first->api_keys().value(first_key.id), "first-secret");
    EXPECT_EQ(second->api_keys().value(second_key.id), "second-secret");

    const auto edited = first->update_character_definition(
        "guide", {.display_name = "Edited guide", .character_markdown = "New instructions"},
        first_epoch);
    EXPECT_EQ(edited.summary.display_name, "Edited guide");
    EXPECT_EQ(edited.character_markdown, "New instructions");
    EXPECT_EQ(original->find_character("guide")->character.display_name, "First guide");
    EXPECT_EQ(second->get_character("guide", second_epoch).summary.display_name, "Second guide");

    second.reset();
    EXPECT_EQ(first->get_character("guide", first_epoch).summary.display_name, "Edited guide");
    EXPECT_FALSE(first->create_session(
        "lobby", "After other owner closed", first_epoch).id.empty());
    first.reset();
    EXPECT_EQ(original->find_character("guide")->character.display_name, "First guide");
}

TEST(Application, CreateOpenSubmitStopSnapshotCloseAndShutdown) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    auto application = Application::open(make_command(workspace, database));
    const auto epoch = application->context_epoch();

    const auto created = application->create_session("lobby", "Headless", epoch);
    const auto opened = application->open_session("lobby", created.id, epoch);
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(opened));
    EXPECT_EQ(application->selected_session()->session_id, created.id);
    const auto boot = application->bootstrap();
    EXPECT_EQ(boot.presentation.initial_forum_id, "lobby");
    EXPECT_EQ(boot.presentation.initial_session_id, created.id);
    EXPECT_EQ(boot.presentation.entrance_forum_id, entrance_id);

    const auto submitted = application->submit(
        "lobby", created.id, RawCommand{"Hello"}, epoch);
    ASSERT_TRUE(std::holds_alternative<CommandResult>(submitted));

    const auto stopped = application->stop("lobby", created.id, epoch);
    ASSERT_TRUE(std::holds_alternative<CommandResult>(stopped));

    const auto snap = application->snapshot("lobby", created.id, epoch);
    ASSERT_TRUE(std::holds_alternative<SessionSnapshot>(snap));
    EXPECT_EQ(std::get<SessionSnapshot>(snap).session_id, created.id);

    application->close_session("lobby", created.id, epoch);
    const auto deleted = application->delete_session("lobby", created.id, epoch);
    EXPECT_FALSE(deleted);
    application->request_shutdown();
    EXPECT_TRUE(application->join_shutdown(2s));
    EXPECT_FALSE(application->running());
}

TEST(Application, SubscribeInstallsInitialSnapshotAtSequenceZero) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    auto application = Application::open(make_command(workspace, database));
    const auto epoch = application->context_epoch();
    const auto created = application->create_session("lobby", "Events", epoch);
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
        application->open_session("lobby", created.id, epoch)));

    const auto subscribed = application->subscribe(
        "lobby", created.id, SubscribeCommand{"view-1", 1, "sub-1"}, epoch);
    ASSERT_TRUE(std::holds_alternative<SubscribeResult>(subscribed));

    auto session = application->subscription_handle("lobby", created.id);
    ASSERT_TRUE(session);
    const auto item = next_output(*session);
    ASSERT_TRUE(item);
    EXPECT_EQ(item->kind, SessionOutputItem::Kind::snapshot);
    EXPECT_EQ(item->seq, 0U);
    session->acknowledge_output();
}

TEST(Application, AppearanceEditsRefreshTheSubscriptionWithoutCancellingGeneration) {
    test::TestWorkspace workspace;
    MockHttpServer server({http_response(
        "application/json", R"({"choices":[{"message":{"content":"Finished"}}]})")});
    workspace.write_provider("test",
        "host = \"127.0.0.1\"\nport = " + std::to_string(server.port())
        + "\nhttps = false\nmode = \"net\"\nmodel = \"fake\"\n"
          "api = \"chat_completions\"\nstream = false\ntimeout_s = 20\n");
    workspace.write_style("serif", "font = \"serif\"\n");
    std::ofstream(workspace.root() / "forums/lobby/config.toml", std::ios::app)
        << "default_persona = \"reader\"\n";
    auto application = Application::open(make_command(
        workspace, test::import_test_database(workspace.root())));
    const auto epoch = application->context_epoch();
    const auto voice = application->create_voice(
        {.display_name = "Narrator", .elevenlabs_voice_id = "voice-one"}, epoch);
    const auto created = application->create_session("lobby", "Appearance", epoch);
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
        application->open_session("lobby", created.id, epoch)));
    auto session = application->subscription_handle("lobby", created.id);
    ASSERT_TRUE(session);
    ASSERT_TRUE(std::holds_alternative<SubscribeResult>(application->subscribe(
        "lobby", created.id, SubscribeCommand{"view-1", epoch, "sub-1"}, epoch)));
    ASSERT_TRUE(next_output(*session));
    session->acknowledge_output();
    ASSERT_TRUE(std::holds_alternative<CommandResult>(application->submit(
        "lobby", created.id, RawCommand{"Hello"}, epoch)));
    const auto generating = next_output(*session);
    ASSERT_TRUE(generating);
    ASSERT_TRUE(generating->snapshot.generation.active);
    const auto request_id = generating->snapshot.generation.request_id;
    session->acknowledge_output();

    // The server has not started answering yet, so each edit must preserve the
    // same request and deliver its new presentation over the same subscription.
    const auto check_refresh = [&] {
        auto item = next_output(*session);
        EXPECT_TRUE(item);
        if (item) {
            EXPECT_EQ(item->kind, SessionOutputItem::Kind::snapshot);
            EXPECT_EQ(item->snapshot.lifecycle, SessionLifecycle::running);
            EXPECT_TRUE(item->snapshot.generation.active);
            EXPECT_EQ(item->snapshot.generation.request_id, request_id);
        }
        EXPECT_EQ(application->subscription_handle("lobby", created.id), session);
        session->acknowledge_output();
        return item;
    };
    (void)application->update_character("guide",
        {.provider = "test", .style = "serif", .voice = voice.id}, epoch);
    auto refreshed = check_refresh();
    ASSERT_TRUE(refreshed);
    EXPECT_EQ(refreshed->snapshot.characters.front().appearance.font, CharacterFont::serif);
    ASSERT_TRUE(refreshed->snapshot.characters.front().voice);
    EXPECT_EQ(refreshed->snapshot.characters.front().voice->elevenlabs_voice_id, "voice-one");

    (void)application->update_style("serif",
        {.display_name = "Serif", .appearance = {.font = CharacterFont::mono}}, epoch);
    refreshed = check_refresh();
    ASSERT_TRUE(refreshed);
    EXPECT_EQ(refreshed->snapshot.characters.front().appearance.font, CharacterFont::mono);

    (void)application->update_voice(voice.id,
        {.display_name = "Narrator", .elevenlabs_voice_id = "voice-two"}, epoch);
    refreshed = check_refresh();
    ASSERT_TRUE(refreshed);
    ASSERT_TRUE(refreshed->snapshot.characters.front().voice);
    EXPECT_EQ(refreshed->snapshot.characters.front().voice->elevenlabs_voice_id, "voice-two");

    (void)application->update_persona("reader",
        {.style = std::optional<std::string>("serif")}, epoch);
    ASSERT_TRUE(check_refresh());

    server.start();
    server.join();
    SessionSnapshot final;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    do {
        const auto result = application->snapshot("lobby", created.id, epoch);
        ASSERT_TRUE(std::holds_alternative<SessionSnapshot>(result));
        final = std::get<SessionSnapshot>(result);
        if (!final.generation.active) break;
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    EXPECT_FALSE(final.generation.active);
    ASSERT_FALSE(final.transcript.empty());
    EXPECT_EQ(final.transcript.back().text, "Finished");
    EXPECT_EQ(final.transcript.back().status, EntryStatus::complete);
}

TEST(Application, FatalConfigurationEditsCloseAdmissionAndStopLiveSessions) {
    for (const bool settings_edit : {false, true}) {
        SCOPED_TRACE(settings_edit);
        test::TestWorkspace workspace;
        const auto database = test::import_test_database(workspace.root());
        auto application = Application::open(make_command(workspace, database));
        const auto epoch = application->context_epoch();
        const auto created = application->create_session("lobby", "Failure", epoch);
        ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
            application->open_session("lobby", created.id, epoch)));
        auto session = application->subscription_handle("lobby", created.id);
        ASSERT_TRUE(session);
        force_next_workspace_config_fault(WorkspaceConfigFault::publication);
        try {
            if (settings_edit) (void)application->create_style("Committed style", epoch);
            else (void)application->update_character_definition(
                "guide", {.display_name = "Committed guide"}, epoch);
            FAIL() << "Expected restart-required publication failure";
        } catch (const ApplicationError& error) {
            EXPECT_EQ(error.code, ErrorCode::application_unavailable);
        }
        EXPECT_EQ(application->state(), ApplicationState::unavailable);
        EXPECT_FALSE(application->running());
        EXPECT_EQ(application->bootstrap().state, ApplicationState::unavailable);
        EXPECT_EQ(application->check_context(epoch), ErrorCode::application_unavailable);
        EXPECT_THROW((void)application->create_session("lobby", "Rejected", epoch), ApplicationError);
        const auto submitted = application->submit_async("lobby", created.id, RawCommand{"Rejected"}, epoch);
        ASSERT_TRUE(std::holds_alternative<ErrorCode>(submitted));
        EXPECT_EQ(std::get<ErrorCode>(submitted), ErrorCode::application_unavailable);
        EXPECT_THROW((void)application->store().create_style("Rejected"), WorkspaceRestartRequiredError);
        EXPECT_TRUE(application->join_shutdown(2s));
        EXPECT_EQ(session->lifecycle(), LiveSessionState::finished);
        session.reset();
        application.reset();
        auto reopened = Application::open(make_command(workspace, database));
        EXPECT_TRUE(reopened->running());
        if (!settings_edit) EXPECT_EQ(reopened->get_character("guide", reopened->context_epoch()).summary.display_name,
            "Committed guide");
    }
}

TEST(Application, RejectedConfigurationLeavesApplicationAvailable) {
    test::TestWorkspace workspace;
    auto application = Application::open(make_command(
        workspace, test::import_test_database(workspace.root())));
    const auto epoch = application->context_epoch();
    EXPECT_THROW((void)application->update_character_definition(
        "guide", {.display_name = "Rejected", .character_markdown = "$$(missing.md)"},
        epoch), ApplicationError);
    EXPECT_TRUE(application->running());
    EXPECT_EQ(application->get_character("guide", epoch).summary.display_name, "Guide");
    EXPECT_NO_THROW((void)application->update_character_definition(
        "guide", {.display_name = "Accepted"}, epoch));
}

TEST(Application, OwnerThreadConfigurationFailureClosesAdmission) {
    test::TestWorkspace workspace;
    workspace.add_character("other", "Other");
    std::filesystem::create_directories(workspace.root() / "forums/lobby/members/other");
    std::ofstream(workspace.root() / "forums/lobby/members/other/character.toml") << "# member\n";
    auto application = Application::open(make_command(
        workspace, test::import_test_database(workspace.root())));
    const auto epoch = application->context_epoch();
    const auto created = application->create_session("lobby", "Owner failure", epoch);
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
        application->open_session("lobby", created.id, epoch)));
    force_next_workspace_config_fault(WorkspaceConfigFault::publication);
    (void)application->submit("lobby", created.id,
        SetDefaultCharacterCommand{"other"}, epoch);
    EXPECT_EQ(application->state(), ApplicationState::unavailable);
    EXPECT_EQ(application->check_context(epoch), ErrorCode::application_unavailable);
    EXPECT_TRUE(application->join_shutdown(2s));
}

TEST(Application, CloseLeavesTerminalSnapshot) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    auto application = Application::open(make_command(workspace, database));
    const auto epoch = application->context_epoch();
    const auto created = application->create_session("lobby", "Terminal", epoch);
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
        application->open_session("lobby", created.id, epoch)));
    ASSERT_TRUE(std::holds_alternative<SubscribeResult>(
        application->subscribe(
            "lobby", created.id, SubscribeCommand{"view-1", 1, "sub-1"}, epoch)));
    auto session = application->subscription_handle("lobby", created.id);
    ASSERT_TRUE(session);
    (void)next_output(*session);
    session->acknowledge_output();
    application->close_session("lobby", created.id, epoch);
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (session->lifecycle() != LiveSessionState::finished
        && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    const auto terminal = next_output(*session);
    ASSERT_TRUE(terminal);
    EXPECT_EQ(terminal->kind, SessionOutputItem::Kind::snapshot);
    EXPECT_EQ(
        terminal->snapshot.lifecycle, SessionLifecycle::stopping);
}

TEST(Application, AsyncSubmitCompletesBeforeGenerationFinishes) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    auto application = Application::open(make_command(workspace, database));
    const auto epoch = application->context_epoch();
    const auto created = application->create_session("lobby", "Async", epoch);
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
        application->open_session("lobby", created.id, epoch)));

    auto outcome = application->submit_async(
        "lobby", created.id, RawCommand{"Question"}, epoch);
    auto* reply = std::get_if<std::shared_ptr<CommandReply>>(&outcome);
    ASSERT_TRUE(reply);
    const auto result = (*reply)->wait_for(2s);
    ASSERT_TRUE(result);
    ASSERT_TRUE(std::holds_alternative<CommandResult>(*result));
}

TEST(Application, IgnoresObsoleteNativeWebSection) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    const std::filesystem::path config_directory =
        workspace.root() / "cha-config";
    std::filesystem::create_directories(config_directory);
    {
        std::ofstream app(config_directory / "app.toml");
        app << "vault = \"Test\"\n"
            << "[web]\nhost = 1\nport = \"nope\"\nextra = true\n"
            << "[logging]\nfile = \"runtime.log\"\nlevel = \"off\"\n";
    }
    {
        std::ofstream vault(config_directory / "test.toml");
        vault << "vault_name = \"Test\"\n"
              << "data = " << std::quoted(database.string()) << "\n";
    }
    const auto loaded = load_configuration_directory(
        config_directory);
    ASSERT_FALSE(loaded.warnings.empty());
    EXPECT_NE(loaded.warnings.front().find("[web]"), std::string::npos);
    VaultDefinition selected = *find_vault(loaded.vaults, loaded.startup_vault);
    auto application = Application::open(ApplicationCommand{
        .config_directory = loaded.directory,
        .vaults = loaded.vaults,
        .vault = selected,
        .log_file = loaded.log_file,
        .log_level = loaded.log_level,
        .warnings = loaded.warnings,
    });
    EXPECT_TRUE(application->running());
}

TEST(Application, ListsRenamesExportsAndProtectsWelcome) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    auto application = Application::open(make_command(workspace, database));
    const auto epoch = application->context_epoch();

    const auto created = application->create_session("lobby", "Notes", epoch);
    const auto listed = application->list_sessions("lobby", epoch);
    ASSERT_FALSE(listed.empty());
    EXPECT_EQ(listed.front().id, created.id);
    EXPECT_EQ(listed.front().label, "Notes");
    EXPECT_FALSE(listed.front().live);

    const auto renamed = application->rename_session(
        "lobby", created.id, "Renamed", epoch);
    EXPECT_EQ(renamed.label, "Renamed");

    const auto exported = application->export_session("lobby", created.id, epoch);
    EXPECT_NE(exported.markdown.find("CHA session: Renamed"), std::string::npos);

    EXPECT_THROW(
        (void)application->rename_session(
            "builtin-entrance", "builtin-welcome", "Hijacked", epoch),
        ApplicationError);
    EXPECT_EQ(
        application->delete_session("builtin-entrance", "builtin-welcome", epoch),
        ErrorCode::not_found);
}

TEST(Application, CharacterPersonaForumAndFileEditsUseTheStore) {
    test::TestWorkspace workspace;
    const std::filesystem::path database =
        test::import_test_database(workspace.root());
    auto application = Application::open(make_command(workspace, database));
    const auto epoch = application->context_epoch();

    const auto character = application->create_character(
        {.display_name = "Mentor", .description = "A guide"}, epoch);
    EXPECT_FALSE(character.summary.id.empty());
    EXPECT_EQ(character.summary.display_name, "Mentor");
    EXPECT_TRUE(character.writable);

    const auto fetched = application->get_character(character.summary.id, epoch);
    EXPECT_EQ(fetched.summary.display_name, "Mentor");

    const auto defined = application->update_character_definition(
        character.summary.id,
        {.display_name = "Mentor", .character_markdown = "Be brief."}, epoch);
    EXPECT_EQ(defined.character_markdown, "Be brief.");

    const auto file = application->create_character_file(
        character.summary.id, "NOTES.md", "# Notes\n", epoch);
    EXPECT_EQ(file.filename, "NOTES.md");
    EXPECT_EQ(file.content, "# Notes\n");
    EXPECT_TRUE(file.writable);

    const auto persona = application->create_persona("Narrator", epoch);
    EXPECT_EQ(persona.summary.display_name, "Narrator");
    const auto updated_persona = application->update_persona(
        persona.summary.id,
        {.display_name = "Narrator", .persona_markdown = "Speak plainly."}, epoch);
    EXPECT_EQ(updated_persona.persona_markdown, "Speak plainly.");

    const auto forum = application->create_forum(
        {.display_name = "Workshop", .persona_id = persona.summary.id}, epoch);
    EXPECT_EQ(forum.summary.display_name, "Workshop");
    const auto members = application->update_forum_members(
        forum.summary.id,
        {.character_ids = {"guide"}, .persona_id = persona.summary.id}, epoch);
    ASSERT_FALSE(members.summary.members.empty());
    EXPECT_EQ(members.summary.members.front().id, "guide");

    application->delete_forum(forum.summary.id, epoch);
    application->delete_character(character.summary.id, epoch);
    application->delete_persona(persona.summary.id, epoch);
}

} // namespace
} // namespace cha::app
