#include "providers/jev.h"
#include "providers/providers.h"
#include "providers/api_key_store.h"
#include "runtime/live_session_manager.h"
#include "runtime/text_input.h"
#include "support/test_live_session.h"
#include "support/test_notifier.h"
#include "support/mock_http_server.h"
#include "support/test_workspace.h"
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
    return {{.api_key_id = "key-1"}, "Marcus, do you agree with Seneca?",
        {{"character_1", "seneca", "Seneca"}, {"character_2", "marcus", "Marcus"}}};
}

TEST(JevProtocol, SendsOnlyPromptAndOptionsAndValidatesExactChoice) {
    auto input = jev_input();
    const auto body = make_jev_body(input);
    EXPECT_EQ(body["model"], "typesafe/jev-1.13");
    EXPECT_EQ(body["state"].size(), 1u);
    EXPECT_EQ(body["state"]["prompt"], input.prompt);
    EXPECT_EQ(body["questions"]["recipient"]["criteria"].size(), 4u);
    EXPECT_EQ(body["questions"]["web_search"]["type"], "choice");
    EXPECT_EQ(body["questions"]["web_search"]["criteria"].size(), 3u);
    const auto response = [](std::string recipient, std::string search = "no_search") {
        return nlohmann::json{{"answers", {
            {"recipient", {{"type", "choice"}, {"choice", recipient}}},
            {"web_search", {{"type", "choice"}, {"choice", search}}}}}};
    };
    for (const std::string choice : {"character_1", "character_2", "undefined", "all_characters"}) {
        EXPECT_EQ(parse_jev_result(response(choice), input).choice, choice);
    }
    for (const std::string choice : {"Seneca", " character_1", "character_1 extra", "character_9"}) {
        EXPECT_EQ(parse_jev_result(response(choice), input).outcome, JevOutcome::failure);
    }
    for (const auto& [search, expected] : {
        std::pair{"no_search", JevSearch::none},
        std::pair{"search_direct", JevSearch::direct},
        std::pair{"search_rewrite", JevSearch::rewrite}}) {
        const auto result = parse_jev_result(response("character_1", search), input);
        EXPECT_EQ(result.outcome, JevOutcome::success);
        EXPECT_EQ(result.search_choice, expected);
    }
    for (const auto& missing_search : {
        response("character_1", "unknown"),
        nlohmann::json{{"answers", {{"recipient", {{"type", "choice"}, {"choice", "character_1"}}}}}},
        nlohmann::json{{"answers", {
            {"recipient", {{"type", "choice"}, {"choice", "character_1"}}},
            {"web_search", {{"type", "text"}, {"choice", "search_direct"}}}}}}}) {
        const auto result = parse_jev_result(missing_search, input);
        EXPECT_EQ(result.outcome, JevOutcome::success);
        EXPECT_EQ(result.choice, "character_1");
        EXPECT_FALSE(result.search_choice);
    }
    const auto bad_recipient = parse_jev_result(response("character_9", "search_direct"), input);
    EXPECT_EQ(bad_recipient.outcome, JevOutcome::failure);
    EXPECT_EQ(bad_recipient.search_choice, JevSearch::direct);
    EXPECT_EQ(parse_jev_result({{"answers", {{"recipient", {{"type", "text"}, {"choice", "character_1"}}}}}}, input).outcome, JevOutcome::failure);
    EXPECT_EQ(parse_jev_result(nullptr, input).outcome, JevOutcome::failure);
    input.characters = {{"character_1", "a", "Undefined"}, {"character_2", "b", "Undefined"}};
    EXPECT_EQ(make_jev_body(input)["questions"]["recipient"]["criteria"].size(), 4u);
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
    const std::string body = R"({"answers":{"recipient":{"type":"choice","choice":"character_2"},"web_search":{"type":"choice","choice":"search_direct"}}})";
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
    EXPECT_EQ(result.search_choice, JevSearch::direct);
    ASSERT_EQ(server.requests().size(), 1u);
    EXPECT_NE(server.requests()[0].find("POST /api/alpha/decisions "), std::string::npos);
    EXPECT_NE(server.requests()[0].find("Authorization: Bearer test-secret"), std::string::npos);
    EXPECT_NE(server.requests()[0].find("typesafe/jev-1.13"), std::string::npos);
}

class JevRouting : public ::testing::Test {
protected:
    test::TestWorkspace fixture;
    test::TemporarySessionFile journal{"jev", {"lobby", "session"}};
    std::unique_ptr<WorkspaceConfigStore> store;
    std::shared_ptr<test::TestNotifier> notifier = std::make_shared<test::TestNotifier>();
    std::vector<std::function<void()>> workers;
    std::vector<JevRequestInput> classified;
    JevResult decision{JevOutcome::success, "undefined"};
    std::shared_ptr<Providers> providers;
    std::unique_ptr<SessionController> controller;
    WorkspaceJev config;
    void SetUp() override {
        fixture.add_character("marcus", "Marcus");
        const auto member = fixture.root() / "forums/lobby/members/marcus";
        std::filesystem::create_directories(member);
        std::ofstream(member / "character.toml") << "# member\n";
        store = WorkspaceConfigStore::open(test::import_test_database(fixture.root()));
        ApiKeyStore keys(*store);
        config.api_key_id = keys.create("OpenRouter", "test-secret").id;
        store->apply_jev_update(config);
        providers = std::make_shared<Providers>(ProviderClientFactory{},
            [this](auto worker) { workers.push_back(std::move(worker)); },
            [this](const auto& input, const auto&) { classified.push_back(input); return decision; });
        controller = make_controller(notifier);
    }
    std::unique_ptr<SessionController> make_controller(std::shared_ptr<WakeNotifier> wake) {
        return SessionController::from_workspace_for_testing(
            [this] { return store->snapshot(); }, "guide", "reader", journal.path(),
            providers, std::move(wake), {}, {}, {"lobby", "session"});
    }
    void run_workers() {
        auto pending = std::exchange(workers, {});
        for (auto& worker : pending) worker();
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

TEST_F(JevRouting, ClassifiesModelPromptsButSkipsEmptyAndSelfNotes) {
    for (const std::string text : {"", " \t\n", "\n"}) {
        EXPECT_FALSE(send(text).clear_input);
        EXPECT_FALSE(controller->submit_prompt("reader", text).input_consumed);
    }
    EXPECT_TRUE(workers.empty());
    EXPECT_TRUE(controller->view().transcript.entries.empty());
    (void)controller->set_default_character_by_id("-");
    EXPECT_TRUE(send("Marcus, please answer").clear_input);
    EXPECT_TRUE(workers.empty());
    EXPECT_FALSE(send("@Guide Explicit override").clear_input);
    run_workers(); (void)controller->receive_events(100);
    ASSERT_EQ(classified.size(), 1u);
    EXPECT_EQ(classified.back().prompt, "Explicit override");
    finish();
    EXPECT_EQ(controller->view().transcript.entries[1].addressed_to, "guide");
    EXPECT_EQ(controller->view().default_character_id, "-");
    (void)controller->set_default_character_by_id("*");
    EXPECT_EQ(controller->view().default_character_id, "*");
    EXPECT_TRUE(send("@- Private note").clear_input);
    (void)send("/mcast @Guide only Guide");
    EXPECT_TRUE(controller->classification_pending());
    run_workers(); (void)controller->receive_events(100);
    ASSERT_EQ(classified.size(), 2u);
    EXPECT_EQ(classified.back().prompt, "only Guide");
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
    EXPECT_EQ(classified.size(), 2u);
}

TEST_F(JevRouting, ExplicitTargetsIgnoreJevRecipientDecisionsAndFailures) {
    (void)controller->set_default_character_by_id("marcus");
    decision = {JevOutcome::success, "all_characters", {}, JevSearch::direct};
    for (const auto* prompt : {"@Guide question", "/mcast @Guide question"}) {
        const auto before = controller->view().transcript.entries.size();
        (void)send(prompt);
        EXPECT_TRUE(controller->classification_pending());
        run_workers();
        (void)controller->receive_events(100);
        const auto result = controller->take_submission_result();
        ASSERT_TRUE(result);
        EXPECT_EQ(result->outcome, SessionController::SubmissionOutcome::accepted);
        EXPECT_TRUE(result->update.input_consumed);
        EXPECT_EQ(controller->view().default_character_id, "marcus");
        EXPECT_EQ(workers.size(), 1u);
        ASSERT_GT(controller->view().transcript.entries.size(), before);
        EXPECT_EQ(controller->view().transcript.entries[before].addressed_to, "guide");
        finish();
        ASSERT_GT(controller->view().transcript.entries.size(), before + 1);
        EXPECT_EQ(controller->view().transcript.entries[before + 1].participant_id, "guide");
    }

    decision = {JevOutcome::failure, {}, "Jev unavailable"};
    (void)send("@Guide question after failure");
    run_workers();
    (void)controller->receive_events(100);
    const auto failure = controller->take_submission_result();
    ASSERT_TRUE(failure);
    EXPECT_EQ(failure->outcome, SessionController::SubmissionOutcome::accepted);
    EXPECT_TRUE(failure->update.input_consumed);
    EXPECT_TRUE(!failure->update.notice || failure->update.notice->empty());
    EXPECT_EQ(controller->view().default_character_id, "marcus");
    EXPECT_EQ(workers.size(), 1u);
    EXPECT_EQ(controller->view().transcript.entries.back().addressed_to, "guide");
    finish();

    decision = {JevOutcome::success, "all_characters", {}, JevSearch::direct};
    const auto before = controller->view().transcript.entries.size();
    (void)send("@Guide question after removal");
    store->apply_forum_members_and_persona("lobby", std::vector<std::string>{"marcus"}, "reader");
    run_workers();
    (void)controller->receive_events(100);
    const auto removed = controller->take_submission_result();
    ASSERT_TRUE(removed);
    EXPECT_EQ(removed->outcome, SessionController::SubmissionOutcome::failed);
    ASSERT_TRUE(removed->update.notice);
    EXPECT_NE(removed->update.notice->find("no longer in this forum"), std::string::npos);
    EXPECT_EQ(controller->view().default_character_id, "marcus");
    EXPECT_EQ(controller->view().transcript.entries.size(), before);
    EXPECT_TRUE(workers.empty());
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
    ASSERT_TRUE(controller->take_submission_result()->update.input_consumed);
    EXPECT_EQ(store->snapshot()->find_forum("lobby")->default_character_id, "guide");
    finish();
    decision = {JevOutcome::success, "all_characters"};
    (void)send("Guide and Marcus, please answer");
    run_workers(); (void)controller->receive_events(100);
    EXPECT_EQ(controller->view().default_character_id, "*");
    EXPECT_EQ(workers.size(), 2u);
    finish();
    decision = {JevOutcome::success, "undefined"};
    (void)send("Explain further");
    run_workers(); (void)controller->receive_events(100);
    EXPECT_EQ(controller->view().default_character_id, "*");
    EXPECT_EQ(workers.size(), 2u);
    finish();
    EXPECT_EQ(classified.size(), 3u);
    controller.reset();
    controller = make_controller(notifier);
    EXPECT_EQ(controller->view().default_character_id, "guide");
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
        LiveSessionManager manager({}, [&](const FullSessionId&, auto wake) {
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
        if (abandon) EXPECT_FALSE(reply->peek());
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
    LiveSessionManager manager({}, [&](const FullSessionId&, auto wake) {
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
    LiveSessionManager manager({}, [&](const FullSessionId& identity, auto wake) {
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

} // namespace
} // namespace cha
