#include "providers/jev.h"
#include "providers/providers.h"
#include "providers/api_key_store.h"
#include "runtime/live_session_manager.h"
#include "runtime/text_input.h"
#include "storage/sqlite_storage.h"
#include "support/test_live_session.h"
#include "support/test_notifier.h"
#include "support/mock_http_server.h"
#include "support/test_workspace.h"
#include "support/test_transcript.h"
#include "util/logging.h"
#include "workspace/workspace_config_store.h"

#include <gtest/gtest.h>
#include <fstream>
#include <future>
#include <thread>

namespace cha {
namespace {
using namespace std::chrono_literals;

JevRequestInput jev_input() {
    JevRequestInput input{{.api_key_id = "key-1"}, "Marcus, do you agree with Seneca?",
        {{"character_1", "seneca", "Seneca"}, {"character_2", "marcus", "Marcus"}}};
    return input;
}

TEST(JevProtocol, SendsPromptContextAndQuestionsAndValidatesExactChoice) {
    auto input = jev_input();
    const auto body = make_jev_body(input);
    EXPECT_EQ(body["model"], "typesafe/jev-1.13");
    EXPECT_EQ(body["state"].size(), 2u);
    EXPECT_TRUE(body["state"]["previous_turn"].is_null());
    EXPECT_EQ(body["state"]["prompt"], input.prompt);
    EXPECT_EQ(body["questions"]["recipient"]["criteria"].size(), 5u);
    EXPECT_EQ(body["questions"].size(), 4u);
    const auto serialized = nlohmann::ordered_json::parse(body.dump());
    std::vector<std::string> names;
    for (const auto& [name, question] : serialized["questions"].items()) {
        names.push_back(name);
        if (name == "recipient") continue;
        EXPECT_EQ(question["type"], "choice");
        std::vector<std::string> choices;
        for (const auto& [choice, criterion] : question["criteria"].items()) {
            choices.push_back(choice);
            EXPECT_TRUE(criterion.is_string());
        }
        EXPECT_EQ(choices, (std::vector<std::string>{"yes", "no"}));
    }
    EXPECT_EQ(names, (std::vector<std::string>{"recipient", "search_required",
        "page_read_required", "actual_data_required"}));
    EXPECT_FALSE(body["questions"].contains("web_search"));
    const auto response = [](std::string recipient) {
        return nlohmann::json{{"answers", {
            {"recipient", {{"type", "choice"}, {"choice", recipient}}}}}};
    };
    for (const std::string choice : {"character_1", "character_2", "undefined", "all_characters", "self_note"}) {
        EXPECT_EQ(parse_jev_result(response(choice), input).choice, choice);
    }
    for (const std::string choice : {"Seneca", " character_1", "character_1 extra", "character_9"}) {
        EXPECT_EQ(parse_jev_result(response(choice), input).outcome, JevOutcome::failure);
    }
    EXPECT_EQ(parse_jev_result({{"answers", {{"recipient", {{"type", "text"}, {"choice", "character_1"}}}}}}, input).outcome, JevOutcome::failure);
    EXPECT_EQ(parse_jev_result(nullptr, input).outcome, JevOutcome::failure);
    input.characters = {{"character_1", "a", "Undefined"}, {"character_2", "b", "Undefined"}};
    EXPECT_EQ(make_jev_body(input)["questions"]["recipient"]["criteria"].size(), 5u);
}

TEST(JevProtocol, IgnoresObsoleteSearchAnswer) {
    const auto response = nlohmann::json::parse(
        R"({"answers":{"recipient":{"type":"choice","choice":"character_2"},"web_search":{"type":"choice","choice":"search_direct"}}})");
    const auto result = parse_jev_result(response, jev_input());
    EXPECT_EQ(result.outcome, JevOutcome::success);
    EXPECT_EQ(result.choice, "character_2");
}

TEST(JevConfiguration, ValidatesAtomicallyRoundTripsAndDisablesWithoutDeletingKey) {
    test::TestWorkspace fixture;
    const auto database = test::import_test_database(fixture.root());
    auto store = WorkspaceConfigStore::open(database);
    ApiKeyStore keys(*store);
    const auto key = keys.create("OpenRouter", "test-secret");
    EXPECT_FALSE(store->snapshot()->jev());
    WorkspaceJev settings{.api_key_id = key.id};
    store->apply_jev_update(settings);
    ASSERT_TRUE(store->snapshot()->jev());
    EXPECT_EQ(store->snapshot()->jev()->url, "https://openrouter.ai/api/alpha/decisions");
    EXPECT_EQ(store->snapshot()->jev()->model, "typesafe/jev-1.13");
    for (const auto& url : {"relative", "https://", "ftp://host/path", "https://host:bad/", "https://host/path with spaces"}) {
        auto invalid = settings; invalid.url = url;
        EXPECT_THROW(store->apply_jev_update(invalid), std::invalid_argument);
        EXPECT_EQ(store->snapshot()->jev()->url, settings.url);
    }
    auto invalid = settings; invalid.model = " \t";
    EXPECT_THROW(store->apply_jev_update(invalid), std::invalid_argument);
    invalid = settings; invalid.api_key_id = "missing";
    EXPECT_THROW(store->apply_jev_update(invalid), std::invalid_argument);
    const auto exported = fixture.root() / "exported";
    (void)export_workspace_configuration(database, exported, WorkspaceConfigLease::already_held);
    EXPECT_EQ(Workspace::load(exported).jev()->api_key_id, key.id);
    const auto copied = fixture.root() / "copied.sqlite3";
    (void)import_workspace_configuration(exported, copied);
    auto copy = WorkspaceConfigStore::open(copied);
    EXPECT_EQ(copy->snapshot()->jev()->model, settings.model);
    const auto captured = store->snapshot();
    store->apply_jev_update(std::nullopt);
    EXPECT_FALSE(store->snapshot()->jev());
    EXPECT_TRUE(captured->jev());
    EXPECT_EQ(keys.value(key.id), "test-secret");
    store->apply_jev_update(settings);
    keys.remove(key.id);
    EXPECT_TRUE(store->snapshot()->jev());
}

TEST(WebReadConfiguration, PersistsReaderAndKeyAcrossReloadAndExport) {
    test::TestWorkspace fixture;
    const auto database = test::import_test_database(fixture.root());
    auto store = WorkspaceConfigStore::open(database);
    WorkspaceWebSearch settings;
    settings.read_provider = "firecrawl";
    {
        ApiKeyStore keys(*store);
        settings.firecrawl_api_key_id = keys.create("Firecrawl", "fc-secret").id;
    }
    store->apply_web_search_update(settings);
    store.reset();
    auto reloaded = WorkspaceConfigStore::open(database);
    EXPECT_EQ(reloaded->snapshot()->web_search().read_provider, "firecrawl");
    EXPECT_EQ(reloaded->snapshot()->web_search().firecrawl_api_key_id, settings.firecrawl_api_key_id);
    const auto exported = fixture.root() / "exported";
    (void)export_workspace_configuration(database, exported, WorkspaceConfigLease::already_held);
    const auto copy = Workspace::load(exported).web_search();
    EXPECT_EQ(copy.read_provider, "firecrawl");
    EXPECT_EQ(copy.firecrawl_api_key_id, settings.firecrawl_api_key_id);
    // Unknown reader settings do not disable a valid search configuration.
    const auto path = exported / "system/web-search/config.toml";
    std::ofstream(path) << "provider='brave'\ntool_enabled=true\nread_provider='jina'\njina_api_key=123\n";
    const auto obsolete = Workspace::load(exported).web_search();
    EXPECT_TRUE(obsolete.tool_enabled);
    EXPECT_EQ(obsolete.read_provider, "off");
}

TEST(JevConfiguration, UnknownFieldsAreIgnoredAndTargetMarkersAreNotSavedDefaults) {
    test::TestWorkspace fixture;
    std::filesystem::create_directories(fixture.root() / "system/jev");
    std::ofstream(fixture.root() / "system/jev/config.toml")
        << "url='https://openrouter.ai/api/alpha/decisions'\nmodel='typesafe/jev-1.13'\napi_key='missing'\nobsolete=true\n";
    EXPECT_TRUE(Workspace::load(fixture.root()).jev());
    auto store = WorkspaceConfigStore::open(test::import_test_database(fixture.root()));
    for (const auto* marker : {"*", "-"}) {
        EXPECT_THROW(store->apply_forum_default_character("lobby", marker), std::invalid_argument);
        test::TestWorkspace invalid;
        invalid.add_character(marker, "Invalid");
        EXPECT_THROW((void)Workspace::load(invalid.root()), std::runtime_error);
    }
}

TEST(JevConfiguration, InvalidOptionalConfigurationDoesNotPreventWorkspaceLoad) {
    test::TestWorkspace fixture;
    std::filesystem::create_directories(fixture.root() / "system/jev");
    const auto path = fixture.root() / "system/jev/config.toml";
    const auto log_file = fixture.root() / "jev-warnings.log";
    initialize_diagnostic_logging(log_file, "warn");
    for (const auto* contents : {
             "url=[",
             "url='https://example.com'\nmodel='jev'\n",
             "url='relative'\nmodel='jev'\napi_key='saved-key'\n",
             "url='https://example.com'\nmodel='  '\napi_key='saved-key'\n",
             "url='https://example.com'\nmodel='jev'\napi_key=123\n"}) {
        SCOPED_TRACE(contents);
        std::ofstream(path) << contents;
        EXPECT_NO_THROW({
            const auto workspace = Workspace::load(fixture.root());
            EXPECT_FALSE(workspace.jev());
            EXPECT_NE(workspace.find_forum("lobby"), nullptr);
        });
    }
    shutdown_diagnostic_logging();
    std::ifstream log(log_file);
    const std::string warnings{std::istreambuf_iterator<char>(log), {}};
    EXPECT_NE(warnings.find("Recipient detection configuration is ignored:"), std::string::npos);
}

TEST(JevProtocol, DecisionsTransportUsesFullEndpointAndSavedKey) {
    const std::string body = R"({"answers":{"recipient":{"type":"choice","choice":"character_2"}}})";
    MockHttpServer server({"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
        + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body});
    auto input = jev_input();
    input.config.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/api/alpha/decisions";
    input.deadline = std::chrono::steady_clock::now() + 2s;
    std::atomic_bool cancelled{};
    server.start();
    auto result = classify_jev(input.config, "test-secret", input, cancelled);
    server.join();
    EXPECT_EQ(result.outcome, JevOutcome::success);
    EXPECT_EQ(result.choice, "character_2");
    ASSERT_EQ(server.requests().size(), 1u);
    EXPECT_NE(server.requests()[0].find("POST /api/alpha/decisions "), std::string::npos);
    EXPECT_NE(server.requests()[0].find("Authorization: Bearer test-secret"), std::string::npos);
    EXPECT_NE(server.requests()[0].find("typesafe/jev-1.13"), std::string::npos);
}

TEST(JevProtocol, DebugLogsQuestionsAndRawDecisionsWithoutCredentials) {
    for (const auto* level : {"debug", "info"}) {
        test::TestWorkspace fixture;
        const auto path = fixture.root() / "jev-debug.log";
        initialize_diagnostic_logging(path, level);
        const std::string body = R"({"echo":"secret-jev-key","trace":"raw-marker","answers":{"recipient":{"type":"choice","choice":"character_2"}}})";
        MockHttpServer server({http_response("application/json", body)});
        auto input = jev_input();
        input.config.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/decisions";
        input.deadline = std::chrono::steady_clock::now() + 2s;
        server.start();
        const auto result = classify_jev(input.config, "secret-jev-key", input, std::atomic_bool{false});
        server.join();
        shutdown_diagnostic_logging();
        EXPECT_EQ(result.choice, "character_2");
        std::ifstream file(path);
        const std::string output{std::istreambuf_iterator<char>(file), {}};
        EXPECT_EQ(output.find("secret-jev-key"), std::string::npos);
        EXPECT_EQ(output.find("Authorization:"), std::string::npos);
        if (std::string_view(level) == "debug") {
            EXPECT_NE(output.find(input.prompt), std::string::npos);
            EXPECT_NE(output.find("Jev request body"), std::string::npos);
            EXPECT_NE(output.find("Jev raw response"), std::string::npos);
            EXPECT_NE(output.find("raw-marker"), std::string::npos);
            EXPECT_NE(output.find("[REDACTED]"), std::string::npos);
            EXPECT_NE(output.find("duration_ms="), std::string::npos);
        } else {
            EXPECT_EQ(output.find(input.prompt), std::string::npos);
            EXPECT_EQ(output.find("raw-marker"), std::string::npos);
        }
    }
}

class JevRouting : public ::testing::Test {
protected:
    test::TestWorkspace fixture;
    test::TemporarySessionFile journal{"jev", {"lobby", "session"}};
    std::unique_ptr<WorkspaceConfigStore> store;
    std::shared_ptr<test::TestNotifier> notifier = std::make_shared<test::TestNotifier>();
    std::vector<std::function<void()>> workers;
    std::vector<JevRequestInput> classified;
    std::vector<std::string> called_providers;
    std::vector<std::string> searched;
    std::string search_context = R"({"results":[{"title":"Python release","url":"https://python.org/","description":"Latest release"}]})";
    std::string search(const WorkspaceWebSearch& settings, std::string_view query,
        const std::atomic_bool&) {
        EXPECT_EQ(settings.provider, "brave");
        EXPECT_EQ(settings.api_key_id, config.api_key_id);
        searched.emplace_back(query);
        return search_context;
    }
    std::vector<GenerationRequest> generation_inputs;
    JevResult decision{JevOutcome::success, "undefined"};
    std::shared_ptr<Providers> providers;
    std::unique_ptr<SessionController> controller;
    WorkspaceJev config;
    void SetUp() override {
        fixture.add_character("marcus", "Marcus");
        fixture.write_provider("query", "host = \"query.test\"\nport = 1\nmode = \"test\"\nmodel = \"query-model\"\n");
        const auto member = fixture.root() / "forums/lobby/members/marcus";
        std::filesystem::create_directories(member);
        std::ofstream(member / "character.toml") << "# member\n";
        store = WorkspaceConfigStore::open(test::import_test_database(fixture.root()));
        ApiKeyStore keys(*store);
        config.api_key_id = keys.create("OpenRouter", "test-secret").id;
        store->apply_jev_update(config);
        providers = std::make_shared<Providers>(
            [this](SharedCharacterDefinition definition) {
                called_providers.push_back(definition->provider.id);
                return std::make_unique<ProviderClient>(std::move(definition));
            },
            [this](auto worker) { workers.push_back(std::move(worker)); },
            [this](const auto& input, const auto&) { classified.push_back(input); return decision; },
            [this](const auto& settings, auto query, const auto& cancelled) {
                return search(settings, query, cancelled);
            });
        controller = make_controller(notifier);
    }
    std::unique_ptr<SessionController> make_controller(
        std::shared_ptr<WakeNotifier> wake, SessionRestore restored = {}) {
        return SessionController::from_workspace_for_testing(
            [this] { return store->snapshot(); }, "guide", "reader", journal.path(),
            providers, std::move(wake), std::move(restored), {}, {"lobby", "session"});
    }
    void run_workers() {
        auto pending = std::exchange(workers, {});
        for (auto& worker : pending) worker();
    }
    void use_runtime_providers(JevResult result) {
        controller.reset();
        providers = std::make_shared<Providers>(
            [](SharedCharacterDefinition definition) {
                auto backend = test::scripted_backend(std::make_shared<test::BackendControls>(),
                    definition->character.id, definition->character.display_name);
                return std::make_unique<test::RequestBackendFacade>(
                    std::make_shared<test::RequestBackendFacade::Slot>(std::move(backend)));
            }, ProviderThreadLauncher{},
            [result](const auto&, const auto&) { return result; });
    }
    void finish() {
        for (int i = 0; i < 5 && controller->is_generating(); ++i) {
            run_workers(); (void)controller->receive_events(100);
        }
    }
    void TearDown() override {
        controller.reset();
        run_workers();
        providers->shutdown();
    }
    CommandResult send(std::string text) { return handle_text_input(*controller, "reader", std::move(text)); }
};

class SessionNaming : public JevRouting {
protected:
    std::string title = "Planning a small garden";
    GenerationResult title_result;
    std::vector<GenerationRequest> title_inputs;
    std::vector<SharedCharacterDefinition> title_definitions;
    std::shared_future<void> release_title;
    std::promise<void>* title_started{};

    class NameBackend final : public ModelBackend {
    public:
        explicit NameBackend(SessionNaming& test) : test_(test) {}
        RequestPayload prepare(const GenerationRequest& input) override {
            test_.title_inputs.push_back(input);
            return {};
        }
        GenerationResult perform(RequestPayload, const GenerationDeltaSink& sink,
            const std::atomic_bool&) override {
            if (test_.title_started) test_.title_started->set_value();
            if (test_.release_title.valid()) test_.release_title.wait();
            sink({GenerationDeltaKind::reasoning, "This is not the title"});
            sink({GenerationDeltaKind::answer, test_.title.substr(0, 3)});
            sink({GenerationDeltaKind::answer, test_.title.substr(3)});
            return test_.title_result;
        }
    private:
        SessionNaming& test_;
    };

    ProviderClientFactory naming_factory() {
        return [this](SharedCharacterDefinition definition) -> std::unique_ptr<ModelBackend> {
            if (definition->character.id == "session-name") {
                title_definitions.push_back(definition);
                return std::make_unique<NameBackend>(*this);
            }
            return std::make_unique<ProviderClient>(std::move(definition));
        };
    }

    void SetUp() override {
        fixture.write_provider("naming",
            "host = \"naming.test\"\nport = 1\nmode = \"test\"\nmodel = \"naming-model\"\n");
        JevRouting::SetUp();
        controller.reset();
        providers->shutdown();
        auto naming_provider_config = store->snapshot()->find_provider("query")->config;
        naming_provider_config.reasoning_effort = "high";
        naming_provider_config.web_search = WebSearchMode::required;
        store->apply_provider_update("query", "Query", naming_provider_config);
        store->apply_character_settings(workspace_assistant_id, "query",
            std::nullopt, std::nullopt, "high", WebSearchMode::required, true);
        providers = std::make_shared<Providers>(naming_factory(),
            [this](auto worker) { workers.push_back(std::move(worker)); },
            [this](const auto& input, const auto&) { classified.push_back(input); return decision; });
        {
            storage::SqliteDatabase database(journal.path(), storage::SqliteDatabase::Mode::read_write);
            database.execute("UPDATE sessions SET label = 'New session', recent_pending = 1, discardable = 1");
        }
        controller = make_controller(notifier, load_session_state(journal.path()));
        controller->enable_auto_naming("New session");
    }
};

bool is_timestamp_label(std::string_view label) {
    if (label.size() != 19) return false;
    for (std::size_t index = 0; index < label.size(); ++index) {
        const bool separator = index == 4 || index == 7 || index == 10
            || index == 13 || index == 16;
        if (separator ? label[index] != '-'
                      : label[index] < '0' || label[index] > '9') {
            return false;
        }
    }
    return true;
}

TEST_F(SessionNaming, StartsWithJevAndWaitsForBothBeforeTheReply) {
    EXPECT_TRUE(controller->recent_pending());
    EXPECT_TRUE(load_session_state(journal.path()).discardable);
    (void)send("How should I plan a small garden?");
    EXPECT_FALSE(load_session_state(journal.path()).discardable);
    ASSERT_EQ(workers.size(), 2u);
    EXPECT_TRUE(controller->is_naming());
    EXPECT_TRUE(controller->view().transcript.entries.empty());
    EXPECT_TRUE(load_session_state(journal.path()).entries.empty());
    // Naming can finish first, but the prompt waits for Jev.
    auto name_worker = std::move(workers.back());
    workers.pop_back();
    name_worker();
    (void)controller->receive_events(100);
    EXPECT_TRUE(load_session_state(journal.path()).entries.empty());
    EXPECT_EQ(workers.size(), 1u);
    EXPECT_TRUE(controller->recent_pending());
    EXPECT_TRUE(load_session_state(journal.path()).recent_pending);
    EXPECT_EQ(read_session_database_metadata(journal.path()).label, "New session");
    ASSERT_EQ(title_inputs.size(), 1u);
    EXPECT_EQ(title_inputs[0].run.prompt_text, "How should I plan a small garden?");
    EXPECT_TRUE(title_inputs[0].history->entries.empty());
    EXPECT_FALSE(title_inputs[0].web_search_tool);
    EXPECT_FALSE(title_inputs[0].include_tool_instructions);
    ASSERT_EQ(title_definitions.size(), 1u);
    EXPECT_EQ(title_definitions[0]->provider.id, "query");
    EXPECT_EQ(title_definitions[0]->provider.config.model, "query-model");
    EXPECT_EQ(title_definitions[0]->provider.config.reasoning_effort, "low");
    EXPECT_EQ(title_definitions[0]->provider.config.web_search, WebSearchMode::off);
    EXPECT_EQ(title_definitions[0]->provider.config.timeout_s, 10);
    EXPECT_TRUE(title_definitions[0]->character_prompt.empty());
    EXPECT_TRUE(title_definitions[0]->character_description.empty());
    EXPECT_EQ(store->snapshot()->find_character(workspace_assistant_id)->reasoning_effort, "high");
    EXPECT_EQ(store->snapshot()->find_provider("query")->config.reasoning_effort, "high");
    EXPECT_NE(title_definitions[0]->system_prompt.find("at most 6 words"), std::string::npos);

    run_workers();
    const auto update = controller->receive_events(100).update;
    EXPECT_EQ(update.session_label, title);
    ASSERT_EQ(load_session_state(journal.path()).entries.size(), 1u);
    EXPECT_EQ(load_session_state(journal.path()).entries[0].text, "How should I plan a small garden?");
    EXPECT_EQ(workers.size(), 1u);
    EXPECT_FALSE(controller->recent_pending());
    EXPECT_FALSE(load_session_state(journal.path()).recent_pending);
    EXPECT_TRUE(requires_snapshot(update));
    EXPECT_TRUE(controller->is_generating());
    EXPECT_EQ(read_session_database_metadata(journal.path()).label, title);
    finish();
    ASSERT_EQ(controller->view().transcript.entries.size(), 2u);
    (void)send("And what should I plant?");
    finish();
    EXPECT_EQ(title_inputs.size(), 1u);
}

TEST_F(SessionNaming, UsesConfiguredProviderAndEffort) {
    ASSERT_EQ(store->snapshot()->find_character(workspace_assistant_id)->provider_id, "query");
    store->apply_session_naming_update({"naming", "high"});
    (void)send("Plan a garden");
    run_workers();
    (void)controller->receive_events(100);
    run_workers();
    ASSERT_EQ(title_definitions.size(), 1u);
    EXPECT_EQ(title_definitions[0]->provider.id, "naming");
    EXPECT_EQ(title_definitions[0]->provider.config.reasoning_effort, "high");
}

TEST_F(SessionNaming, IdentifiedSelfNoteIsNamedAndSavedWithoutACharacterReply) {
    decision = {JevOutcome::success, "self_note", {}};
    (void)send("Note to self: buy milk.");
    run_workers();
    const auto update = controller->receive_events(100).update;
    const auto result = controller->take_submission_result();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->outcome, SessionController::SubmissionOutcome::accepted);
    EXPECT_TRUE(result->update.input_consumed);
    EXPECT_FALSE(result->persist_default_character_id);
    EXPECT_EQ(controller->view().default_character_id, "-");
    EXPECT_EQ(update.session_label, title);
    EXPECT_FALSE(controller->is_generating());
    EXPECT_FALSE(controller->recent_pending());
    EXPECT_TRUE(workers.empty());
    const auto restored = load_session_state(journal.path());
    ASSERT_EQ(restored.entries.size(), 1u);
    EXPECT_EQ(restored.entries.front().addressed_to, "-");
    EXPECT_EQ(restored.entries.front().text, "Note to self: buy milk.");
    EXPECT_EQ(read_session_database_metadata(journal.path()).label, title);
}

TEST_F(SessionNaming, WorksWithoutJevAndLimitsTheNameToSixWords) {
    store->apply_jev_update(std::nullopt);
    title = "  Один два три четыре пять шесть семь восемь  ";
    EXPECT_TRUE(send("Помоги спланировать сад").clear_input);
    ASSERT_EQ(workers.size(), 2u);
    run_workers();
    const auto update = controller->receive_events(100).update;
    EXPECT_EQ(update.session_label, "Один два три четыре пять шесть");
    EXPECT_EQ(read_session_database_metadata(journal.path()).label, *update.session_label);
    EXPECT_TRUE(classified.empty());
    EXPECT_FALSE(controller->is_generating());
}

TEST_F(SessionNaming, ClearCancelsAnUndeliveredNameAndKeepsTheSessionVisible) {
    (void)send("@- Old note");
    ASSERT_FALSE(controller->is_generating());
    ASSERT_TRUE(controller->is_naming());
    run_workers();

    const auto cleared = send("/clear");
    EXPECT_TRUE(cleared.clear_input);
    EXPECT_FALSE(controller->is_naming());
    EXPECT_FALSE(controller->recent_pending());
    EXPECT_FALSE(controller->receive_events(100).update.session_label);
    const auto restored = load_session_state(journal.path());
    EXPECT_TRUE(restored.entries.empty());
    EXPECT_FALSE(restored.recent_pending);
    EXPECT_EQ(read_session_database_metadata(journal.path()).label, "New session");
}

TEST_F(SessionNaming, ClearInAnEmptySessionKeepsItHiddenFromRecents) {
    const auto cleared = send("/clear");
    EXPECT_TRUE(cleared.clear_input);
    EXPECT_EQ(cleared.session.notice, "");
    EXPECT_FALSE(requires_snapshot(cleared.session));
    EXPECT_TRUE(controller->recent_pending());
    EXPECT_TRUE(load_session_state(journal.path()).recent_pending);
}

TEST_F(SessionNaming, ManualRenameWinsOverACompletedButUndeliveredTitle) {
    (void)send("Plan a garden");
    run_workers();
    (void)controller->receive_events(100);
    run_workers();
    controller->rename("My own name");
    EXPECT_FALSE(controller->receive_events(100).update.session_label);
    finish();
    (void)send("Another question");
    finish();
    EXPECT_EQ(title_inputs.size(), 1u);
    EXPECT_EQ(read_session_database_metadata(journal.path()).label, "My own name");
}

TEST_F(SessionNaming, FailedTitleDoesNotInterruptTheReply) {
    title_result = {GenerationOutcome::transport_error, "Naming unavailable"};
    (void)send("Plan a garden");
    run_workers();
    const auto update = controller->receive_events(100).update;
    ASSERT_TRUE(update.session_label);
    EXPECT_TRUE(is_timestamp_label(*update.session_label));
    finish();
    EXPECT_FALSE(controller->recent_pending());
    EXPECT_FALSE(load_session_state(journal.path()).recent_pending);
    EXPECT_EQ(read_session_database_metadata(journal.path()).label, *update.session_label);
    EXPECT_EQ(controller->view().transcript.entries.back().status, EntryStatus::complete);
}

TEST_F(SessionNaming, FailedVisibilityWriteDoesNotInterruptTheReply) {
    {
        storage::SqliteDatabase database(journal.path(), storage::SqliteDatabase::Mode::read_write);
        database.execute(
            "CREATE TRIGGER reject_visibility BEFORE UPDATE OF recent_pending ON sessions "
            "BEGIN SELECT RAISE(ABORT, 'visibility write failed'); END");
    }
    title_result = {GenerationOutcome::transport_error, "Naming unavailable"};
    (void)send("Plan a garden");
    ASSERT_NO_THROW(finish());
    EXPECT_TRUE(controller->recent_pending());
    const auto restored = load_session_state(journal.path());
    EXPECT_TRUE(restored.recent_pending);
    ASSERT_EQ(restored.entries.size(), 2u);
    EXPECT_EQ(restored.entries.back().status, EntryStatus::complete);

    {
        storage::SqliteDatabase database(journal.path(), storage::SqliteDatabase::Mode::read_write);
        database.execute("DROP TRIGGER reject_visibility");
    }
    (void)send("What should I plant?");
    ASSERT_NO_THROW(finish());
    EXPECT_FALSE(controller->recent_pending());
    EXPECT_FALSE(load_session_state(journal.path()).recent_pending);
    ASSERT_EQ(controller->view().transcript.entries.size(), 4u);
    EXPECT_EQ(controller->view().transcript.entries.back().status, EntryStatus::complete);
}

TEST_F(SessionNaming, EmptyTitleUsesATimestamp) {
    title = "   ";
    (void)send("Plan a garden");
    run_workers();
    const auto update = controller->receive_events(100).update;
    ASSERT_TRUE(update.session_label);
    EXPECT_TRUE(is_timestamp_label(*update.session_label));
    finish();
    EXPECT_FALSE(controller->recent_pending());
    EXPECT_FALSE(load_session_state(journal.path()).recent_pending);
    EXPECT_EQ(read_session_database_metadata(journal.path()).label, *update.session_label);
    EXPECT_EQ(controller->view().transcript.entries.back().status, EntryStatus::complete);
}

TEST_F(SessionNaming, SkipsUserNamedSessionsAfterReopen) {
    controller->rename("My session");
    controller->enable_auto_naming("My session");
    (void)send("Plan a garden");
    EXPECT_EQ(workers.size(), 1u);
    finish();
    controller.reset();
    controller = make_controller(notifier, load_session_state(journal.path()));
    controller->enable_auto_naming(read_session_database_metadata(journal.path()).label);
    (void)send("A later prompt");
    finish();
    EXPECT_TRUE(title_inputs.empty());
}

class SessionNamingRetry : public SessionNaming, public testing::WithParamInterface<bool> {
protected:
    void SetUp() override {
        SessionNaming::SetUp();
        if (!GetParam()) store->apply_jev_update(std::nullopt);
    }
};

TEST_P(SessionNamingRetry, FailedNamingUsesTimestampAndDoesNotRetry) {
    title_result = {GenerationOutcome::transport_error, "Network unavailable"};
    (void)send("Plan a garden");
    finish();
    const std::string label = read_session_database_metadata(journal.path()).label;
    EXPECT_TRUE(is_timestamp_label(label));
    ASSERT_EQ(title_inputs.size(), 1u);
    EXPECT_EQ(title_inputs[0].run.prompt_text, "Plan a garden");
    EXPECT_FALSE(controller->recent_pending());

    (void)send("Yes, continue");
    finish();
    EXPECT_EQ(title_inputs.size(), 1u);
    EXPECT_EQ(read_session_database_metadata(journal.path()).label, label);

    controller.reset();
    controller = make_controller(notifier, load_session_state(journal.path()));
    controller->enable_auto_naming(label);
    (void)send("And after reopening?");
    finish();
    EXPECT_EQ(title_inputs.size(), 1u);
}

TEST_P(SessionNamingRetry, ReopensAfterInterruptionAndRetriesOnTheNextSubmission) {
    (void)send("Plan a garden");
    if (GetParam()) {
        ASSERT_EQ(workers.size(), 2u);
        auto name_worker = std::move(workers.back());
        workers.pop_back();
        run_workers();
        (void)controller->receive_events(100);
        EXPECT_TRUE(controller->view().transcript.entries.empty());
        controller.reset();
        name_worker();
        controller = make_controller(notifier, load_session_state(journal.path()));
        controller->enable_auto_naming(read_session_database_metadata(journal.path()).label);
        (void)send("Yes, continue");
        finish();
        ASSERT_EQ(title_inputs.size(), 1u);
        EXPECT_EQ(title_inputs[0].run.prompt_text, "Yes, continue");
        EXPECT_EQ(read_session_database_metadata(journal.path()).label, title);
        return;
    }
    ASSERT_EQ(workers.size(), 2u);
    auto name_worker = std::move(workers.back());
    workers.pop_back();
    finish();
    EXPECT_TRUE(controller->is_naming());
    EXPECT_FALSE(controller->view().transcript.entries.empty());
    (void)send("Continue while the title is pending");
    EXPECT_EQ(workers.size(), 1u);
    finish();
    EXPECT_TRUE(controller->is_naming());
    EXPECT_TRUE(title_inputs.empty());

    controller.reset();
    name_worker();
    controller = make_controller(notifier, load_session_state(journal.path()));
    controller->enable_auto_naming(read_session_database_metadata(journal.path()).label);
    EXPECT_TRUE(workers.empty());
    (void)send("Yes, continue");
    finish();
    ASSERT_EQ(title_inputs.size(), 1u);
    EXPECT_EQ(title_inputs[0].run.prompt_text, "Plan a garden");
    EXPECT_EQ(read_session_database_metadata(journal.path()).label, title);
}

TEST_P(SessionNamingRetry, ManualRenameAfterFailureStopsRetriesIncludingAfterReopen) {
    title_result = {GenerationOutcome::transport_error, "Network unavailable"};
    (void)send("Plan a garden");
    finish();
    controller->rename("My garden notes");
    title_result = {};
    (void)send("Continue");
    finish();
    EXPECT_EQ(title_inputs.size(), 1u);
    controller.reset();
    controller = make_controller(notifier, load_session_state(journal.path()));
    controller->enable_auto_naming(read_session_database_metadata(journal.path()).label);
    (void)send("Continue after reopening");
    finish();
    EXPECT_EQ(title_inputs.size(), 1u);
    EXPECT_EQ(read_session_database_metadata(journal.path()).label, "My garden notes");
}

TEST_P(SessionNamingRetry, FirstNoteGetsTimestampWhenNamingFails) {
    title_result = {GenerationOutcome::transport_error, "Network unavailable"};
    EXPECT_TRUE(send("@- Plan a garden").clear_input);
    EXPECT_FALSE(controller->is_generating());
    ASSERT_EQ(workers.size(), 1u);
    run_workers();
    const auto update = controller->receive_events(100).update;
    ASSERT_TRUE(update.session_label);
    EXPECT_TRUE(is_timestamp_label(*update.session_label));
    ASSERT_EQ(title_inputs.size(), 1u);
    EXPECT_EQ(title_inputs[0].run.prompt_text, "Plan a garden");
    ASSERT_EQ(controller->view().transcript.entries.size(), 1u);
    EXPECT_EQ(controller->view().transcript.entries[0].addressed_to, "-");

    controller.reset();
    controller = make_controller(notifier, load_session_state(journal.path()));
    controller->enable_auto_naming(read_session_database_metadata(journal.path()).label);
    (void)controller->set_default_character_by_id("-");
    EXPECT_EQ(send("Another note").clear_input, !GetParam());
    finish();
    EXPECT_TRUE(workers.empty());
    EXPECT_EQ(title_inputs.size(), 1u);
    EXPECT_EQ(read_session_database_metadata(journal.path()).label, *update.session_label);
    EXPECT_EQ(controller->view().transcript.entries.size(), 2u);
    EXPECT_EQ(classified.size(), GetParam() ? 1u : 0u);
    EXPECT_FALSE(controller->is_generating());

    (void)controller->set_default_character_by_id("guide");
    (void)send("Now answer my question");
    finish();
    EXPECT_EQ(title_inputs.size(), 1u);
}

INSTANTIATE_TEST_SUITE_P(WithAndWithoutJev, SessionNamingRetry, testing::Bool());

TEST_F(SessionNaming, StoppedClassificationDoesNotRenameAnEmptySession) {
    (void)send("Plan a garden");
    run_workers();
    (void)controller->request_stop();
    EXPECT_FALSE(controller->receive_events(100).update.session_label);
    EXPECT_FALSE(controller->is_naming());
    EXPECT_TRUE(controller->view().transcript.entries.empty());
    EXPECT_EQ(read_session_database_metadata(journal.path()).label, "New session");
    (void)send("A new first prompt");
    EXPECT_EQ(title_inputs.size(), 1u);
    EXPECT_EQ(workers.size(), 2u);
    finish();
    EXPECT_EQ(read_session_database_metadata(journal.path()).label, title);
}

TEST_F(SessionNaming, SnapshotReportsRetentionEvenWithoutEntries) {
    controller.reset();
    LiveSessionManager manager({}, [&](const FullSessionId&, auto wake, std::uint64_t) {
        return OpenedSession{.label = "New session",
            .controller = make_controller(wake, load_session_state(journal.path()))};
    });
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open({"lobby", "session"}, 2s)));
    auto session = manager.lookup({"lobby", "session"});
    EXPECT_TRUE(std::get<SessionSnapshot>(session->snapshot(2s)).discardable);

    EXPECT_TRUE(std::holds_alternative<CommandResult>(session->submit(RawCommand{"/foo"}, 2s)));
    const auto snapshot = std::get<SessionSnapshot>(session->snapshot(2s));
    EXPECT_FALSE(snapshot.discardable);
    EXPECT_TRUE(snapshot.recent_pending);
    EXPECT_TRUE(snapshot.transcript.empty());
}

class SessionNamingRuntime : public SessionNaming, public testing::WithParamInterface<bool> {};

TEST_P(SessionNamingRuntime, WaitsForNamingBeforeStartingTheReply) {
    if (GetParam()) title_result = {GenerationOutcome::transport_error, "Naming unavailable"};
    controller.reset();
    providers->shutdown();
    std::promise<void> started, release;
    title_started = &started;
    release_title = release.get_future().share();
    providers = std::make_shared<Providers>(naming_factory(), ProviderThreadLauncher{},
        [](const auto&, const auto&) { return JevResult{JevOutcome::success, "undefined"}; });
    std::string mirrored_label;
    LiveSessionManager manager({}, [&](const FullSessionId&, auto wake, std::uint64_t) {
        return OpenedSession{.label = "New session",
            .controller = make_controller(wake, load_session_state(journal.path())),
            .mirror = [&](std::string_view label, auto) { mirrored_label = label; }};
    });
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open({"lobby", "session"}, 2s)));
    auto session = manager.lookup({"lobby", "session"});
    auto queued = session->enqueue(RawCommand{"Plan a garden"});
    auto reply = std::get<std::shared_ptr<CommandReply>>(queued);
    EXPECT_EQ(started.get_future().wait_for(2s), std::future_status::ready);
    auto snapshot = std::get<SessionSnapshot>(session->snapshot(2s));
    EXPECT_TRUE(snapshot.generation.active);
    EXPECT_TRUE(snapshot.transcript.empty());
    EXPECT_FALSE(reply->peek());
    EXPECT_FALSE(session->idle_for_retirement());
    EXPECT_EQ(snapshot.session_label, "New session");
    EXPECT_TRUE(snapshot.recent_pending);
    release.set_value();
    const auto submitted = reply->wait_for(2s);
    ASSERT_TRUE(submitted);
    EXPECT_TRUE(std::holds_alternative<CommandResult>(*submitted));
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    do {
        std::this_thread::sleep_for(1ms);
        snapshot = std::get<SessionSnapshot>(session->snapshot(2s));
    } while ((snapshot.generation.active || snapshot.recent_pending)
        && std::chrono::steady_clock::now() < deadline);
    const std::string expected_label = read_session_database_metadata(journal.path()).label;
    if (GetParam()) EXPECT_TRUE(is_timestamp_label(expected_label));
    else EXPECT_EQ(expected_label, title);
    EXPECT_EQ(snapshot.session_label, expected_label);
    EXPECT_FALSE(snapshot.recent_pending);
    EXPECT_EQ(read_session_database_metadata(journal.path()).label, expected_label);
    EXPECT_EQ(mirrored_label, expected_label);
    EXPECT_TRUE(session->idle_for_retirement());
    ASSERT_EQ(snapshot.transcript.size(), 2u);
}

INSTANTIATE_TEST_SUITE_P(SuccessAndFailure, SessionNamingRuntime, testing::Bool());

TEST_F(SessionNaming, NamingTimeoutUsesTimestampAndAllowsTheReply) {
    controller.reset();
    providers->shutdown();
    std::promise<void> started, release;
    title_started = &started;
    release_title = release.get_future().share();
    providers = std::make_shared<Providers>(naming_factory(), ProviderThreadLauncher{},
        [](const auto&, const auto&) { return JevResult{JevOutcome::success, "undefined"}; });
    LiveSessionManager manager({}, [&](const FullSessionId&, auto wake, std::uint64_t) {
        return OpenedSession{.label = "New session",
            .controller = make_controller(wake, load_session_state(journal.path()))};
    });
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open({"lobby", "session"}, 2s)));
    auto session = manager.lookup({"lobby", "session"});
    auto queued = session->enqueue(RawCommand{"Plan a garden"});
    auto reply = std::get<std::shared_ptr<CommandReply>>(queued);
    EXPECT_EQ(started.get_future().wait_for(2s), std::future_status::ready);
    const auto submitted = reply->wait_for(12s);
    release.set_value();
    ASSERT_TRUE(submitted);
    EXPECT_TRUE(std::holds_alternative<CommandResult>(*submitted));
    const std::string label = read_session_database_metadata(journal.path()).label;
    ASSERT_EQ(label.size(), 19u);
    for (const std::size_t separator : {4u, 7u, 10u, 13u, 16u}) {
        EXPECT_EQ(label[separator], '-');
    }
    EXPECT_EQ(title_inputs.size(), 1u);
}

TEST_F(JevRouting, PageReadingWorksWithSearchDisabledAndMarksWebUse) {
    struct ReadBackend final : ModelBackend {
        explicit ReadBackend(bool& offered) : offered(offered) {}
        RequestPayload prepare(const GenerationRequest& input) override {
            EXPECT_FALSE(input.web_search_tool);
            read = input.web_read_tool;
            offered = static_cast<bool>(read);
            return {};
        }
        GenerationResult perform(RequestPayload, const GenerationDeltaSink& sink,
            const std::atomic_bool& cancelled) override {
            if (read) EXPECT_EQ(read("https://example.org", cancelled), "Page content");
            sink({GenerationDeltaKind::answer, "Answer"});
            return {};
        }
        bool& offered;
        std::function<std::string(std::string_view, const std::atomic_bool&)> read;
    };
    controller.reset();
    providers->shutdown();
    store->apply_jev_update(std::nullopt);
    bool offered = false;
    int reads = 0;
    providers = std::make_shared<Providers>(
        [&](SharedCharacterDefinition) { return std::make_unique<ReadBackend>(offered); },
        [this](auto worker) { workers.push_back(std::move(worker)); }, JevExecutor{}, WebSearchExecutor{},
        [&](const WorkspaceWebSearch& settings, std::string_view url, const auto&) {
            EXPECT_EQ(url, "https://example.org");
            EXPECT_NE(settings.read_provider, "off");
            ++reads;
            return "Page content";
        });
    controller = make_controller(notifier);
    for (const auto* reader : {"firecrawl", "off"}) {
        WorkspaceWebSearch settings;
        settings.read_provider = reader;
        settings.firecrawl_api_key_id = config.api_key_id;
        store->apply_web_search_update(settings);
        (void)send("Read this page");
        finish();
        EXPECT_EQ(offered, std::string_view(reader) != "off");
        EXPECT_EQ(controller->view().transcript.entries.back().web_search_used, offered);
        EXPECT_EQ(controller->view().transcript.entries.back().text, "Answer");
    }
    EXPECT_EQ(reads, 1);
    EXPECT_TRUE(classified.empty());
    // A character override must disable reading without blocking generation.
    WorkspaceWebSearch settings;
    settings.read_provider = "firecrawl";
    settings.firecrawl_api_key_id = config.api_key_id;
    store->apply_web_search_update(settings);
    store->apply_character_settings("guide", "test", std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, false);
    (void)send("Read with web tools disabled for this character");
    finish();
    EXPECT_FALSE(offered);
    EXPECT_EQ(reads, 1);
    EXPECT_FALSE(controller->view().transcript.entries.back().web_search_used);
    EXPECT_EQ(controller->view().transcript.entries.back().status, EntryStatus::complete);
    EXPECT_EQ(controller->view().transcript.entries.back().text, "Answer");
    store->apply_character_settings("guide", "test", std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, std::nullopt);
    // A removed key must disable reading without blocking generation.
    ApiKeyStore keys(*store);
    keys.remove(config.api_key_id);
    (void)send("Read with a missing key");
    finish();
    EXPECT_FALSE(offered);
    EXPECT_EQ(reads, 1);
    EXPECT_EQ(controller->view().transcript.entries.back().status, EntryStatus::complete);
}

TEST_F(JevRouting, OnDemandSearchUsesWorkspaceDefaultAndCharacterOverrideWithoutRecipientDetection) {
    struct SearchBackend final : ModelBackend {
        explicit SearchBackend(bool& offered) : offered(offered) {}
        RequestPayload prepare(const GenerationRequest& input) override {
            search = input.web_search_tool;
            offered = static_cast<bool>(search);
            return {};
        }
        GenerationResult perform(RequestPayload, const GenerationDeltaSink& sink,
            const std::atomic_bool& cancelled) override {
            if (search) {
                EXPECT_FALSE(search("model query", cancelled).empty());
            }
            sink({GenerationDeltaKind::answer, "Answer"});
            return {};
        }
        bool& offered;
        std::function<std::string(std::string_view, const std::atomic_bool&)> search;
    };
    controller.reset();
    providers->shutdown();
    store->apply_jev_update(std::nullopt);
    bool offered = false;
    providers = std::make_shared<Providers>(
        [&](SharedCharacterDefinition) { return std::make_unique<SearchBackend>(offered); },
        [this](auto worker) { workers.push_back(std::move(worker)); }, JevExecutor{},
        [this](const auto& settings, auto query, const auto& cancelled) {
            return search(settings, query, cancelled);
        });
    controller = make_controller(notifier);
    for (bool workspace_default : {false, true}) {
        store->apply_web_search_update({"brave", config.api_key_id, workspace_default});
        EXPECT_EQ(store->snapshot()->web_search().tool_enabled, workspace_default);
        for (std::optional<bool> override : {std::optional<bool>{}, std::optional<bool>{false}, std::optional<bool>{true}}) {
            store->apply_character_settings("guide", "test", std::nullopt, std::nullopt,
                std::nullopt, std::nullopt, override);
            EXPECT_EQ(store->snapshot()->find_character("guide")->web_search_tool, override);
            const auto count = searched.size();
            const bool enabled = override.value_or(workspace_default);
            (void)send("What is current?");
            finish();
            EXPECT_EQ(offered, enabled);
            EXPECT_EQ(searched.size(), count + (enabled ? 1 : 0));
            EXPECT_EQ(controller->view().transcript.entries.back().web_search_used, enabled);
            EXPECT_TRUE(classified.empty());
        }
    }
    // A character override must not advertise a tool with an empty or stale key.
    store->apply_character_settings("guide", "test", std::nullopt, std::nullopt,
        std::nullopt, std::nullopt, true);
    for (const std::string key : {"", "missing-key"}) {
        store->apply_web_search_update({"brave", key, false});
        const auto count = searched.size();
        (void)send("Answer without search");
        finish();
        EXPECT_FALSE(offered);
        EXPECT_EQ(searched.size(), count);
        EXPECT_EQ(controller->view().transcript.entries.back().status, EntryStatus::complete);
        EXPECT_EQ(controller->view().transcript.entries.back().text, "Answer");
    }
}

TEST_F(JevRouting, ConfiguredWebToolsDoNotRetrieveBeforeOrWithoutAModelCall) {
    const std::string prompt = "Get the most important story on theregister.com and explain it.";
    struct CapturingBackend final : ModelBackend {
        explicit CapturingBackend(std::vector<GenerationRequest>& requests) : requests(requests) {}
        RequestPayload prepare(const GenerationRequest& input) override {
            requests.push_back(input);
            EXPECT_TRUE(input.web_search_tool);
            EXPECT_TRUE(input.web_read_tool);
            return {};
        }
        GenerationResult perform(RequestPayload, const GenerationDeltaSink& sink,
            const std::atomic_bool&) override {
            sink({GenerationDeltaKind::answer, "Answer"});
            return {};
        }
        std::vector<GenerationRequest>& requests;
    };
    controller.reset();
    providers->shutdown();
    std::vector<GenerationRequest> requests;
    int reads = 0;
    providers = std::make_shared<Providers>(
        [&](SharedCharacterDefinition definition) {
            called_providers.push_back(definition->provider.id);
            return std::make_unique<CapturingBackend>(requests);
        },
        [this](auto worker) { workers.push_back(std::move(worker)); },
        [this](const auto& input, const auto&) { classified.push_back(input); return decision; },
        [this](const auto& settings, auto query, const auto& cancelled) {
            return search(settings, query, cancelled);
        },
        [&](const auto&, auto, const auto&) { ++reads; return "Page content"; });
    store->apply_web_search_update({"brave", config.api_key_id, true, "firecrawl", config.api_key_id});
    controller = make_controller(notifier);
    (void)send(prompt);
    finish();
    ASSERT_EQ(classified.size(), 1u);
    EXPECT_EQ(make_jev_body(classified.front())["questions"].size(), 4u);
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests.front().run.prompt_text, prompt);
    EXPECT_EQ(called_providers, (std::vector<std::string>{"test"}));
    EXPECT_TRUE(searched.empty());
    EXPECT_EQ(reads, 0);
    EXPECT_FALSE(controller->view().transcript.entries.back().web_search_used);
}

TEST_F(JevRouting, OnDemandSearchRunsOnlyWhenTheModelCallsIt) {
    struct SearchBackend final : ModelBackend {
        explicit SearchBackend(const std::vector<std::string>& calls) : calls(calls) {}
        const std::vector<std::string>& calls;
        RequestPayload prepare(const GenerationRequest& input) override {
            EXPECT_TRUE(calls.empty());
            EXPECT_EQ(input.run.prompt_text, "Original query");
            EXPECT_TRUE(input.web_search_tool);
            search = input.web_search_tool;
            return {};
        }
        GenerationResult perform(RequestPayload, const GenerationDeltaSink& sink,
            const std::atomic_bool& cancelled) override {
            if (search) (void)search("follow-up query", cancelled);
            sink({GenerationDeltaKind::answer, "Answer"});
            return {};
        }
        std::function<std::string(std::string_view, const std::atomic_bool&)> search;
    };
    controller.reset();
    providers->shutdown();
    providers = std::make_shared<Providers>(
        [this](SharedCharacterDefinition) { return std::make_unique<SearchBackend>(searched); },
        [this](auto worker) { workers.push_back(std::move(worker)); },
        [this](const auto&, const auto&) { return decision; },
        [this](const auto& settings, auto query, const auto& cancelled) {
            return search(settings, query, cancelled);
        });
    store->apply_web_search_update({"brave", config.api_key_id, true});
    controller = make_controller(notifier);
    decision = {JevOutcome::success, "undefined", {}};
    (void)send("Original query");
    finish();
    EXPECT_EQ(searched, (std::vector<std::string>{"follow-up query"}));
    EXPECT_TRUE(controller->view().transcript.entries.back().web_search_used);
}

TEST_F(JevRouting, ClassifiesImplicitPromptsButSkipsEmptyAndExplicitTargets) {
    for (const std::string text : {"", " \t\n", "\n"}) {
        EXPECT_FALSE(send(text).clear_input);
        EXPECT_FALSE(controller->submit_prompt("reader", text).input_consumed);
    }
    EXPECT_TRUE(workers.empty());
    EXPECT_TRUE(controller->view().transcript.entries.empty());
    (void)controller->set_default_character_by_id("-");
    EXPECT_FALSE(send("Another note").clear_input);
    finish();
    EXPECT_TRUE(workers.empty());
    EXPECT_TRUE(send("@Guide Explicit override").clear_input);
    run_workers(); (void)controller->receive_events(100);
    finish();
    EXPECT_EQ(controller->view().transcript.entries[1].addressed_to, "guide");
    EXPECT_EQ(controller->view().default_character_id, "-");
    (void)controller->set_default_character_by_id("*");
    EXPECT_EQ(controller->view().default_character_id, "*");
    EXPECT_TRUE(send("@- Private note").clear_input);
    store->apply_web_search_update({"brave", config.api_key_id, true});
    (void)send("/mcast @Guide only Guide");
    EXPECT_FALSE(controller->classification_pending());
    EXPECT_EQ(classified.size(), 1u);
    finish();
    EXPECT_FALSE(send("@unknown Wrong handle").clear_input);
    EXPECT_FALSE(controller->classification_pending());
    store->apply_jev_update(std::nullopt);
    EXPECT_TRUE(send("Everyone").clear_input);
    EXPECT_FALSE(controller->classification_pending());
    EXPECT_EQ(workers.size(), 2u);
    finish();
    (void)controller->set_default_character_by_id("guide");
    EXPECT_TRUE(send("Plain question").clear_input);
    finish();
    EXPECT_EQ(classified.size(), 1u);
}

TEST_F(JevRouting, IdentifiedSelfNotesStayActiveUntilAnotherRecipientIsIdentified) {
    store->apply_web_search_update({"brave", config.api_key_id, true});
    for (const auto* target : {"guide", "marcus", "*"}) {
        (void)controller->set_default_character_by_id(target);
        decision = {JevOutcome::success, "self_note"};
        EXPECT_FALSE(send("Note to self: read Marcus tomorrow.").clear_input);
        EXPECT_TRUE(controller->classification_pending());
        run_workers();
        const auto update = controller->receive_events(100).update;
        const auto result = controller->take_submission_result();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, SessionController::SubmissionOutcome::accepted);
        EXPECT_TRUE(result->update.input_consumed);
        EXPECT_TRUE(requires_snapshot(update));
        EXPECT_FALSE(result->persist_default_character_id);
        EXPECT_EQ(controller->view().default_character_id, "-");
        EXPECT_FALSE(controller->is_generating());
        EXPECT_TRUE(workers.empty());
        EXPECT_TRUE(called_providers.empty());
        EXPECT_TRUE(searched.empty());
        EXPECT_EQ(controller->view().transcript.entries.back().addressed_to, "-");
        decision = {JevOutcome::success, "undefined"};
        EXPECT_FALSE(send("Also buy milk.").clear_input);
        EXPECT_TRUE(controller->classification_pending());
        finish();
        const auto continued = controller->take_submission_result();
        ASSERT_TRUE(continued);
        EXPECT_EQ(continued->outcome, SessionController::SubmissionOutcome::accepted);
        EXPECT_EQ(controller->view().default_character_id, "-");
        EXPECT_EQ(controller->view().transcript.entries.back().addressed_to, "-");
        EXPECT_TRUE(workers.empty());
        EXPECT_TRUE(called_providers.empty());
        EXPECT_TRUE(searched.empty());
    }
    const auto restored = load_session_state(journal.path());
    ASSERT_EQ(restored.entries.size(), 6u);
    for (const auto& entry : restored.entries) {
        EXPECT_EQ(entry.kind, EntryKind::human);
        EXPECT_EQ(entry.addressed_to, "-");
    }
    EXPECT_EQ(store->snapshot()->find_forum("lobby")->default_character_id, "guide");
    decision = {JevOutcome::success, "character_2"};
    EXPECT_FALSE(send("Marcus, please answer.").clear_input);
    EXPECT_TRUE(controller->classification_pending());
    run_workers();
    (void)controller->receive_events(100);
    EXPECT_EQ(classified.size(), 7u);
    EXPECT_EQ(controller->view().default_character_id, "marcus");
    EXPECT_EQ(controller->view().transcript.entries.back().addressed_to, "marcus");
    EXPECT_EQ(workers.size(), 1u);
    finish();
}

TEST_F(JevRouting, FailedClassificationKeepsTheNotesTarget) {
    (void)controller->set_default_character_by_id("-");
    decision = {JevOutcome::failure, {}, "Unavailable"};
    EXPECT_FALSE(send("Another note.").clear_input);
    run_workers();
    const auto update = controller->receive_events(100).update;
    const auto result = controller->take_submission_result();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->outcome, SessionController::SubmissionOutcome::accepted);
    EXPECT_TRUE(result->update.input_consumed);
    EXPECT_FALSE(result->persist_default_character_id);
    EXPECT_EQ(update.notice, "Recipient detection failed. Saved as a self-note.");
    EXPECT_EQ(controller->view().default_character_id, "-");
    EXPECT_EQ(controller->view().transcript.entries.back().addressed_to, "-");
    EXPECT_FALSE(controller->is_generating());
    EXPECT_TRUE(workers.empty());
    EXPECT_TRUE(called_providers.empty());
}

TEST_F(JevRouting, RuntimeAcceptsSelfNotesWithoutSavingADefaultCharacter) {
    use_runtime_providers({JevOutcome::success, "self_note", {}});
    int saved = 0;
    LiveSessionManager manager({}, [&](const FullSessionId&, auto wake, std::uint64_t) {
        return OpenedSession{.label = "Original", .controller = make_controller(wake),
            .persist_default_character = [&](auto) { ++saved; }};
    });
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open({"lobby", "session"}, 2s)));
    auto session = manager.lookup({"lobby", "session"});
    const auto reply = session->submit(RawCommand{"Note to self: buy milk."}, 2s);
    ASSERT_TRUE(std::holds_alternative<CommandResult>(reply));
    EXPECT_TRUE(std::get<CommandResult>(reply).clear_input);
    EXPECT_EQ(saved, 0);
    const auto continuation = session->submit(RawCommand{"Also buy milk."}, 2s);
    ASSERT_TRUE(std::holds_alternative<CommandResult>(continuation));
    EXPECT_TRUE(std::get<CommandResult>(continuation).clear_input);
    EXPECT_EQ(saved, 0);
    const auto snapshot = std::get<SessionSnapshot>(session->snapshot(2s));
    EXPECT_EQ(snapshot.default_character_id, "-");
    EXPECT_FALSE(snapshot.generation.active);
    ASSERT_EQ(snapshot.transcript.size(), 2u);
    EXPECT_EQ(snapshot.transcript.front().addressed_to, "-");
    EXPECT_EQ(snapshot.transcript.back().addressed_to, "-");
}

TEST_F(JevRouting, ExplicitTargetsSkipJevAndPreserveTheDefaultRecipient) {
    (void)controller->set_default_character_by_id("marcus");
    decision = {JevOutcome::failure, {}, "Jev unavailable"};
    for (const auto* prompt : {"@Guide question", "/mcast @Guide question"}) {
        const auto before = controller->view().transcript.entries.size();
        EXPECT_TRUE(send(prompt).clear_input);
        EXPECT_FALSE(controller->classification_pending());
        EXPECT_TRUE(classified.empty());
        EXPECT_EQ(controller->view().default_character_id, "marcus");
        EXPECT_EQ(workers.size(), 1u);
        ASSERT_GT(controller->view().transcript.entries.size(), before);
        EXPECT_EQ(controller->view().transcript.entries[before].addressed_to, "guide");
        finish();
        ASSERT_GT(controller->view().transcript.entries.size(), before + 1);
        EXPECT_EQ(controller->view().transcript.entries[before + 1].participant_id, "guide");
    }
}

TEST_F(JevRouting, SpecificDecisionsUpdateCurrentTargetAndUndefinedKeepsIt) {
    decision = {JevOutcome::success, "character_2"};
    EXPECT_FALSE(send("Marcus, please answer").clear_input);
    EXPECT_TRUE(controller->is_generating());
    EXPECT_TRUE(controller->view().generation.active);
    EXPECT_FALSE(controller->view().generation.request_id);
    EXPECT_TRUE(controller->view().generation.character_id.empty());
    EXPECT_TRUE(controller->view().transcript.entries.empty());
    EXPECT_FALSE(send("second").clear_input);
    EXPECT_FALSE(controller->start_multicast("reader", "second", {}).input_consumed);
    EXPECT_FALSE(requires_snapshot(controller->set_default_character_by_id("*")));
    EXPECT_FALSE(requires_snapshot(controller->cover_conversation()));
    EXPECT_FALSE(requires_snapshot(controller->uncover_conversation()));
    EXPECT_FALSE(requires_snapshot(controller->delete_turn(1)));
    run_workers();
    (void)controller->receive_events(100);
    EXPECT_EQ(controller->view().default_character_id, "marcus");
    EXPECT_EQ(controller->view().transcript.entries.front().addressed_to, "marcus");
    const auto selected = controller->take_submission_result();
    ASSERT_TRUE(selected);
    EXPECT_TRUE(selected->update.input_consumed);
    EXPECT_EQ(selected->persist_default_character_id, "marcus");
    EXPECT_EQ(store->snapshot()->find_forum("lobby")->default_character_id, "guide");
    finish();
    decision = {JevOutcome::success, "all_characters"};
    (void)send("Guide and Marcus, please answer");
    run_workers(); (void)controller->receive_events(100);
    EXPECT_EQ(controller->view().default_character_id, "*");
    EXPECT_EQ(workers.size(), 2u);
    EXPECT_FALSE(controller->take_submission_result()->persist_default_character_id);
    finish();
    decision = {JevOutcome::success, "undefined"};
    (void)send("Explain further");
    run_workers(); (void)controller->receive_events(100);
    EXPECT_EQ(controller->view().default_character_id, "*");
    EXPECT_EQ(workers.size(), 2u);
    EXPECT_FALSE(controller->take_submission_result()->persist_default_character_id);
    finish();
    EXPECT_EQ(classified.size(), 3u);
    controller.reset();
    controller = make_controller(notifier);
    EXPECT_EQ(controller->view().default_character_id, "guide");
}

TEST_F(JevRouting, RuntimeSavesIdentifiedCharacterForForumAndReopensWithIt) {
    use_runtime_providers({JevOutcome::success, "character_2"});
    const auto opener = [&](const FullSessionId&, auto wake, std::uint64_t) {
        return OpenedSession{.label = "Original",
            .controller = SessionController::from_workspace_for_testing(
                [this] { return store->snapshot(); },
                store->snapshot()->find_forum("lobby")->default_character_id,
                "reader", journal.path(), providers, wake, {}, {}, {"lobby", "session"}),
            .persist_default_character = [&](auto id) {
                (void)store->apply_forum_default_character("lobby", id);
            }};
    };
    {
        LiveSessionManager manager({}, opener);
        ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open({"lobby", "session"}, 2s)));
        auto session = manager.lookup({"lobby", "session"});
        const auto reply = session->submit(RawCommand{"Marcus, please answer"}, 2s);
        ASSERT_TRUE(std::holds_alternative<CommandResult>(reply));
        EXPECT_TRUE(std::get<CommandResult>(reply).clear_input);
        EXPECT_EQ(store->snapshot()->find_forum("lobby")->default_character_id, "marcus");
        const auto snapshot = std::get<SessionSnapshot>(session->snapshot(2s));
        EXPECT_EQ(snapshot.default_character_id, "marcus");
        EXPECT_EQ(snapshot.forum.default_character_id, "marcus");
    }
    LiveSessionManager reopened({}, opener);
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(reopened.open({"lobby", "session"}, 2s)));
    const auto snapshot = std::get<SessionSnapshot>(
        reopened.lookup({"lobby", "session"})->snapshot(2s));
    EXPECT_EQ(snapshot.default_character_id, "marcus");
}

TEST_F(JevRouting, RuntimeDoesNotSaveMarkersFallbacksOrExplicitRecipients) {
    controller.reset();
    for (const auto& result : {JevResult{JevOutcome::success, "all_characters"},
             JevResult{JevOutcome::success, "undefined"},
             JevResult{JevOutcome::failure, {}, "Unavailable"},
             JevResult{JevOutcome::success, "character_2"}}) {
        SCOPED_TRACE(result.choice);
        test::TemporarySessionFile file{"jev_unsaved", {"lobby", "session"}};
        use_runtime_providers(result);
        int saved = 0;
        LiveSessionManager manager({}, [&](const FullSessionId&, auto wake, std::uint64_t) {
            return OpenedSession{.label = "Original",
                .controller = SessionController::from_workspace_for_testing(
                    [this] { return store->snapshot(); }, "guide", "reader", file.path(),
                    providers, wake, {}, {}, {"lobby", "session"}),
                .persist_default_character = [&](auto) { ++saved; }};
        });
        ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open({"lobby", "session"}, 2s)));
        const auto reply = manager.lookup({"lobby", "session"})->submit(
            RawCommand{result.choice == "character_2" ? "@Guide question" : "Question"}, 2s);
        ASSERT_TRUE(std::holds_alternative<CommandResult>(reply));
        EXPECT_TRUE(std::get<CommandResult>(reply).clear_input);
        EXPECT_EQ(saved, 0);
    }
}

TEST_F(JevRouting, RuntimeKeepsIdentifiedCharacterWhenForumSettingsCannotBeSaved) {
    use_runtime_providers({JevOutcome::success, "character_2"});
    LiveSessionManager manager({}, [&](const FullSessionId&, auto wake, std::uint64_t) {
        return OpenedSession{.label = "Original", .controller = make_controller(wake),
            .persist_default_character = [](auto) { throw std::runtime_error("Read-only workspace"); }};
    });
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open({"lobby", "session"}, 2s)));
    auto session = manager.lookup({"lobby", "session"});
    const auto reply = session->submit(RawCommand{"Marcus, please answer"}, 2s);
    ASSERT_TRUE(std::holds_alternative<CommandResult>(reply));
    EXPECT_TRUE(std::get<CommandResult>(reply).clear_input);
    ASSERT_TRUE(std::get<CommandResult>(reply).session.notice);
    EXPECT_NE(std::get<CommandResult>(reply).session.notice->find("not saved"), std::string::npos);
    const auto snapshot = std::get<SessionSnapshot>(session->snapshot(2s));
    EXPECT_EQ(snapshot.default_character_id, "marcus");
    ASSERT_TRUE(snapshot.notice);
    EXPECT_NE(snapshot.notice->find("not saved"), std::string::npos);
    EXPECT_EQ(store->snapshot()->find_forum("lobby")->default_character_id, "guide");
}

TEST_F(JevRouting, SettingsAreCapturedAndFailuresFallbackWithNotice) {
    (void)send("Question");
    auto changed = config; changed.model = "intentional-model";
    store->apply_jev_update(changed);
    decision = {JevOutcome::failure, {}, "timeout"};
    run_workers();
    const auto log_file = fixture.root() / "jev-failure.log";
    initialize_diagnostic_logging(log_file, "warn");
    const auto completed = controller->receive_events(100);
    shutdown_diagnostic_logging();
    std::ifstream log(log_file);
    const std::string diagnostic{std::istreambuf_iterator<char>(log), {}};
    EXPECT_NE(diagnostic.find("using captured fallback target: timeout"), std::string::npos);
    EXPECT_EQ(classified.back().config.model, "typesafe/jev-1.13");
    EXPECT_EQ(completed.update.notice, "Recipient detection failed. Sent to Guide.");
    EXPECT_EQ(controller->take_submission_result()->update.notice, completed.update.notice);
    finish();
    (void)controller->set_default_character_by_id("*");
    decision = {JevOutcome::success, "unknown-option"};
    (void)send("Question again");
    store->apply_jev_update(std::nullopt);
    run_workers();
    EXPECT_EQ(classified.back().config.model, "intentional-model");
    const auto multicast = controller->receive_events(100);
    EXPECT_EQ(multicast.update.notice, "Recipient detection failed. Sent to all characters.");
    finish();
    const auto count = classified.size();
    EXPECT_TRUE(send("No detection").clear_input);
    finish();
    EXPECT_EQ(classified.size(), count);
}

TEST_F(JevRouting, StopExpiryAndShutdownPreventLateDispatch) {
    for (int mode = 0; mode < 3; ++mode) {
        auto submission = std::make_shared<SubmissionState>();
        submission->deadline = std::chrono::steady_clock::now() + 2s;
        (void)controller->submit_prompt("reader", "Question", {}, submission);
        if (mode == 0) (void)controller->request_stop();
        if (mode == 1) {
            submission->cancelled = true;
            (void)controller->receive_events(100);
        }
        if (mode == 2) controller->shutdown();
        decision = {JevOutcome::success, "all_characters"};
        run_workers();
        (void)controller->receive_events(100);
        EXPECT_FALSE(controller->is_generating());
        EXPECT_TRUE(controller->view().transcript.entries.empty());
        EXPECT_EQ(controller->view().default_character_id, "guide");
        EXPECT_TRUE(controller->take_submission_result());
        EXPECT_FALSE(controller->take_submission_result());
    }
    EXPECT_TRUE(classified.empty());
}

TEST_F(JevRouting, ExpiredDeadlineAndInvalidFallbackNeverDispatch) {
    decision = {JevOutcome::success, "self_note"};
    auto submission = std::make_shared<SubmissionState>();
    (void)controller->submit_prompt("reader", "Question", {}, submission);
    run_workers();
    submission->deadline = std::chrono::steady_clock::now() - 1ms;
    (void)controller->receive_events(100);
    EXPECT_EQ(controller->take_submission_result()->outcome, SessionController::SubmissionOutcome::expired);
    EXPECT_TRUE(controller->view().transcript.entries.empty());
    (void)send("Another question");
    decision = {JevOutcome::failure, {}, "missing key"};
    run_workers();
    store->apply_forum_members_and_persona("lobby", std::vector<std::string>{"marcus"}, "reader");
    const auto result = controller->receive_events(100);
    EXPECT_FALSE(controller->is_generating());
    EXPECT_TRUE(controller->view().transcript.entries.empty());
    EXPECT_FALSE(result.update.notice);
    const auto submission_result = controller->take_submission_result();
    ASSERT_TRUE(submission_result);
    EXPECT_EQ(submission_result->outcome, SessionController::SubmissionOutcome::failed);
    ASSERT_TRUE(submission_result->update.notice);
    EXPECT_NE(submission_result->update.notice->find("Choose a target"), std::string::npos);
}

TEST_F(JevRouting, EscapedAddressingAndOneCharacterForumStillClassify) {
    store->apply_forum_members_and_persona("lobby", std::vector<std::string>{"guide"}, "reader");
    (void)send("@@Guide ordinary text");
    run_workers(); (void)controller->receive_events(100);
    ASSERT_EQ(classified.size(), 1u);
    EXPECT_EQ(classified[0].characters.size(), 1u);
    EXPECT_EQ(classified[0].prompt, "@Guide ordinary text");
    finish();
}

TEST(JevProviders, ClosedAdmissionLaunchFailureAndCancellationHaveOneResultAndWake) {
    auto notifier = std::make_shared<test::TestNotifier>();
    Providers closed;
    closed.shutdown();
    auto request = closed.make_jev_request(jev_input(), notifier);
    ASSERT_TRUE(request->try_receive());
    EXPECT_FALSE(request->try_receive());
    Providers failed({}, [](auto) { throw std::runtime_error("launch failed"); });
    request = failed.make_jev_request(jev_input(), notifier);
    EXPECT_EQ(request->try_receive()->outcome, JevOutcome::failure);
    EXPECT_TRUE(failed.shutdown_until(std::chrono::steady_clock::now()));
    std::function<void()> worker;
    int executions = 0;
    Providers delayed({}, [&](auto next) { worker = std::move(next); },
        [&](const auto&, const auto&) { ++executions; return JevResult{JevOutcome::success, "undefined"}; });
    request = delayed.make_jev_request(jev_input(), notifier);
    request->cancel();
    EXPECT_FALSE(delayed.shutdown_until(std::chrono::steady_clock::now()));
    worker(); worker = {};
    EXPECT_EQ(executions, 0);
    EXPECT_EQ(request->try_receive()->outcome, JevOutcome::cancelled);
    EXPECT_TRUE(delayed.shutdown_until(std::chrono::steady_clock::now()));
}

TEST(JevProviders, StartupSharesBudgetAndSubmissionDeadlineCapsIt) {
    auto notifier = std::make_shared<test::TestNotifier>();
    auto input = jev_input();
    input.deadline = std::chrono::steady_clock::now() + 1s;
    const auto deadline = input.deadline;
    Providers providers({}, [](auto worker) { worker(); },
        [&](const auto& actual, const auto&) {
            EXPECT_EQ(actual.deadline, deadline);
            return JevResult{JevOutcome::success, "undefined"};
        });
    auto request = providers.make_jev_request(input, notifier);
    EXPECT_EQ(request->try_receive()->outcome, JevOutcome::success);
    input.deadline = std::chrono::steady_clock::now() - 1ms;
    request = providers.make_jev_request(input, notifier);
    EXPECT_EQ(request->try_receive()->outcome, JevOutcome::failure);
}

TEST(JevProviders, DroppedHandlesAndTerminalResultsDoNotFinishWorkerCleanup) {
    class BlockingNotifier final : public WakeNotifier {
    public:
        std::promise<void> entered;
        std::shared_future<void> released;
        std::thread::id worker;
        void wake() noexcept override {
            if (std::this_thread::get_id() != worker) return;
            entered.set_value(); released.wait();
        }
    };
    auto notifier = std::make_shared<BlockingNotifier>();
    std::promise<void> release;
    notifier->released = release.get_future().share();
    Providers providers({}, {}, [&](const auto&, const auto&) {
        notifier->worker = std::this_thread::get_id();
        return JevResult{JevOutcome::success, "undefined"};
    });
    auto request = providers.make_jev_request(jev_input(), notifier);
    ASSERT_EQ(notifier->entered.get_future().wait_for(2s), std::future_status::ready);
    EXPECT_EQ(request->try_receive()->outcome, JevOutcome::success);
    std::weak_ptr<JevRequest> retained = request;
    request.reset();
    EXPECT_FALSE(retained.expired());
    EXPECT_FALSE(providers.shutdown_until(std::chrono::steady_clock::now()));
    release.set_value();
    providers.shutdown();
}

TEST(JevProviders, ExceptionsAndMissingCredentialsPublishFailureOnce) {
    auto notifier = std::make_shared<test::TestNotifier>();
    Providers providers({}, [](auto worker) { worker(); }, [](const auto&, const auto&) -> JevResult {
        throw std::runtime_error("private exception text");
    });
    auto request = providers.make_jev_request(jev_input(), notifier);
    const auto result = request->try_receive();
    ASSERT_TRUE(result);
    EXPECT_EQ(result->outcome, JevOutcome::failure);
    EXPECT_EQ(result->message.find("private"), std::string::npos);
    EXPECT_FALSE(request->try_receive());
}

TEST_F(JevRouting, RuntimeExpiryAndAbandonmentCancelWithoutFallback) {
    controller.reset();
    for (const bool abandon : {false, true}) {
        std::promise<void> started, release;
        auto released = release.get_future().share();
        providers = std::make_shared<Providers>(ProviderClientFactory{}, ProviderThreadLauncher{},
            [&](const auto&, const auto&) {
                started.set_value(); released.wait();
                return JevResult{JevOutcome::success, "all_characters"};
            });
        LiveSessionManager manager({}, [&](const FullSessionId&, auto wake, std::uint64_t) {
            return OpenedSession{.label = "Original", .controller = make_controller(wake)};
        });
        ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open({"lobby", "session"}, 2s)));
        auto session = manager.lookup({"lobby", "session"});
        auto reply = std::get<std::shared_ptr<CommandReply>>(session->enqueue(RawCommand{"Question"},
            std::chrono::steady_clock::now() + (abandon ? 2s : 100ms)));
        EXPECT_EQ(started.get_future().wait_for(2s), std::future_status::ready);
        if (abandon) reply->abandon();
        else {
            const auto result = reply->wait_for(2s);
            ASSERT_TRUE(result);
            EXPECT_EQ(std::get<ErrorCode>(*result), ErrorCode::command_timeout);
        }
        const auto until = std::chrono::steady_clock::now() + 2s;
        while (!session->idle_for_retirement() && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(1ms);
        EXPECT_TRUE(session->idle_for_retirement());
        release.set_value();
        providers->shutdown();
        const auto snapshot = std::get<SessionSnapshot>(session->snapshot(2s));
        EXPECT_TRUE(snapshot.transcript.empty());
        EXPECT_EQ(snapshot.default_character_id, "guide");
        if (abandon) {
            EXPECT_FALSE(reply->peek());
        }
    }
}

TEST(JevProviders, AdmissionRacingWithShutdownRemainsRegisteredUntilWorkerFinishes) {
    auto notifier = std::make_shared<test::TestNotifier>();
    std::promise<void> launching, release;
    auto released = release.get_future().share();
    std::atomic_int executed{};
    Providers providers({}, [&](auto worker) {
        launching.set_value(); released.wait();
        std::thread(std::move(worker)).detach();
    }, [&](const auto&, const auto&) { ++executed; return JevResult{JevOutcome::success, "undefined"}; });
    auto admitted = std::async(std::launch::async, [&] { return providers.make_jev_request(jev_input(), notifier); });
    EXPECT_EQ(launching.get_future().wait_for(2s), std::future_status::ready);
    EXPECT_FALSE(providers.shutdown_until(std::chrono::steady_clock::now()));
    release.set_value();
    auto request = admitted.get();
    providers.shutdown();
    EXPECT_EQ(executed.load(), 0);
    EXPECT_EQ(request->try_receive()->outcome, JevOutcome::cancelled);
    EXPECT_FALSE(request->try_receive());
}

TEST_F(JevRouting, RuntimeDefersReplyAndStopSettlesItWithoutPersistingMarkers) {
    controller.reset();
    // A real worker blocked before classification; the runtime must remain free for Stop.
    std::promise<void> started, release;
    auto released = release.get_future().share();
    providers = std::make_shared<Providers>(ProviderClientFactory{}, ProviderThreadLauncher{},
        [&](const auto&, const auto&) { started.set_value(); released.wait(); return JevResult{JevOutcome::success, "all_characters"}; });
    int saved = 0;
    LiveSessionManager manager({}, [&](const FullSessionId&, auto wake, std::uint64_t) {
        return OpenedSession{.label = "Original", .controller = make_controller(wake),
            .persist_default_character = [&](auto) { ++saved; }};
    });
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.open({"lobby", "session"}, 2s)));
    auto session = manager.lookup({"lobby", "session"});
    for (const auto* target : {"*", "-", "guide"}) {
        auto result = session->submit(SetDefaultCharacterCommand{target}, 2s);
        EXPECT_TRUE(std::holds_alternative<CommandResult>(result));
    }
    EXPECT_EQ(saved, 1);
    auto result = session->enqueue(RawCommand{"Question"});
    auto reply = std::get<std::shared_ptr<CommandReply>>(result);
    ASSERT_EQ(started.get_future().wait_for(2s), std::future_status::ready);
    EXPECT_FALSE(reply->peek());
    EXPECT_FALSE(session->idle_for_retirement());
    auto snapshot = std::get<SessionSnapshot>(session->snapshot(2s));
    EXPECT_TRUE(snapshot.generation.active);
    EXPECT_FALSE(snapshot.generation.request_id);
    EXPECT_TRUE(std::holds_alternative<CommandResult>(session->submit(StopCommand{}, 2s)));
    auto completed = reply->wait_for(2s);
    ASSERT_TRUE(completed);
    EXPECT_FALSE(std::get<CommandResult>(*completed).clear_input);
    release.set_value();
    providers->shutdown();
    snapshot = std::get<SessionSnapshot>(session->snapshot(2s));
    EXPECT_TRUE(snapshot.transcript.empty());
    EXPECT_EQ(snapshot.default_character_id, "guide");
}

TEST_F(JevRouting, NavigationKeepsPendingSessionAndCompletionWakesOwnerExactlyOnce) {
    controller.reset();
    std::promise<void> started, release;
    auto released = release.get_future().share();
    providers = std::make_shared<Providers>(ProviderClientFactory{}, ProviderThreadLauncher{},
        [&](const auto&, const auto&) { started.set_value(); released.wait(); return JevResult{JevOutcome::success, "character_2"}; });
    test::TemporarySessionFile other{"jev_other", {"lobby", "other"}};
    LiveSessionManager manager({}, [&](const FullSessionId& identity, auto wake, std::uint64_t) {
        return OpenedSession{.label = identity.session_id,
            .controller = SessionController::from_workspace_for_testing(
                [this] { return store->snapshot(); }, "guide", "reader",
                identity.session_id == "session" ? journal.path() : other.path(),
                providers, wake, {}, {}, identity)};
    });
    ASSERT_TRUE(std::holds_alternative<LiveSessionReady>(manager.select({"lobby", "session"}, 2s)));
    auto session = manager.lookup({"lobby", "session"});
    auto reply = std::get<std::shared_ptr<CommandReply>>(session->enqueue(RawCommand{"Original prompt"}));
    EXPECT_EQ(started.get_future().wait_for(2s), std::future_status::ready);
    EXPECT_TRUE(std::holds_alternative<LiveSessionReady>(manager.select({"lobby", "other"}, 2s)));
    EXPECT_FALSE(reply->peek());
    EXPECT_EQ(session->lifecycle(), LiveSessionState::running);
    release.set_value();
    const auto result = reply->wait_for(2s);
    ASSERT_TRUE(result);
    EXPECT_TRUE(std::get<CommandResult>(*result).clear_input);
    const auto other_snapshot = std::get<SessionSnapshot>(manager.lookup({"lobby", "other"})->snapshot(2s));
    EXPECT_TRUE(other_snapshot.transcript.empty());
    // Retirement may now complete, but the accepted prompt was durably saved in its origin.
    providers->shutdown();
    const auto restored = load_session_state(journal.path());
    EXPECT_EQ(std::count_if(restored.entries.begin(), restored.entries.end(), [](const auto& entry) {
        return entry.kind == EntryKind::human && entry.text == "Original prompt" && entry.addressed_to == "marcus";
    }), 1);
}


TEST(JevProtocol, ParsesResearchAnswersIndependentlyAndLogsInvalidDecisions) {
    test::TestWorkspace fixture;
    const auto path = fixture.root() / "decisions.log";
    initialize_diagnostic_logging(path, "warn");
    auto response = nlohmann::json::parse(R"({"answers":{
        "recipient":{"type":"choice","choice":"character_2"},
        "search_required":{"type":"choice","choice":"yes"},
        "page_read_required":{"type":"choice","choice":"yes"},
        "actual_data_required":{"type":"choice","choice":"yes"}}})");
    const auto check = [&](bool search, bool read, bool data, JevOutcome outcome) {
        const auto result = parse_jev_result(response, jev_input());
        EXPECT_EQ(result.outcome, outcome);
        if (outcome == JevOutcome::success) EXPECT_EQ(result.choice, "character_2");
        EXPECT_EQ(result.research_needs.search, search);
        EXPECT_EQ(result.research_needs.page_read, read);
        EXPECT_EQ(result.research_needs.actual_data, data);
    };
    check(true, true, true, JevOutcome::success);
    response["answers"]["search_required"]["choice"] = "no";
    check(false, true, true, JevOutcome::success);
    for (const auto& answer : {nlohmann::json(nullptr), nlohmann::json::object(),
        nlohmann::json{{"type", "noul"}, {"choice", "yes"}},
        nlohmann::json{{"type", "choice"}, {"choice", true}},
        nlohmann::json{{"type", "choice"}, {"choice", "Yes"}}}) {
        response["answers"]["page_read_required"] = answer;
        check(false, false, true, JevOutcome::success);
    }
    response["answers"].erase("search_required");
    response["answers"]["recipient"]["choice"] = "invalid";
    check(false, false, true, JevOutcome::failure);
    response = nullptr;
    check(false, false, false, JevOutcome::failure);
    shutdown_diagnostic_logging();
    std::ifstream file(path);
    const std::string log{std::istreambuf_iterator<char>(file), {}};
    for (const auto* name : {"search_required", "page_read_required", "actual_data_required"})
        EXPECT_NE(log.find(std::string("Invalid or missing Jev research decision: ") + name), std::string::npos);
}

TEST(JevContext, SelectsLastUncoveredHumanAndCompletedNonemptyReplies) {
    std::vector<TranscriptEntry> entries{
        test::human_entry(1, {"user", "User"}, {"a", "A"}, "Older"),
        make_character_entry(2, "a", "A", "Older answer", EntryStatus::complete),
        test::human_entry(3, {"user", "User"}, {"*", "All"}, "Compare"),
        make_character_entry(4, "a", "A", "First", EntryStatus::complete),
        make_character_entry(5, "b", "B", "Second", EntryStatus::complete),
        make_character_entry(6, "a", "A", "Partial", EntryStatus::failed),
        make_character_entry(7, "b", "B", "Stopped", EntryStatus::cancelled),
        make_character_entry(8, "a", "A", "", EntryStatus::complete),
        make_notice_entry(9, "Notice"), make_error_entry(10, "Error"),
        make_character_entry(11, "a", "A", "Open", EntryStatus::complete),
    };
    TranscriptView view{.entries = entries, .open_entry_id = 11, .covered_until = 3};
    const auto turn = jev_previous_turn(view);
    ASSERT_TRUE(turn);
    EXPECT_EQ(turn->human.speaker, "User");
    EXPECT_EQ(turn->human.text, "Compare");
    ASSERT_EQ(turn->replies.size(), 2u);
    EXPECT_EQ(turn->replies[0].speaker, "A");
    EXPECT_EQ(turn->replies[0].text, "First");
    EXPECT_EQ(turn->replies[1].speaker, "B");
    EXPECT_EQ(turn->replies[1].text, "Second");
    view.covered_until = 4;
    EXPECT_FALSE(jev_previous_turn(view));
    view.covered_until = {};
    auto input = jev_input();
    input.previous_turn = jev_previous_turn(view);
    const auto selected = make_jev_body(input)["state"];
    entries[0].text = std::string(100000, 'x');
    input.previous_turn = jev_previous_turn(view);
    EXPECT_EQ(make_jev_body(input)["state"], selected);
    entries.push_back(test::human_entry(12, {"user", "User"}, {"-", "-"}, "Note"));
    view.entries = entries;
    input.previous_turn = jev_previous_turn(view);
    EXPECT_EQ(input.previous_turn->human.text, "Note");
    EXPECT_TRUE(input.previous_turn->replies.empty());
    EXPECT_FALSE(jev_previous_turn({}));
}

TEST(JevContext, BuilderLimitsEveryTextAtCompleteUtf8CodePoints) {
    auto input = jev_input();
    input.prompt = std::string(5000, 'p');
    for (const auto& codepoint : {std::string("x"), std::string("é"), std::string("€"), std::string("😀")}) {
        for (std::size_t remaining = 0; remaining <= codepoint.size(); ++remaining) {
            const auto prefix = std::string(2048 - remaining, 'a');
            const auto text = prefix + codepoint + "tail";
            input.previous_turn = JevPreviousTurn{{"User", text}, {{"Seneca", text}, {"Marcus", text}}};
            const auto state = make_jev_body(input)["state"];
            const auto expected = prefix + (remaining == codepoint.size() ? codepoint : "");
            const auto limited = text.substr(0, expected.size());
            EXPECT_EQ(state["previous_turn"]["human"]["text"], limited);
            ASSERT_EQ(state["previous_turn"]["replies"].size(), 2u);
            for (const auto& reply : state["previous_turn"]["replies"])
                EXPECT_EQ(reply["text"], limited);
            EXPECT_EQ(state["prompt"], input.prompt);
            EXPECT_EQ(state["previous_turn"]["human"]["speaker"], "User");
        }
    }
}

class JevCaptureBackend final : public ModelBackend {
public:
    explicit JevCaptureBackend(std::vector<GenerationRequest>& inputs) : inputs_(inputs) {}
    RequestPayload prepare(const GenerationRequest& input) override {
        inputs_.push_back(input);
        return {};
    }
    GenerationResult perform(RequestPayload, const GenerationDeltaSink& sink,
        const std::atomic_bool&) override {
        sink({GenerationDeltaKind::answer, "Answer"});
        return {};
    }
private:
    std::vector<GenerationRequest>& inputs_;
};

TEST_F(JevRouting, SharesResearchNeedsAcrossImplicitChildrenAndPreservesBypasses) {
    controller.reset();
    providers->shutdown();
    auto& inputs = generation_inputs;
    providers = std::make_shared<Providers>(
        [this](auto) { return std::make_unique<JevCaptureBackend>(generation_inputs); },
        [this](auto worker) { workers.push_back(std::move(worker)); },
        [this](const auto& input, const auto&) { classified.push_back(input); return decision; });
    controller = make_controller(notifier);
    decision = {JevOutcome::success, "all_characters", {}, {true, true, true}};
    (void)send("Compare current prices");
    finish();
    ASSERT_EQ(classified.size(), 1u);
    EXPECT_FALSE(classified[0].previous_turn);
    ASSERT_EQ(inputs.size(), 2u);
    for (const auto& input : inputs) {
        EXPECT_TRUE(input.research_needs.search);
        EXPECT_TRUE(input.research_needs.page_read);
        EXPECT_TRUE(input.research_needs.actual_data);
        EXPECT_EQ(input.run.prompt_text, "Compare current prices");
    }
    for (const auto& entry : controller->view().transcript.entries)
        if (entry.kind == EntryKind::human) EXPECT_EQ(entry.text, "Compare current prices");
    decision = {JevOutcome::failure, {}, "Invalid recipient decision", {false, true, true}};
    (void)send("Which is cheapest?");
    finish();
    ASSERT_EQ(classified.size(), 2u);
    ASSERT_TRUE(classified.back().previous_turn);
    EXPECT_EQ(classified.back().previous_turn->human.text, "Compare current prices");
    EXPECT_FALSE(inputs.back().research_needs.search);
    EXPECT_TRUE(inputs.back().research_needs.page_read);
    EXPECT_TRUE(inputs.back().research_needs.actual_data);
    decision = {JevOutcome::success, "self_note", {}, {true, true, true}};
    const auto before_note = inputs.size();
    (void)send("Note to self: compare prices");
    finish();
    EXPECT_EQ(inputs.size(), before_note);
    const auto before_bypasses = classified.size();
    for (const auto* prompt : {"@Guide question", "/mcast question", "/mcast @Marcus question"}) {
        const auto before = inputs.size();
        (void)send(prompt);
        finish();
        ASSERT_GT(inputs.size(), before);
        for (auto i = before; i < inputs.size(); ++i) {
            EXPECT_FALSE(inputs[i].research_needs.search);
            EXPECT_FALSE(inputs[i].research_needs.page_read);
            EXPECT_FALSE(inputs[i].research_needs.actual_data);
        }
    }
    EXPECT_EQ(classified.size(), before_bypasses);
    (void)controller->set_default_character_by_id("guide");
    store->apply_jev_update(std::nullopt);
    const auto before_disabled = inputs.size();
    (void)send("Jev disabled");
    finish();
    ASSERT_GT(inputs.size(), before_disabled);
    EXPECT_EQ(classified.size(), before_bypasses);
    EXPECT_FALSE(inputs.back().research_needs.search);
    EXPECT_FALSE(inputs.back().research_needs.page_read);
    EXPECT_FALSE(inputs.back().research_needs.actual_data);
}

} // namespace
} // namespace cha
