#include "app/application.h"
#include "providers/api_key_store.h"
#include "storage/sqlite_storage.h"

#include "support/test_workspace.h"
#include "support/mock_http_server.h"
#include "runtime/live_session.h"
#include "workspace/builtins.h"
#include "workspace/workspace_config_store.h"

#include <gtest/gtest.h>

#include "util/logging.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <string>
#include <thread>
#include <tuple>
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
}

TEST(Application, EventsAreBestEffortAndReadFailuresHidePrivateData) {
    test::TestWorkspace workspace;
    const auto path = test::import_test_database(workspace.root());
    auto application = Application::open(make_command(workspace, path));
    const auto epoch = application->context_epoch();
    EXPECT_TRUE(application->list_events(epoch).empty());
    application->record_event(EventSeverity::success, "Saved entry", epoch);
    ASSERT_EQ(application->list_events(epoch).size(), 1U);
    EXPECT_EQ(application->list_events(epoch)[0].message, "Saved entry");

    const auto log = workspace.root() / "events.log";
    initialize_diagnostic_logging(log, "warn");
    storage::SqliteDatabase database(path, storage::SqliteDatabase::Mode::read_write);
    database.execute("CREATE TRIGGER fail_event BEFORE INSERT ON event_history "
        "BEGIN SELECT RAISE(ABORT, 'private SQL diagnostic'); END");
    EXPECT_NO_THROW(application->record_event(EventSeverity::error, "private event payload", epoch));
    EXPECT_NO_THROW(application->record_event(EventSeverity::info, "private stale payload", epoch + 1));
    EXPECT_EQ(application->list_events(epoch).size(), 1U);
    database.execute("DROP TABLE event_history");
    EXPECT_NO_THROW(application->record_event(EventSeverity::info, "private missing table payload", epoch));
    try {
        (void)application->list_events(epoch);
        FAIL() << "Expected read failure";
    } catch (const ApplicationError& error) {
        EXPECT_EQ(error.code, ErrorCode::internal_error);
        EXPECT_STREQ(error.what(), "Could not load events");
    }
    try {
        (void)application->list_events(epoch + 1);
        FAIL() << "Expected stale context failure";
    } catch (const ApplicationError& error) {
        EXPECT_EQ(error.code, ErrorCode::vault_changed);
    }
    application->request_shutdown();
    ASSERT_TRUE(application->join_shutdown(2s));
    EXPECT_NO_THROW(application->record_event(EventSeverity::info, "private shutdown payload", epoch));
    try {
        (void)application->list_events(epoch);
        FAIL() << "Expected unavailable context failure";
    } catch (const ApplicationError& error) {
        EXPECT_EQ(error.code, ErrorCode::application_unavailable);
    }
    shutdown_diagnostic_logging();
    std::ifstream input(log);
    const std::string warnings{std::istreambuf_iterator<char>(input), {}};
    EXPECT_NE(warnings.find("Event recording failed"), std::string::npos);
    EXPECT_NE(warnings.find("Event dropped: originating context unavailable"), std::string::npos);
    EXPECT_NE(warnings.find("Event history read failed"), std::string::npos);
    EXPECT_EQ(warnings.find("private"), std::string::npos);
}

TEST(Application, PictureReadsNeedNoSessionAndUseApplicationErrors) {
    test::TestWorkspace workspace;
    std::ofstream(workspace.root() / "characters/guide/PICTURE.png") << "not an image";
    auto application = Application::open(make_command(
        workspace, test::import_test_database(workspace.root())));
    const auto epoch = application->context_epoch();
    EXPECT_EQ(application->get_character("guide", epoch).markdown_files,
        (std::vector<std::string>{"CHARACTER.md", "PICTURE.png", "character.toml"}));
    const auto picture = application->get_character_picture("guide", epoch);
    ASSERT_TRUE(picture);
    EXPECT_EQ(picture->filename, "PICTURE.png");
    EXPECT_EQ(picture->content_base64, "bm90IGFuIGltYWdl");
    EXPECT_FALSE(application->get_character_picture(assistant_id, epoch));
    for (const auto& [id, context, expected] :
         std::vector<std::tuple<std::string, std::uint64_t, ErrorCode>>{
             {"missing", epoch, ErrorCode::not_found},
             {"guide", 0, ErrorCode::vault_changed},
             {"guide", epoch + 1, ErrorCode::vault_changed}}) {
        try {
            (void)application->get_character_picture(id, context);
            FAIL() << "Expected application error";
        } catch (const ApplicationError& error) {
            EXPECT_EQ(error.code, expected);
        }
    }
}

TEST(Application, CharacterTextAndPictureFilesUseTheirOwnReadAndEditPaths) {
    test::TestWorkspace workspace;
    std::ofstream(workspace.root() / "characters/guide/NOTES.txt", std::ios::binary) << "# Plain text\n<not-markdown>";
    std::ofstream(workspace.root() / "characters/guide/PICTURE.png") << "png";
    std::ofstream(workspace.root() / "characters/guide/PICTURE.jpg") << "jpg";
    auto application = Application::open(make_command(
        workspace, test::import_test_database(workspace.root())));
    const auto epoch = application->context_epoch();
    EXPECT_EQ(application->get_character_file("guide", "NOTES.txt", epoch).content,
        "# Plain text\n<not-markdown>");
    (void)application->update_character_file("guide", "NOTES.txt", "New text", epoch);
    EXPECT_EQ(application->get_character_file("guide", "NOTES.txt", epoch).content, "New text");
    (void)application->create_character_file("guide", "MORE.txt", "More text", epoch);
    EXPECT_EQ(application->get_character_file("guide", "MORE.txt", epoch).content, "More text");
    application->delete_character_file("guide", "NOTES.txt", epoch);
    EXPECT_THROW((void)application->get_character_file("guide", "NOTES.txt", epoch), ApplicationError);
    const auto picture = application->get_character_picture("guide", epoch, "PICTURE.jpg");
    ASSERT_TRUE(picture);
    EXPECT_EQ(picture->filename, "PICTURE.jpg");
    EXPECT_EQ(picture->content_base64, "anBn");
    application->delete_character_file("guide", "PICTURE.jpg", epoch);
    EXPECT_FALSE(application->get_character_picture("guide", epoch, "PICTURE.jpg"));
    ASSERT_TRUE(application->get_character_picture("guide", epoch));
    EXPECT_EQ(application->get_character_picture("guide", epoch)->filename, "PICTURE.png");
    application->update_character_picture(assistant_id, "assistant-portrait.png", "cG5n", epoch);
    EXPECT_EQ(application->get_character_picture(assistant_id, epoch, "assistant-portrait.png")->content_base64, "cG5n");
    application->delete_character_file(assistant_id, "assistant-portrait.png", epoch);
    EXPECT_FALSE(application->get_character_picture(assistant_id, epoch));
    EXPECT_THROW(application->delete_character_file("guide", "../PICTURE.png", epoch), ApplicationError);
    EXPECT_THROW(application->delete_character_file("guide", "character.toml", epoch), ApplicationError);
}

TEST(Application, CharacterFileListsIncludeAllDirectVaultFiles) {
    test::TestWorkspace workspace;
    const auto group = workspace.root() / "characters/philosophers";
    std::filesystem::create_directory(group);
    std::filesystem::rename(workspace.root() / "characters/guide", group / "guide");
    std::filesystem::create_directory(group / "guide/notes");
    std::ofstream(group / "guide/notes/CHILD.md") << "Nested note";
    std::ofstream(group / "guide/NOTES.md") << "Direct note";
    for (const auto* filename : {"PICTURE.png", "PICTURE.webp", "PICTURE.jpg", "PICTURE.jpeg", "PICTURE.gif"}) {
        std::ofstream(group / "guide" / filename, std::ios::binary) << "picture bytes";
    }
    std::ofstream(workspace.root() / "system/assistant/PICTURE.jpg", std::ios::binary) << "assistant picture";
    auto application = Application::open(make_command(
        workspace, test::import_test_database(workspace.root())));
    const auto epoch = application->context_epoch();
    EXPECT_EQ(application->get_character("guide", epoch).markdown_files,
        (std::vector<std::string>{"CHARACTER.md", "NOTES.md", "PICTURE.gif", "PICTURE.jpeg",
            "PICTURE.jpg", "PICTURE.png", "PICTURE.webp", "character.toml"}));
    EXPECT_EQ(application->get_character(assistant_id, epoch).markdown_files,
        (std::vector<std::string>{"CHARACTER.md", "PICTURE.jpg", "character.toml"}));
    application->update_character_picture("guide", "PICTURE.png", "cG5n", epoch);
    EXPECT_EQ(application->get_character("guide", epoch).markdown_files,
        (std::vector<std::string>{"CHARACTER.md", "NOTES.md", "PICTURE.gif", "PICTURE.jpeg",
            "PICTURE.jpg", "PICTURE.png", "PICTURE.webp", "character.toml"}));
    EXPECT_EQ(application->get_character_file("guide", "NOTES.md", epoch).content, "Direct note");
}

TEST(Application, StartsWithoutAListenerAndBootstraps) {
    test::TestWorkspace workspace;
    std::ofstream(workspace.root() / "personas" / "reader" / "persona.toml", std::ios::app)
        << "voice = 'obsolete-voice'\n";
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
    for (const auto& persona : boot.presentation.personas) {
        EXPECT_FALSE(nlohmann::json(persona).contains("voice"));
    }
    const auto persona = application->get_persona("reader", application->context_epoch());
    EXPECT_FALSE(nlohmann::json(persona).contains("voice"));
    EXPECT_FALSE(persona.voice);
    EXPECT_TRUE(persona.available_voices.empty());
}

TEST(Application, PersonaSettingsPreserveTemplatesWhenEditingMetadata) {
    test::TestWorkspace workspace;
    const auto reader = workspace.root() / "personas" / "reader";
    const std::string prompt_template = "$${persona.display_name}: $$(BIO.md)";
    std::ofstream(reader / "PERSONA.md") << prompt_template;
    std::ofstream(reader / "BIO.md") << "Enjoys reading.";
    auto application = Application::open(make_command(
        workspace, test::import_test_database(workspace.root())));
    const auto epoch = application->context_epoch();

    EXPECT_EQ(application->store().snapshot()->find_persona("reader")->prompt,
        "Reader: Enjoys reading.");
    EXPECT_EQ(application->get_persona("reader", epoch).persona_markdown,
        prompt_template);
    const auto updated = application->update_persona(
        "reader", {.display_name = "Writer"}, epoch);
    EXPECT_EQ(updated.persona_markdown, prompt_template);
    EXPECT_EQ(application->store().snapshot()->find_persona("reader")->prompt,
        "Writer: Enjoys reading.");
    const auto saved = application->update_persona(
        "reader", {.persona_markdown = prompt_template}, epoch);
    EXPECT_EQ(saved.persona_markdown, prompt_template);
    EXPECT_EQ(application->store().snapshot()->find_persona("reader")->prompt,
        "Writer: Enjoys reading.");
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

TEST(Application, DiscardsOnlyUnusedTemporarySessions) {
    test::TestWorkspace workspace;
    auto application = Application::open(make_command(workspace, test::import_test_database(workspace.root())));
    const auto epoch = application->context_epoch();
    const auto unopened = application->create_session("lobby", "", epoch);
    EXPECT_FALSE(application->delete_session("lobby", unopened.id, epoch, true));
    EXPECT_EQ(std::get<ErrorCode>(application->open_session("lobby", unopened.id, epoch)), ErrorCode::not_found);

    const auto empty = application->create_session("lobby", "", epoch);
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(application->open_session("lobby", empty.id, epoch)));
    EXPECT_FALSE(application->delete_session("lobby", empty.id, epoch, true));
    EXPECT_TRUE(application->list_sessions("lobby", epoch).empty());
    EXPECT_FALSE(application->delete_session("lobby", empty.id, epoch, true));

    for (const auto* text : {"@- Keep this note", "Hello"}) {
        const auto used = application->create_session("lobby", "", epoch);
        ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(application->open_session("lobby", used.id, epoch)));
        (void)application->submit("lobby", used.id, RawCommand{text}, epoch);
        EXPECT_FALSE(application->delete_session("lobby", used.id, epoch, true));
        EXPECT_TRUE(std::holds_alternative<SessionSnapshot>(application->snapshot("lobby", used.id, epoch)));
    }
    const auto named = application->create_session("lobby", "Intentionally empty", epoch);
    EXPECT_FALSE(application->delete_session("lobby", named.id, epoch, true));
    EXPECT_EQ(application->list_sessions("lobby", epoch).size(), 3u);
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

TEST(Application, BackgroundGenerationRetainsSessionOwnershipUntilRetirement) {
    test::TestWorkspace workspace;
    MockHttpServer server({http_response(
        "application/json", R"({"choices":[{"message":{"content":"Finished"}}]})")});
    workspace.write_provider("test",
        "host = \"127.0.0.1\"\nport = " + std::to_string(server.port())
        + "\nhttps = false\nmode = \"net\"\nmodel = \"fake\"\n"
          "api = \"chat_completions\"\nstream = false\ntimeout_s = 20\n");
    const auto command = make_command(workspace, test::import_test_database(workspace.root()));
    auto main = Application::open(command);
    const auto epoch = main->context_epoch();
    const auto first = main->create_session("lobby", "Background", epoch);
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(main->open_session("lobby", first.id, epoch)));
    auto handle = main->subscription_handle("lobby", first.id);
    ASSERT_TRUE(handle);
    ASSERT_TRUE(std::holds_alternative<CommandResult>(main->submit(
        "lobby", first.id, RawCommand{"Hello"}, epoch)));
    ASSERT_TRUE(std::get<SessionSnapshot>(main->snapshot("lobby", first.id, epoch)).generation.active);
    const auto second = main->create_session("lobby", "Selected", epoch);
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(main->open_session("lobby", second.id, epoch)));
    auto secondary = Application::open(command);
    try {
        (void)secondary->open_session("lobby", first.id, secondary->context_epoch());
        FAIL() << "Expected session ownership refusal";
    } catch (const ApplicationError& error) {
        EXPECT_EQ(error.code, ErrorCode::invalid_argument);
        EXPECT_STREQ(error.what(), "This session is open in another CHA process.");
    }
    // Saved history can still be exported while another process owns the controller.
    EXPECT_NE(secondary->export_session("lobby", first.id, secondary->context_epoch()).markdown.find("Hello"),
        std::string::npos);
    server.start();
    server.join();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (handle->lifecycle() != LiveSessionState::finished
        && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    ASSERT_EQ(handle->lifecycle(), LiveSessionState::finished);
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(secondary->open_session(
        "lobby", first.id, secondary->context_epoch())));
    const auto restored = std::get<SessionSnapshot>(secondary->snapshot(
        "lobby", first.id, secondary->context_epoch()));
    EXPECT_FALSE(restored.generation.active);
    EXPECT_EQ(restored.transcript.back().text, "Finished");
}

TEST(Application, AppearanceAndToolEditsRefreshTheSubscriptionWithoutCancellingGeneration) {
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

    for (std::optional<bool> tool : {std::optional<bool>{true}, std::optional<bool>{false}, std::optional<bool>{}}) {
        const auto saved = application->update_character("guide",
            {.provider = "test", .style = "serif", .voice = voice.id, .web_search_tool = tool}, epoch);
        EXPECT_EQ(saved.web_search_tool, tool);
        ASSERT_TRUE(check_refresh());
    }

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

TEST(Application, StartupLoadsHarmlessFieldsAndKeepsKnownSettings) {
    test::TestWorkspace workspace;
    std::ofstream(workspace.root() / "characters" / "guide" / "character.toml")
        << "display_name = \"Guide\"\nprovider = \"test\"\nlegacy_flag = true\n";
    std::filesystem::create_directories(workspace.root() / "system" / "web-search");
    std::ofstream(workspace.root() / "system" / "web-search" / "config.toml")
        << "provider = \"brave\"\nquery_provider = \"test\"\n";
    const auto log_file = workspace.root() / "startup-warnings.log";
    initialize_diagnostic_logging(log_file, "warn");
    auto application = Application::open(make_command(
        workspace, test::import_test_database(workspace.root())));
    const auto epoch = application->context_epoch();
    EXPECT_EQ(application->get_character("guide", epoch).summary.display_name, "Guide");
    EXPECT_EQ(application->store().snapshot()->web_search().provider, "brave");
    shutdown_diagnostic_logging();
    const auto logged = [&] {
        std::ifstream input(log_file);
        return std::string{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
    }();
    EXPECT_NE(logged.find("unsupported field 'legacy_flag'"), std::string::npos);
    EXPECT_NE(logged.find("Ignoring unused web search field: query_provider"),
        std::string::npos);
}

TEST(Application, SharedCharacterInstructionsPersistAndRebuildPrompts) {
    test::TestWorkspace workspace;
    std::ofstream(workspace.root() / "characters/character-voice.md") << "Original voice.";
    std::ofstream(workspace.root() / "characters/guide/CHARACTER.md", std::ios::binary)
        << "$$(../character-voice.md)\nGuide profile.";
    const auto database = test::import_test_database(workspace.root());
    auto application = Application::open(make_command(workspace, database));
    const auto epoch = application->context_epoch();
    const auto original = application->get_character_instructions(epoch);
    EXPECT_EQ(original.filename, "character-voice.md");
    EXPECT_EQ(original.content, "Original voice.");
    EXPECT_TRUE(original.writable);
    const std::string replacement = "Portray $${character.display_name}.";
    EXPECT_EQ(application->update_character_instructions(replacement, epoch).content, replacement);
    EXPECT_THROW((void)application->update_character_instructions("$$(missing.md)", epoch), ApplicationError);
    EXPECT_EQ(application->get_character_instructions(epoch).content, replacement);
    EXPECT_THROW((void)application->get_character_instructions(epoch + 1), ApplicationError);
    EXPECT_THROW((void)application->update_character_instructions("Wrong vault", epoch + 1), ApplicationError);
    EXPECT_THROW(application->delete_character_file("guide", "../character-voice.md", epoch), ApplicationError);
    application->request_shutdown();
    ASSERT_TRUE(application->join_shutdown(2s));
    application.reset();

    auto store = WorkspaceConfigStore::open(database);
    EXPECT_EQ(store->get_character_instructions(), replacement);
    EXPECT_EQ(store->snapshot()->find_forum_member("lobby", "guide")->character_prompt,
        "Portray Guide.\nGuide profile.");
}

TEST(Application, MissingSharedCharacterInstructionsCanBeCreatedAndCleared) {
    test::TestWorkspace workspace;
    auto application = Application::open(make_command(
        workspace, test::import_test_database(workspace.root())));
    const auto epoch = application->context_epoch();
    EXPECT_TRUE(application->get_character_instructions(epoch).content.empty());
    const std::string content(70 * 1024, 'x');
    EXPECT_EQ(application->update_character_instructions(content, epoch).content, content);
    EXPECT_EQ(application->get_character_instructions(epoch).content, content);
    (void)application->update_character_instructions("", epoch);
    EXPECT_TRUE(application->get_character_instructions(epoch).content.empty());
}

TEST(Application, SystemPromptPreviewIsStableWithoutCreatingASession) {
    test::TestWorkspace fixture;
    const auto workspace = Workspace::load(fixture.root());
    const auto database = test::import_test_database(fixture.root());
    auto application = Application::open(make_command(fixture, database));
    const auto epoch = application->context_epoch();
    ASSERT_TRUE(application->list_sessions("lobby", epoch).empty());
    const auto prompt = application->get_forum_system_prompt("lobby", "guide", epoch);
    EXPECT_EQ(prompt, workspace.character_definition(
        FullSessionId{"lobby", "prompt-preview"}, "guide").system_prompt);
    EXPECT_EQ(application->get_forum_system_prompt("lobby", "guide", epoch), prompt);
    EXPECT_TRUE(application->list_sessions("lobby", epoch).empty());
    const auto created = application->create_session("lobby", "Actual session", epoch);
    ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(
        application->open_session("lobby", created.id, epoch)));
    EXPECT_EQ(application->get_forum_system_prompt("lobby", "guide", epoch), prompt);
    EXPECT_EQ(application->list_sessions("lobby", epoch).size(), 1U);
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
    application->delete_character(character.summary.id, application->context_epoch());
    application->delete_persona(persona.summary.id, application->context_epoch());
}

} // namespace
} // namespace cha::app
