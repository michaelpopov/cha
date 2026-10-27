#include "app/application.h"
#include "providers/api_key_store.h"
#include "util/base64.h"
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
#include <future>
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

#if defined(__APPLE__) || defined(_WIN32)
const std::string audio_batch = "batches/" + std::string(32, 'b');
const std::string audio_file = "files/" + std::string(32, 'a');
nlohmann::json audio_operation(bool done = true) {
    nlohmann::json result{{"name", audio_batch}, {"done", done}, {"metadata", {{"state", "BATCH_STATE_RUNNING"}}}};
    if (done) result["response"] = {{"responsesFile", audio_file}};
    return result;
}
std::string audio_results() {
    std::string wav;
    const auto number = [&](unsigned value, int bytes) {
        for (int i = 0; i < bytes; ++i) wav += static_cast<char>((value >> (8 * i)) & 255);
    };
    wav += "RIFF"; number(4836, 4); wav += "WAVEfmt "; number(16, 4);
    number(1, 2); number(1, 2); number(24000, 4); number(48000, 4); number(2, 2); number(16, 2);
    wav += "data"; number(4800, 4); wav.append(4800, '\0');
    return nlohmann::json{{"key", "0"}, {"response", {{"candidates", nlohmann::json::array({
        {{"content", {{"parts", nlohmann::json::array({{{"inlineData", {{"mimeType", "audio/wav"},
            {"data", encode_base64(wav)}}}}})}}}}})}}}}.dump() + '\n';
}
std::string audio_json(const nlohmann::json& value) { return http_response("application/json", value.dump()); }
std::string audio_http_error(int status) {
    return "HTTP/1.1 " + std::to_string(status) + " Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
}

// Release test barriers and join before their captured local state is destroyed,
// including when an ASSERT returns early.
struct AudioWorkerCleanup {
    std::unique_ptr<Application>& application;
    std::function<void()> release;
    ~AudioWorkerCleanup() {
        if (!release) return;
        application->request_shutdown();
        release();
        EXPECT_TRUE(application->join_shutdown(2s));
    }
};

void release_audio_gate(std::promise<void>& gate) {
    try { gate.set_value(); } catch (const std::future_error&) { /* Already released. */ }
}

class NoteAudioBackground : public testing::Test {
protected:
    void SetUp() override {
        workspace.add_character("writer", "Writer");
        std::filesystem::create_directories(workspace.root() / "forums/lobby/members/writer");
        std::ofstream(workspace.root() / "forums/lobby/members/writer/character.toml") << "# member\n";
        path = test::import_test_database(workspace.root());
        RuntimeSettings settings;
        settings.note_audio_wait = [this](int seconds) {
            std::size_t index;
            { const std::lock_guard lock(waits_mutex); waits.push_back(seconds); index = waits.size(); }
            if (on_wait) on_wait(index);
        };
        settings.note_audio_before_launch = [this](auto id) { if (on_launch) on_launch(id); };
        settings.note_audio_worker_started = [this](auto id, auto cancel) { if (on_worker) on_worker(id, std::move(cancel)); };
        auto command = make_command(workspace, path);
        command.test_shutdown_grace_ms = 50;
        application = Application::open(command, {}, settings);
        epoch = application->context_epoch();
        note = application->create_note_item({}, NoteItemKind::note, "Chapter", epoch);
        (void)application->set_note_read_only(note.id, true, epoch);
        key_id = application->create_api_key({.display_name = "Audio", .value = "old-key"}, epoch).id;
        initialize_diagnostic_logging(workspace.root() / "audio.log", "warn");
    }
    void TearDown() override { application.reset(); shutdown_diagnostic_logging(); }
    std::string log_text() {
        std::ifstream input(workspace.root() / "audio.log");
        return {std::istreambuf_iterator<char>(input), {}};
    }
    void configure(int port) {
        auto settings = application->get_notes_settings(epoch);
        settings.audio_url = "http://127.0.0.1:" + std::to_string(port);
        settings.audio_api_key = key_id;
        (void)application->save_notes_settings(settings, epoch);
    }
    void activate() {
        application->set_note_audio_changed([this](auto id, auto context) {
            completed_id = id; completed_epoch = context; ++completions;
        });
    }
    bool finished(std::int64_t id = 0) {
        if (!id) id = note.id;
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!application->background_note_audio_status(id, epoch).at("pending").get<bool>()) return true;
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }
    void fail_maintenance() {
        (void)application->create_vault({.display_name = "Other"}, epoch);
        const auto session = application->create_session("lobby", "Blocked", epoch);
        ASSERT_TRUE(std::holds_alternative<OpenSessionSuccess>(application->open_session("lobby", session.id, epoch)));
        std::optional<WorkspaceConfigStore::MaintenanceGuard> edit(application->store().reserve_maintenance());
        const auto changing = application->submit_async("lobby", session.id, SetDefaultCharacterCommand{"writer"}, epoch);
        ASSERT_TRUE(std::holds_alternative<std::shared_ptr<CommandReply>>(changing));
        auto switching = std::async(std::launch::async, [&] {
            try { (void)application->switch_vault("Other", {}, epoch); return false; }
            catch (const std::runtime_error&) { return true; }
        });
        const auto status = switching.wait_for(500ms);
        edit.reset();
        EXPECT_EQ(status, std::future_status::ready);
        EXPECT_TRUE(switching.get());
        EXPECT_GT(application->context_epoch(), epoch);
        epoch = application->context_epoch();
        EXPECT_EQ(application->state(), ApplicationState::running);
    }
    NoteAudioAttempt saved_attempt(int port, bool registered = true) {
        // Seed a recovered row completely before enabling its native worker.
        application->set_note_audio_changed({});
        const auto attempt = *application->store().accept_note_audio(note.id, 1,
            "http://127.0.0.1:" + std::to_string(port), key_id, [](const auto&) {});
        if (registered) EXPECT_TRUE(application->store().register_note_audio(attempt.id, audio_batch, "Kore"));
        return attempt;
    }
    test::TestWorkspace workspace;
    std::filesystem::path path;
    std::unique_ptr<Application> application;
    std::uint64_t epoch{};
    NoteItem note;
    std::string key_id;
    std::mutex waits_mutex;
    std::vector<int> waits;
    std::vector<int> waits_snapshot() { const std::lock_guard lock(waits_mutex); return waits; }
    std::function<void(std::int64_t)> on_launch;
    std::function<void(std::int64_t, std::shared_ptr<std::atomic_bool>)> on_worker;
    std::function<void(std::size_t)> on_wait;
    std::atomic_int completions{};
    std::atomic_int64_t completed_id{};
    std::atomic_uint64_t completed_epoch{};
};

TEST_F(NoteAudioBackground, AcceptanceReturnsDuringUploadAndCommitsOneCompleteOutcome) {
    MockHttpServer upload({audio_json({{"file", {{"name", audio_file}}}})});
    MockHttpServer server({"HTTP/1.1 200 OK\r\nx-goog-upload-url: http://127.0.0.1:"
        + std::to_string(upload.port()) + "/secret\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        audio_json(audio_operation()), http_response("application/jsonl", audio_results())});
    server.pause_before_response(1);
    upload.start(); server.start(); configure(server.port()); activate();
    const auto accepted = application->start_background_note_audio(note.id, "Kore", {"Paragraph"}, epoch);
    EXPECT_GT(accepted.at("attempt_id").get<int>(), 0);
    ASSERT_TRUE(server.wait_for_requests(1, 2s));
    EXPECT_EQ(application->background_note_audio_status(note.id, epoch),
        (nlohmann::json{{"pending", true}, {"attempt_id", accepted["attempt_id"]}}));
    EXPECT_THROW(application->start_background_note_audio(note.id, "Kore", {"Paragraph"}, epoch), ApplicationError);
    EXPECT_TRUE(application->list_events(epoch).empty());
    server.resume_responses();
    ASSERT_TRUE(finished());
    upload.join(); server.join();
    EXPECT_EQ(server.requests().size(), 3u);
    EXPECT_EQ(application->get_note_audio_info(note.id, epoch)["count"], 1);
    const auto events = application->list_events(epoch);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].severity, EventSeverity::success);
    EXPECT_EQ(events[1].severity, EventSeverity::info);
    EXPECT_EQ(completions.load(), 1);
    EXPECT_EQ(completed_id.load(), note.id);
    EXPECT_EQ(completed_epoch.load(), epoch);
    EXPECT_EQ(application->background_note_audio_status(note.id, epoch), (nlohmann::json{{"pending", false}}));
}

TEST_F(NoteAudioBackground, UploadFailuresRetainAcceptanceUntilSubmissionSucceeds) {
    MockHttpServer upload({audio_json({{"file", {{"name", audio_file}}}})});
    MockHttpServer server({"", audio_http_error(503), audio_http_error(401),
        "HTTP/1.1 200 OK\r\nx-goog-upload-url: http://127.0.0.1:"
            + std::to_string(upload.port()) + "/secret\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        audio_json(audio_operation()), http_response("application/jsonl", audio_results())});
    on_wait = [&](auto index) {
        const auto attempt = application->store().get_note_audio_attempt_for_note(note.id);
        ASSERT_TRUE(attempt);
        EXPECT_FALSE(attempt->batch_id);
        EXPECT_TRUE(application->background_note_audio_status(note.id, epoch)["pending"]);
        EXPECT_EQ(application->get_note_audio_info(note.id, epoch)["count"], 0);
        if (index == 3) application->api_keys().replace(key_id, "new-key");
    };
    configure(server.port()); activate(); upload.start(); server.start();
    (void)application->start_background_note_audio(note.id, "Kore", {"Paragraph"}, epoch);
    ASSERT_TRUE(finished()); upload.join(); server.join();
    EXPECT_EQ(waits_snapshot(), (std::vector<int>{10, 20, 40}));
    ASSERT_EQ(server.requests().size(), 6u);
    for (std::size_t i = 0; i < 4; ++i)
        EXPECT_TRUE(server.requests()[i].starts_with("POST /upload/v1beta/files"));
    EXPECT_NE(server.requests()[3].find("new-key"), std::string::npos);
    EXPECT_TRUE(server.requests()[4].starts_with("POST /v1beta/models/"));
    ASSERT_EQ(upload.requests().size(), 1u);
    EXPECT_EQ(upload.requests()[0].find("x-goog-api-key"), std::string::npos);
    const auto events = application->list_events(epoch);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].severity, EventSeverity::success);
    EXPECT_EQ(events[1].severity, EventSeverity::info);
    EXPECT_EQ(application->get_note_audio_info(note.id, epoch)["count"], 1);
    EXPECT_EQ(completions.load(), 1);
    EXPECT_EQ(log_text().find("new-key"), std::string::npos);
}

TEST_F(NoteAudioBackground, FailedFinalizationRestartsUploadBeforeCreatingOneBatch) {
    MockHttpServer upload({audio_http_error(503), "", audio_json({{"file", {{"name", audio_file}}}})});
    const auto upload_start = "HTTP/1.1 200 OK\r\nx-goog-upload-url: http://127.0.0.1:"
        + std::to_string(upload.port()) + "/secret\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    MockHttpServer server({upload_start, upload_start, upload_start,
        audio_json(audio_operation()), http_response("application/jsonl", audio_results())});
    configure(server.port()); activate(); upload.start(); server.start();
    (void)application->start_background_note_audio(note.id, "Kore", {"Paragraph"}, epoch);
    ASSERT_TRUE(finished()); upload.join(); server.join();
    EXPECT_EQ(waits_snapshot(), (std::vector<int>{10, 20}));
    ASSERT_EQ(server.requests().size(), 5u);
    EXPECT_TRUE(server.requests()[3].starts_with("POST /v1beta/models/"));
    ASSERT_EQ(upload.requests().size(), 3u);
    for (const auto& request : upload.requests()) {
        EXPECT_EQ(request_body(request), request_body(upload.requests()[0]));
        EXPECT_EQ(request.find("x-goog-api-key"), std::string::npos);
    }
    EXPECT_EQ(application->list_events(epoch).size(), 2u);
    EXPECT_EQ(application->get_note_audio_info(note.id, epoch)["count"], 1);
    EXPECT_EQ(completions.load(), 1);
}

TEST_F(NoteAudioBackground, RejectedCreationRetriesTheSameFileWithoutUploadingAgain) {
    MockHttpServer upload({audio_json({{"file", {{"name", audio_file}}}})});
    MockHttpServer server({"HTTP/1.1 200 OK\r\nx-goog-upload-url: http://127.0.0.1:"
        + std::to_string(upload.port()) + "/secret\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        audio_http_error(401), audio_http_error(429), audio_http_error(429),
        audio_json(audio_operation()), http_response("application/jsonl", audio_results())});
    on_wait = [&](auto index) {
        ASSERT_TRUE(application->store().get_note_audio_attempt_for_note(note.id));
        if (index == 1) application->api_keys().replace(key_id, "new-key");
    };
    configure(server.port()); activate(); upload.start(); server.start();
    (void)application->start_background_note_audio(note.id, "Kore", {"Paragraph"}, epoch);
    ASSERT_TRUE(finished()); upload.join(); server.join();
    EXPECT_EQ(waits_snapshot(), (std::vector<int>{10, 20, 40}));
    ASSERT_EQ(server.requests().size(), 6u);
    for (std::size_t i = 1; i < 5; ++i) {
        EXPECT_TRUE(server.requests()[i].starts_with("POST /v1beta/models/"));
        EXPECT_EQ(request_body(server.requests()[i]), request_body(server.requests()[1]));
    }
    EXPECT_NE(server.requests()[1].find("old-key"), std::string::npos);
    EXPECT_NE(server.requests()[2].find("new-key"), std::string::npos);
    EXPECT_EQ(upload.requests().size(), 1u);
    const auto events = application->list_events(epoch);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].severity, EventSeverity::success);
    EXPECT_EQ(events[1].severity, EventSeverity::info);
    EXPECT_EQ(completions.load(), 1);
}

TEST_F(NoteAudioBackground, AbandonmentDuringSubmissionRetrySendsNothingMore) {
    MockHttpServer server({audio_http_error(503)});
    on_wait = [&](auto) { (void)application->set_note_read_only(note.id, false, epoch); };
    configure(server.port()); activate(); server.start();
    (void)application->start_background_note_audio(note.id, "Kore", {"Paragraph"}, epoch);
    ASSERT_TRUE(finished()); server.join();
    application->request_shutdown();
    ASSERT_TRUE(application->join_shutdown(2s));
    EXPECT_EQ(server.requests().size(), 1u);
    EXPECT_EQ(waits_snapshot(), (std::vector<int>{10}));
    const auto events = application->store().list_events();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].severity, EventSeverity::warning);
    EXPECT_NE(events[0].message.find("abandoned"), std::string::npos);
    EXPECT_EQ(completions.load(), 0);
}

TEST_F(NoteAudioBackground, ShutdownDuringSubmissionRetryRetainsTheUnregisteredAttempt) {
    MockHttpServer server({audio_http_error(503)});
    std::promise<void> waiting;
    on_wait = [&](auto) { application->request_shutdown(); waiting.set_value(); };
    configure(server.port()); activate(); server.start();
    (void)application->start_background_note_audio(note.id, "Kore", {"Paragraph"}, epoch);
    ASSERT_EQ(waiting.get_future().wait_for(2s), std::future_status::ready);
    ASSERT_TRUE(application->join_shutdown(2s)); server.join();
    ASSERT_TRUE(application->store().get_note_audio_attempt_for_note(note.id));
    EXPECT_FALSE(application->store().get_note_audio_attempt_for_note(note.id)->batch_id);
    EXPECT_TRUE(application->store().list_events().empty());
    EXPECT_EQ(waits_snapshot(), (std::vector<int>{10}));
    EXPECT_EQ(server.requests().size(), 1u);
    EXPECT_EQ(completions.load(), 0);
}

TEST_F(NoteAudioBackground, AcceptanceCommitRollbackCannotSubmitOrCleanUpTheImmediateRetry) {
    MockHttpServer upload({audio_json({{"file", {{"name", audio_file}}}})});
    MockHttpServer server({"HTTP/1.1 200 OK\r\nx-goog-upload-url: http://127.0.0.1:"
        + std::to_string(upload.port()) + "/secret\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        audio_json(audio_operation()), http_response("application/jsonl", audio_results())});
    std::promise<void> old_entered, release_old;
    auto released = release_old.get_future().share();
    std::atomic_int workers{};
    AudioWorkerCleanup cleanup{application, [&] { release_audio_gate(release_old); }};
    on_worker = [&](auto, auto cancel) {
        if (++workers == 1) {
            old_entered.set_value();
            released.wait_for(2s);
            EXPECT_TRUE(cancel->load());
        }
    };
    configure(server.port()); activate();
    storage::SqliteDatabase database(path, storage::SqliteDatabase::Mode::read_write);
    database.execute("CREATE TABLE fail_accept(value INTEGER REFERENCES note_items(id) DEFERRABLE INITIALLY DEFERRED)");
    database.execute("CREATE TRIGGER fail_accept_commit AFTER INSERT ON note_audio_attempts BEGIN INSERT INTO fail_accept VALUES(-1); END");
    EXPECT_THROW(application->start_background_note_audio(note.id, "Kore", {"Discarded text"}, epoch), std::runtime_error);
    ASSERT_EQ(old_entered.get_future().wait_for(2s), std::future_status::ready);
    EXPECT_FALSE(application->store().get_note_audio_attempt_for_note(note.id));
    EXPECT_TRUE(application->list_events(epoch).empty());
    database.execute("DROP TRIGGER fail_accept_commit");
    database.execute("DROP TABLE fail_accept");
    upload.start(); server.start();
    const auto accepted = application->start_background_note_audio(note.id, "Kore", {"Only this submission"}, epoch);
    EXPECT_EQ(accepted["attempt_id"], 1);
    release_old.set_value();
    ASSERT_TRUE(finished());
    upload.join(); server.join();
    EXPECT_EQ(server.requests().size(), 3u);
    ASSERT_EQ(upload.requests().size(), 1u);
    EXPECT_NE(upload.requests()[0].find("Only this submission"), std::string::npos);
    EXPECT_EQ(upload.requests()[0].find("Discarded text"), std::string::npos);
    EXPECT_EQ(completions.load(), 1);
    EXPECT_EQ(application->list_events(epoch).size(), 2u);
}

TEST_F(NoteAudioBackground, SubmissionsAreSerializedButRegisteredBatchesFinishIndependently) {
    MockHttpServer a_upload({audio_json({{"file", {{"name", audio_file}}}})});
    MockHttpServer a({"HTTP/1.1 200 OK\r\nx-goog-upload-url: http://127.0.0.1:"
        + std::to_string(a_upload.port()) + "/secret\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", audio_json(audio_operation()), http_response("application/jsonl", audio_results())});
    MockHttpServer b_upload({audio_json({{"file", {{"name", "files/" + std::string(32, 'c')}}}})});
    auto b_operation = audio_operation(); b_operation["name"] = "batches/" + std::string(32, 'd');
    b_operation["response"]["responsesFile"] = "files/" + std::string(32, 'c');
    MockHttpServer b({"HTTP/1.1 200 OK\r\nx-goog-upload-url: http://127.0.0.1:"
        + std::to_string(b_upload.port()) + "/secret\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        audio_json(b_operation), http_response("application/jsonl", audio_results())});
    const auto other = application->create_note_item({}, NoteItemKind::note, "B", epoch);
    (void)application->set_note_read_only(other.id, true, epoch);
    AudioWorkerCleanup cleanup{application, [&] { a_upload.resume_responses(); a.resume_responses(); b.resume_responses(); }};
    a_upload.pause_before_response(1); a.pause_before_response(3);
    a_upload.start(); a.start(); b_upload.start(); b.start();
    configure(a.port()); activate();
    const auto first = application->start_background_note_audio(note.id, "Kore", {"A"}, epoch);
    ASSERT_TRUE(a_upload.wait_for_requests(1, 2s));
    configure(b.port());
    const auto submission_started = std::chrono::steady_clock::now();
    const auto second = application->start_background_note_audio(other.id, "Kore", {"B"}, epoch, true);
    EXPECT_NE(first["attempt_id"], second["attempt_id"]);
    EXPECT_THROW(application->start_background_note_audio(note.id, "Kore", {"duplicate"}, epoch), ApplicationError);
    EXPECT_EQ(application->start_background_note_audio(note.id, "Kore", {"duplicate"}, epoch, true), (nlohmann::json{{"skipped", true}}));
    EXPECT_LT(std::chrono::steady_clock::now() - submission_started, 500ms);
    EXPECT_FALSE(b.wait_for_requests(1, 150ms));
    EXPECT_TRUE(application->background_note_audio_status(other.id, epoch)["pending"]);
    // Registration releases submission even while A's result download is stalled.
    a_upload.resume_responses();
    ASSERT_TRUE(a.wait_for_requests(3, 2s));
    const bool b_finished = finished(other.id);
    // Release A even if an expectation fails; the mock has a five-second deadline.
    EXPECT_TRUE(application->background_note_audio_status(note.id, epoch)["pending"]);
    EXPECT_EQ(application->get_note_audio_info(other.id, epoch)["count"], 1);
    EXPECT_EQ(completed_id.load(), other.id);
    a.resume_responses();
    EXPECT_TRUE(b_finished);
    ASSERT_TRUE(finished());
    a_upload.join(); a.join(); b_upload.join(); b.join();
    EXPECT_EQ(application->get_note_audio_info(note.id, epoch)["count"], 1);
    EXPECT_EQ(application->get_note_audio_info(other.id, epoch)["count"], 1);
    EXPECT_EQ(application->list_events(epoch).front().severity, EventSeverity::success);
}

TEST_F(NoteAudioBackground, SubmissionRetryBudgetSpansUploadAndCreationThenReleasesTheNextNote) {
    MockHttpServer upload({audio_http_error(503), audio_json({{"file", {{"name", audio_file}}}})});
    const auto upload_start = "HTTP/1.1 200 OK\r\nx-goog-upload-url: http://127.0.0.1:"
        + std::to_string(upload.port()) + "/secret\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    MockHttpServer first({audio_http_error(503), upload_start, upload_start, audio_http_error(429), audio_http_error(429)});
    MockHttpServer next_upload({audio_json({{"file", {{"name", audio_file}}}})});
    MockHttpServer next({"HTTP/1.1 200 OK\r\nx-goog-upload-url: http://127.0.0.1:"
        + std::to_string(next_upload.port()) + "/secret\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        audio_json(audio_operation()), http_response("application/jsonl", audio_results())});
    const auto other = application->create_note_item({}, NoteItemKind::note, "Next", epoch);
    (void)application->set_note_read_only(other.id, true, epoch);
    AudioWorkerCleanup cleanup{application, [&] { first.resume_responses(); }};
    first.pause_before_response(1);
    first.start(); upload.start(); next.start(); next_upload.start();
    configure(first.port()); activate();
    (void)application->start_background_note_audio(note.id, "Kore", {"First"}, epoch);
    ASSERT_TRUE(first.wait_for_requests(1, 2s));
    configure(next.port());
    (void)application->start_background_note_audio(other.id, "Kore", {"Next"}, epoch);
    EXPECT_FALSE(next.wait_for_requests(1, 150ms));
    first.resume_responses();
    ASSERT_TRUE(finished()); ASSERT_TRUE(finished(other.id));
    first.join(); upload.join(); next.join(); next_upload.join();
    EXPECT_EQ(waits_snapshot(), (std::vector<int>{10, 20, 40}));
    EXPECT_EQ(first.requests().size(), 5u);
    EXPECT_EQ(upload.requests().size(), 2u);
    EXPECT_EQ(next.requests().size(), 3u);
    EXPECT_EQ(application->get_note_audio_info(note.id, epoch)["count"], 0);
    EXPECT_EQ(application->get_note_audio_info(other.id, epoch)["count"], 1);
    const auto events = application->list_events(epoch);
    ASSERT_EQ(events.size(), 3u);
    const auto failure = std::find_if(events.begin(), events.end(), [](const auto& event) {
        return event.severity == EventSeverity::error;
    });
    ASSERT_NE(failure, events.end());
    EXPECT_NE(failure->message.find("Audio batch creation failed"), std::string::npos);
    EXPECT_NE(failure->message.find("retry limit reached"), std::string::npos);
}

TEST_F(NoteAudioBackground, AbandoningAWaitingSubmissionAllowsRestartWithoutSendingTheOldRequest) {
    const auto partial = "HTTP/1.1 200 OK\r\nContent-Length: 100000\r\nConnection: close\r\n\r\n";
    MockHttpServer first({partial}, true);
    MockHttpServer upload({audio_json({{"file", {{"name", audio_file}}}})});
    MockHttpServer next({"HTTP/1.1 200 OK\r\nx-goog-upload-url: http://127.0.0.1:"
        + std::to_string(upload.port()) + "/secret\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        audio_json(audio_operation()), http_response("application/jsonl", audio_results())});
    const auto other = application->create_note_item({}, NoteItemKind::note, "Waiting", epoch);
    (void)application->set_note_read_only(other.id, true, epoch);
    first.start(); next.start(); upload.start(); configure(first.port()); activate();
    (void)application->start_background_note_audio(note.id, "Kore", {"First"}, epoch);
    ASSERT_TRUE(first.wait_for_requests(1, 2s));
    configure(next.port());
    const auto old = application->start_background_note_audio(other.id, "Kore", {"Discarded"}, epoch);
    EXPECT_FALSE(next.wait_for_requests(1, 150ms));
    (void)application->set_note_read_only(other.id, false, epoch);
    (void)application->set_note_read_only(other.id, true, epoch);
    const auto replacement = application->start_background_note_audio(other.id, "Kore", {"Replacement"}, epoch);
    EXPECT_NE(old["attempt_id"], replacement["attempt_id"]);
    (void)application->set_note_read_only(note.id, false, epoch);
    ASSERT_TRUE(finished(other.id)); first.join(); next.join(); upload.join();
    ASSERT_EQ(upload.requests().size(), 1u);
    EXPECT_NE(request_body(upload.requests()[0]).find("Replacement"), std::string::npos);
    EXPECT_EQ(request_body(upload.requests()[0]).find("Discarded"), std::string::npos);
    EXPECT_EQ(completions.load(), 1);
}

TEST_F(NoteAudioBackground, ShutdownInterruptsActiveAndWaitingSubmissions) {
    MockHttpServer first({"HTTP/1.1 200 OK\r\nContent-Length: 100000\r\nConnection: close\r\n\r\n"}, true);
    MockHttpServer upload({audio_json({{"file", {{"name", audio_file}}}})});
    MockHttpServer next({"HTTP/1.1 200 OK\r\nx-goog-upload-url: http://127.0.0.1:"
        + std::to_string(upload.port()) + "/secret\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        audio_json(audio_operation()), http_response("application/jsonl", audio_results())});
    const auto other = application->create_note_item({}, NoteItemKind::note, "Waiting", epoch);
    (void)application->set_note_read_only(other.id, true, epoch);
    first.start(); next.start(); configure(first.port()); activate();
    (void)application->start_background_note_audio(note.id, "Kore", {"First"}, epoch);
    ASSERT_TRUE(first.wait_for_requests(1, 2s));
    configure(next.port());
    (void)application->start_background_note_audio(other.id, "Kore", {"Waiting"}, epoch);
    application->request_shutdown();
    EXPECT_TRUE(application->join_shutdown(2s)); first.join();
    EXPECT_FALSE(next.wait_for_requests(1, 50ms));
    EXPECT_EQ(application->store().get_note_audio_attempts().size(), 2u);
    EXPECT_TRUE(application->store().list_events().empty());
    application.reset();
    application = Application::open(make_command(workspace, path));
    epoch = application->context_epoch();
    activate();
    ASSERT_TRUE(finished()); ASSERT_TRUE(finished(other.id));
    EXPECT_FALSE(next.wait_for_requests(1, 50ms));
    const auto events = application->list_events(epoch);
    ASSERT_EQ(events.size(), 2u);
    for (const auto& event : events) {
        EXPECT_EQ(event.severity, EventSeverity::warning);
        EXPECT_NE(event.message.find("possibly before submission started"), std::string::npos);
        EXPECT_NE(event.message.find("If it was submitted"), std::string::npos);
    }
    // An explicit new start succeeds against this same listening service, proving
    // neither recovery worker sent a request before the warnings retired them.
    upload.start();
    (void)application->start_background_note_audio(other.id, "Kore", {"Explicit resubmission"}, epoch);
    ASSERT_TRUE(finished(other.id)); next.join(); upload.join();
    EXPECT_EQ(next.requests().size(), 3u);
    EXPECT_EQ(application->get_note_audio_info(other.id, epoch)["count"], 1);
}

TEST_F(NoteAudioBackground, RecoveryLaunchFailureAndUnknownRowsDoNotBlockOtherRegisteredJobs) {
    MockHttpServer a({audio_json(audio_operation()), http_response("application/jsonl", audio_results())});
    auto operation = audio_operation(); operation["name"] = "batches/" + std::string(32, 'c');
    operation["response"]["responsesFile"] = "files/" + std::string(32, 'd');
    MockHttpServer b({audio_json(operation), http_response("application/jsonl", audio_results())});
    const auto first = saved_attempt(a.port());
    const auto other = application->create_note_item({}, NoteItemKind::note, "B", epoch);
    const auto unknown = application->create_note_item({}, NoteItemKind::note, "Unknown", epoch);
    for (auto id : {other.id, unknown.id}) (void)application->set_note_read_only(id, true, epoch);
    const auto second = *application->store().accept_note_audio(other.id, 1, "http://127.0.0.1:" + std::to_string(b.port()), key_id, [](const auto&) {});
    ASSERT_TRUE(application->store().register_note_audio(second.id, operation["name"].get<std::string>(), "Kore"));
    (void)application->store().accept_note_audio(unknown.id, 1, "http://127.0.0.1:1", key_id, [](const auto&) {});
    bool fail = true;
    AudioWorkerCleanup cleanup{application, [&] { a.resume_responses(); b.resume_responses(); }};
    on_launch = [&](auto id) { if (id == first.id && std::exchange(fail, false)) throw std::runtime_error("launch"); };
    a.pause_before_response(1); b.pause_before_response(1); a.start(); b.start();
    activate();
    ASSERT_TRUE(b.wait_for_requests(1, 2s));
    EXPECT_TRUE(application->background_note_audio_status(note.id, epoch)["pending"]);
    EXPECT_TRUE(application->background_note_audio_status(other.id, epoch)["pending"]);
    ASSERT_TRUE(finished(unknown.id));
    activate(); activate();
    ASSERT_TRUE(a.wait_for_requests(1, 2s));
    b.resume_responses();
    EXPECT_TRUE(finished(other.id));
    EXPECT_TRUE(application->background_note_audio_status(note.id, epoch)["pending"]);
    a.resume_responses();
    EXPECT_TRUE(finished()); a.join(); b.join();
    EXPECT_EQ(completions.load(), 2);
    for (const auto* server : {&a, &b}) {
        ASSERT_EQ(server->requests().size(), 2u);
        for (const auto& request : server->requests()) EXPECT_TRUE(request.starts_with("GET "));
    }
    const auto events = application->list_events(epoch);
    EXPECT_EQ(std::count_if(events.begin(), events.end(), [](const auto& event) { return event.severity == EventSeverity::warning; }), 1);
}

TEST_F(NoteAudioBackground, RetryingJobsWarnIndependentlyAndDoNotBlockCompletion) {
    auto b_operation = audio_operation(); b_operation["name"] = "batches/" + std::string(32, 'c');
    b_operation["response"]["responsesFile"] = "files/" + std::string(32, 'd');
    auto c_operation = audio_operation(); c_operation["name"] = "batches/" + std::string(32, 'e');
    c_operation["response"]["responsesFile"] = "files/" + std::string(32, 'f');
    MockHttpServer a({audio_http_error(401), audio_json(audio_operation()), http_response("application/jsonl", audio_results())});
    MockHttpServer b({audio_http_error(401), audio_json(b_operation), http_response("application/jsonl", audio_results())});
    MockHttpServer c({audio_json(c_operation), http_response("application/jsonl", audio_results())});
    saved_attempt(a.port());
    std::vector<std::int64_t> ids{note.id};
    for (auto* server : {&b, &c}) {
        const auto item = application->create_note_item({}, NoteItemKind::note, "Note " + std::to_string(ids.size()), epoch);
        (void)application->set_note_read_only(item.id, true, epoch);
        const auto attempt = *application->store().accept_note_audio(item.id, 1, "http://127.0.0.1:" + std::to_string(server->port()), key_id, [](const auto&) {});
        ASSERT_TRUE(application->store().register_note_audio(attempt.id,
            (server == &b ? b_operation : c_operation)["name"].get<std::string>(), "Kore"));
        ids.push_back(item.id);
    }
    std::promise<void> both_waiting, release;
    auto released = release.get_future().share();
    AudioWorkerCleanup cleanup{application, [&] { release_audio_gate(release); }};
    on_wait = [&](auto index) { if (index == 2) both_waiting.set_value(); released.wait_for(3s); };
    a.start(); b.start(); c.start(); activate();
    const auto waiting = both_waiting.get_future().wait_for(2s);
    const bool c_finished = finished(ids[2]);
    EXPECT_TRUE(application->background_note_audio_status(ids[0], epoch)["pending"]);
    EXPECT_TRUE(application->background_note_audio_status(ids[1], epoch)["pending"]);
    const auto events = application->list_events(epoch);
    EXPECT_EQ(std::count_if(events.begin(), events.end(), [](const auto& event) { return event.severity == EventSeverity::warning; }), 2);
    release.set_value();
    EXPECT_EQ(waiting, std::future_status::ready); EXPECT_TRUE(c_finished);
    EXPECT_TRUE(finished(ids[0])); EXPECT_TRUE(finished(ids[1]));
    a.join(); b.join(); c.join();
    EXPECT_EQ(completions.load(), 3);
}

TEST_F(NoteAudioBackground, MissingOnlySkipsUnusableContentAndWarningFailureIsDefinite) {
    activate();
    EXPECT_EQ(application->start_background_note_audio(note.id, "Kore", {}, epoch, true), (nlohmann::json{{"skipped", true}}));
    EXPECT_EQ(application->list_events(epoch).size(), 1u); // Empty input needs no configured service.
    configure(1);
    EXPECT_EQ(application->start_background_note_audio(note.id, "Kore", {std::string(5001, 'x')}, epoch, true), (nlohmann::json{{"skipped", true}}));
    EXPECT_EQ(application->list_events(epoch).size(), 2u);
    storage::SqliteDatabase db(path, storage::SqliteDatabase::Mode::read_write);
    db.execute("CREATE TRIGGER fail_warning BEFORE INSERT ON event_history BEGIN SELECT RAISE(ABORT,'private SQL'); END");
    try { (void)application->start_background_note_audio(note.id, "Kore", {}, epoch, true); FAIL(); }
    catch (const ApplicationError& error) {
        EXPECT_EQ(error.code, ErrorCode::invalid_argument);
        EXPECT_NE(std::string(error.what()).find("No job was started"), std::string::npos);
        EXPECT_EQ(std::string(error.what()).find("private SQL"), std::string::npos);
    }
    EXPECT_TRUE(application->store().get_note_audio_attempts().empty());
    db.execute("DROP TRIGGER fail_warning");
    EXPECT_EQ(application->start_background_note_audio(note.id, "Kore", {}, epoch, true), (nlohmann::json{{"skipped", true}}));
}

TEST_F(NoteAudioBackground, ShutdownSignalsAllControlsBeforeJoinAndWithoutTheLifecycleLock) {
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<std::shared_ptr<std::atomic_bool>> controls;
    std::promise<void> release;
    auto released = release.get_future().share();
    AudioWorkerCleanup cleanup{application, [&] { release_audio_gate(release); }};
    on_worker = [&](auto, auto cancel) {
        { const std::lock_guard lock(mutex); controls.push_back(cancel); ready.notify_all(); }
        released.wait_for(3s);
    };
    saved_attempt(1, false);
    const auto other = application->create_note_item({}, NoteItemKind::note, "B", epoch);
    (void)application->set_note_read_only(other.id, true, epoch);
    (void)application->store().accept_note_audio(other.id, 1, "http://127.0.0.1:1", key_id, [](const auto&) {});
    activate();
    {
        std::unique_lock lock(mutex);
        ASSERT_TRUE(ready.wait_for(lock, 2s, [&] { return controls.size() == 2; }));
    }
    const auto third = application->create_note_item({}, NoteItemKind::note, "C", epoch);
    (void)application->set_note_read_only(third.id, true, epoch);
    configure(1);
    std::promise<void> launching, release_launch;
    auto launch_released = release_launch.get_future().share();
    AudioWorkerCleanup launch_cleanup{application, [&] { release_audio_gate(release_launch); release_audio_gate(release); }};
    on_launch = [&](auto) { launching.set_value(); launch_released.wait_for(3s); };
    auto request = std::async(std::launch::async, [&] {
        return application->start_background_note_audio(third.id, "Kore", {"C"}, epoch);
    });
    ASSERT_EQ(launching.get_future().wait_for(2s), std::future_status::ready);
    const auto start = std::chrono::steady_clock::now();
    application->request_shutdown();
    EXPECT_LT(std::chrono::steady_clock::now() - start, 500ms);
    { const std::lock_guard lock(mutex); for (const auto& control : controls) EXPECT_TRUE(control->load()); }
    release_launch.set_value();
    (void)request.get();
    release.set_value();
    EXPECT_TRUE(application->join_shutdown(2s));
    EXPECT_EQ(application->store().get_note_audio_attempts().size(), 3u);
}

TEST_F(NoteAudioBackground, LaunchAndCommitFailureDoNotCancelAnotherLiveAttempt) {
    std::mutex mutex;
    std::condition_variable ready;
    std::vector<std::shared_ptr<std::atomic_bool>> controls;
    std::promise<void> release;
    const auto released = release.get_future().share();
    AudioWorkerCleanup cleanup{application, [&] { release_audio_gate(release); }};
    on_worker = [&](auto, auto cancel) {
        { const std::lock_guard lock(mutex); controls.push_back(cancel); ready.notify_all(); }
        released.wait_for(3s);
    };
    configure(1); activate();
    const auto first = application->start_background_note_audio(note.id, "Kore", {"A"}, epoch);
    { std::unique_lock lock(mutex); ASSERT_TRUE(ready.wait_for(lock, 2s, [&] { return controls.size() == 1; })); }
    const auto other = application->create_note_item({}, NoteItemKind::note, "B", epoch);
    (void)application->set_note_read_only(other.id, true, epoch);
    on_launch = [](auto) { throw std::runtime_error("launch failed"); };
    EXPECT_THROW(application->start_background_note_audio(other.id, "Kore", {"B"}, epoch), std::runtime_error);
    EXPECT_TRUE(application->store().get_note_audio_attempt_by_id(first["attempt_id"].get<std::int64_t>()));
    EXPECT_FALSE(application->store().get_note_audio_attempt_for_note(other.id));
    { const std::lock_guard lock(mutex); EXPECT_FALSE(controls[0]->load()); }
    on_launch = {};
    storage::SqliteDatabase db(path, storage::SqliteDatabase::Mode::read_write);
    db.execute("CREATE TABLE fail_accept(value INTEGER REFERENCES note_items(id) DEFERRABLE INITIALLY DEFERRED)");
    db.execute("CREATE TRIGGER fail_commit AFTER INSERT ON note_audio_attempts BEGIN INSERT INTO fail_accept VALUES(-1); END");
    EXPECT_THROW(application->start_background_note_audio(other.id, "Kore", {"B"}, epoch), std::runtime_error);
    { std::unique_lock lock(mutex); ASSERT_TRUE(ready.wait_for(lock, 2s, [&] { return controls.size() == 2; }));
      EXPECT_FALSE(controls[0]->load()); EXPECT_TRUE(controls[1]->load()); }
    EXPECT_EQ(application->store().get_note_audio_attempts().size(), 1u);
    application->request_shutdown(); release.set_value();
    EXPECT_TRUE(application->join_shutdown(2s));
}

TEST_F(NoteAudioBackground, AbandonmentSignalsOnlyTheRemovedNotesAndContextChangesSignalAll) {
    for (const std::string action : {"editable", "delete", "callback", "maintenance", "failed maintenance", "switch"}) {
        SCOPED_TRACE(action);
        std::mutex mutex;
        std::condition_variable ready;
        std::vector<std::pair<std::int64_t, std::shared_ptr<std::atomic_bool>>> controls;
        std::promise<void> release;
        const auto released = release.get_future().share();
        AudioWorkerCleanup cleanup{application, [&] { release_audio_gate(release); }};
        on_worker = [&](auto id, auto cancel) {
            { const std::lock_guard lock(mutex); controls.emplace_back(id, cancel); ready.notify_all(); }
            released.wait_for(3s);
        };
        application->set_note_audio_changed({});
        const auto folder = application->create_note_item({}, NoteItemKind::folder, "Folder " + action, epoch);
        const auto a = application->create_note_item(folder.id, NoteItemKind::note, "A", epoch);
        const auto nested = application->create_note_item(folder.id, NoteItemKind::folder, "Nested", epoch);
        const auto b = application->create_note_item(nested.id, NoteItemKind::note, "B", epoch);
        const auto outside = application->create_note_item({}, NoteItemKind::note, "Outside " + action, epoch);
        for (auto id : {a.id, b.id, outside.id}) {
            (void)application->set_note_read_only(id, true, epoch);
            (void)application->store().accept_note_audio(id, 1, "http://127.0.0.1:1", key_id, [](const auto&) {});
        }
        activate();
        { std::unique_lock lock(mutex); ASSERT_TRUE(ready.wait_for(lock, 2s, [&] { return controls.size() == 3; })); }
        const auto a_attempt = application->store().get_note_audio_attempt_for_note(a.id)->id;
        const auto b_attempt = application->store().get_note_audio_attempt_for_note(b.id)->id;
        if (action == "editable") (void)application->set_note_read_only(a.id, false, epoch);
        else if (action == "delete") application->delete_note_item(folder.id, epoch);
        else if (action == "callback") application->set_note_audio_changed({});
        else if (action == "maintenance") (void)application->update_vault("Test", {.display_name = "Test"}, epoch);
        else if (action == "failed maintenance") fail_maintenance();
        else {
            (void)application->create_vault({.display_name = "Switched"}, epoch);
            (void)application->switch_vault("Switched", {}, epoch);
        }
        epoch = application->context_epoch();
        { const std::lock_guard lock(mutex);
          for (std::size_t i = 0; i < 3; ++i) {
            const auto& [id, cancel] = controls[i];
            const bool abandoned = action == "editable" ? id == a_attempt
                : action == "delete" ? id == a_attempt || id == b_attempt : true;
            EXPECT_EQ(cancel->load(), abandoned);
          }
        }
        // Stop replacement workers too before releasing the barrier; no HTTP is needed.
        application->set_note_audio_changed({});
        release.set_value();
        // Reopening the fixture joins all workers before the local hooks are destroyed.
        application.reset();
        cleanup.release = {};
        on_worker = {};
        std::filesystem::remove_all(workspace.root() / "cha-config");
        auto command = make_command(workspace, path);
        command.test_shutdown_grace_ms = 50;
        RuntimeSettings settings;
        settings.note_audio_worker_started = [this](auto id, auto cancel) { if (on_worker) on_worker(id, std::move(cancel)); };
        application = Application::open(command, {}, settings);
        epoch = application->context_epoch();
        // Retire retained test rows without adding recovery workers to the next case.
        for (const auto& attempt : application->store().get_note_audio_attempts())
            (void)application->store().retire_note_audio(attempt.id, EventSeverity::warning, "Test cleanup");
    }
}

TEST_F(NoteAudioBackground, RegistrationAndFinalEventFailuresRetryOnlyTheLocalTransaction) {
    MockHttpServer upload({audio_json({{"file", {{"name", audio_file}}}})});
    MockHttpServer server({"HTTP/1.1 200 OK\r\nx-goog-upload-url: http://127.0.0.1:"
        + std::to_string(upload.port()) + "/secret\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        audio_json(audio_operation()), http_response("application/jsonl", audio_results())});
    configure(server.port()); activate();
    storage::SqliteDatabase database(path, storage::SqliteDatabase::Mode::read_write);
    database.execute("CREATE TRIGGER fail_start BEFORE INSERT ON event_history WHEN NEW.severity='info' BEGIN SELECT RAISE(ABORT,'full'); END");
    database.execute("CREATE TRIGGER fail_finish BEFORE INSERT ON event_history WHEN NEW.severity='success' BEGIN SELECT RAISE(ABORT,'full'); END");
    on_wait = [&](auto index) {
        EXPECT_TRUE(application->store().get_note_audio_attempt_for_note(note.id));
        database.execute(index == 1 ? "DROP TRIGGER fail_start" : "DROP TRIGGER fail_finish");
    };
    upload.start(); server.start();
    (void)application->start_background_note_audio(note.id, "Kore", {"Paragraph"}, epoch);
    ASSERT_TRUE(finished());
    upload.join(); server.join();
    EXPECT_EQ(waits_snapshot(), (std::vector<int>{10, 10}));
    EXPECT_EQ(server.requests().size(), 3u);
    EXPECT_EQ(application->list_events(epoch).size(), 2u);
    EXPECT_EQ(completions.load(), 1);
}

TEST_F(NoteAudioBackground, NativeRecoveryReadsSavedBatchImmediatelyWithoutSubmitting) {
    MockHttpServer server({audio_json(audio_operation()), http_response("application/jsonl", audio_results())});
    const auto attempt = saved_attempt(server.port());
    // A daemon/no native callback leaves the record untouched.
    EXPECT_EQ(application->store().get_note_audio_attempt_for_note(note.id)->id, attempt.id);
    application.reset();
    application = Application::open(make_command(workspace, path));
    epoch = application->context_epoch();
    server.start(); activate(); activate();
    ASSERT_TRUE(finished());
    server.join();
    ASSERT_EQ(server.requests().size(), 2u);
    EXPECT_TRUE(server.requests()[0].starts_with("GET /v1beta/" + audio_batch));
    EXPECT_TRUE(server.requests()[1].starts_with("GET /download/"));
    EXPECT_EQ(application->list_events(epoch).size(), 2u);
    EXPECT_EQ(completions.load(), 1);
}

TEST_F(NoteAudioBackground, UnknownSubmissionRetiresOnceAndSecondaryProcessLeavesItIntact) {
    const auto attempt = saved_attempt(1, false);
    auto secondary = Application::open(make_command(workspace, path));
    secondary->set_note_audio_changed([](auto, auto) {});
    EXPECT_EQ(secondary->store().get_note_audio_attempt_for_note(note.id)->id, attempt.id);
    EXPECT_THROW(secondary->start_background_note_audio(note.id, "Kore", {"Text"}, secondary->context_epoch()), ApplicationError);
    secondary.reset();
    activate(); activate();
    ASSERT_TRUE(finished());
    const auto events = application->list_events(epoch);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].severity, EventSeverity::warning);
    EXPECT_NE(events[0].message.find("status unknown"), std::string::npos);
    EXPECT_EQ(completions.load(), 0);
}

TEST_F(NoteAudioBackground, RetainsReadsBeyondThreeFailuresAndResolvesTheSameKeyAgain) {
    MockHttpServer server({audio_http_error(503), audio_http_error(500), audio_http_error(503), audio_http_error(503),
        audio_http_error(401), audio_json(audio_operation()), http_response("application/jsonl", audio_results())});
    saved_attempt(server.port());
    on_wait = [&](auto index) {
        if (index == 5) application->api_keys().replace(key_id, "new-key");
    };
    server.start(); activate();
    ASSERT_TRUE(finished());
    server.join();
    EXPECT_EQ(waits_snapshot(), (std::vector<int>{10, 20, 40, 40, 40}));
    ASSERT_EQ(server.requests().size(), 7u);
    EXPECT_NE(server.requests()[4].find("old-key"), std::string::npos);
    EXPECT_NE(server.requests()[5].find("new-key"), std::string::npos);
    const auto events = application->list_events(epoch);
    EXPECT_EQ(std::count_if(events.begin(), events.end(), [](const auto& event) { return event.severity == EventSeverity::warning; }), 1);
    EXPECT_EQ(completions.load(), 1);
    const auto warning = std::find_if(events.begin(), events.end(), [](const auto& event) { return event.severity == EventSeverity::warning; });
    ASSERT_NE(warning, events.end());
    EXPECT_NE(warning->message.find("HTTP 503"), std::string::npos);
    const auto log = log_text();
    EXPECT_NE(log.find("HTTP 503"), std::string::npos);
    EXPECT_NE(log.find("HTTP 401"), std::string::npos);
    EXPECT_EQ(log.find("old-key"), std::string::npos);
    EXPECT_EQ(log.find("new-key"), std::string::npos);
}

TEST_F(NoteAudioBackground, AbandonmentDiscardsLateCompletionAndDoesNotTouchANewAttempt) {
    MockHttpServer server({audio_json(audio_operation()), http_response("application/jsonl", audio_results())});
    const auto attempt = saved_attempt(server.port());
    server.pause_before_response(2); server.start(); activate();
    ASSERT_TRUE(server.wait_for_requests(2, 2s));
    (void)application->set_note_read_only(note.id, false, epoch);
    (void)application->set_note_read_only(note.id, true, epoch);
    const auto next = saved_attempt(1, false);
    EXPECT_GT(next.id, attempt.id);
    server.resume_responses(); server.join();
    EXPECT_EQ(application->store().get_note_audio_attempt_for_note(note.id)->id, next.id);
    EXPECT_EQ(application->get_note_audio_info(note.id, epoch)["count"], 0);
    EXPECT_EQ(completions.load(), 0);
    const auto events = application->list_events(epoch);
    EXPECT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].severity, EventSeverity::warning);
}
TEST_F(NoteAudioBackground, FailedMaintenanceReservationResumesRegisteredWorkInTheNewEpoch) {
    auto failed = audio_operation(); failed.erase("response"); failed["error"] = {{"message", "failed"}};
    MockHttpServer server({audio_json(audio_operation(false)), audio_json(failed)});
    saved_attempt(server.port());
    std::promise<void> waiting, release;
    auto released = release.get_future().share();
    on_wait = [&](auto) { waiting.set_value(); released.wait(); };
    server.start(); activate();
    ASSERT_EQ(waiting.get_future().wait_for(2s), std::future_status::ready);
    fail_maintenance();
    release.set_value();
    ASSERT_TRUE(finished());
    server.join();
    ASSERT_EQ(server.requests().size(), 2u);
    for (const auto& request : server.requests()) EXPECT_TRUE(request.starts_with("GET /v1beta/" + audio_batch));
    const auto events = application->list_events(epoch);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[0].severity, EventSeverity::error);
    EXPECT_EQ(events[1].severity, EventSeverity::info);
}

TEST_F(NoteAudioBackground, FailedMaintenanceReservationRetiresInterruptedSubmissionWithoutResubmitting) {
    MockHttpServer server({"HTTP/1.1 200 OK\r\nx-goog-upload-url: http://127.0.0.1:1/secret\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"});
    server.pause_before_response(1);
    configure(server.port()); activate(); server.start();
    (void)application->start_background_note_audio(note.id, "Kore", {"Paragraph"}, epoch);
    ASSERT_TRUE(server.wait_for_requests(1, 2s));
    fail_maintenance();
    server.resume_responses(); server.join();
    ASSERT_TRUE(finished());
    EXPECT_EQ(server.requests().size(), 1u);
    const auto events = application->list_events(epoch);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].severity, EventSeverity::warning);
    EXPECT_NE(events[0].message.find("status unknown"), std::string::npos);
}

TEST_F(NoteAudioBackground, FailureEventsAndLogsKeepSafeSpecificReasons) {
    const auto previous = workspace.root() / "previous.m4a";
    std::ofstream(previous) << "previous audio";
    application->store().save_note_audio(note.id, {previous});
    auto corrupt = nlohmann::json::parse(audio_results());
    auto bytes = decode_base64(corrupt["response"]["candidates"][0]["content"]["parts"][0]["inlineData"]["data"].get_ref<const std::string&>());
    bytes.pop_back();
    corrupt["response"]["candidates"][0]["content"]["parts"][0]["inlineData"]["data"] = encode_base64(bytes);
    const std::vector<std::pair<std::string, std::string>> cases{
        {"", "The service returned an incomplete audio batch."},
        {nlohmann::json{{"key", "0"}, {"error", {{"message", "private service body old-key"}}}}.dump() + '\n',
            "Audio generation failed for text block 1."},
        {corrupt.dump() + '\n', "Could not convert audio for text block 1. The service returned a truncated or invalid WAV."},
        {"private service body old-key\n", "Check the service response and audio format"},
    };
    for (const auto& [body, reason] : cases) {
        SCOPED_TRACE(reason);
        MockHttpServer server({audio_json(audio_operation()), http_response("application/jsonl", body)});
        saved_attempt(server.port());
        server.start(); activate();
        ASSERT_TRUE(finished()); server.join();
        const auto event = application->list_events(epoch).front();
        EXPECT_EQ(event.severity, EventSeverity::error);
        EXPECT_NE(event.message.find(reason), std::string::npos);
        EXPECT_NE(event.message.find("Chapter"), std::string::npos);
        EXPECT_EQ(event.message.find("Audio audio"), std::string::npos);
        EXPECT_EQ(event.message.find("ExtAudioFile"), std::string::npos);
        EXPECT_EQ(application->store().get_note_audio_clip(note.id, 0).bytes, "previous audio");
    }
    const auto log = log_text();
    EXPECT_NE(log.find("incomplete audio batch"), std::string::npos);
    EXPECT_NE(log.find("Audio conversion failed"), std::string::npos);
    EXPECT_EQ(log.find("private service body"), std::string::npos);
    EXPECT_EQ(log.find("old-key"), std::string::npos);
}

TEST_F(NoteAudioBackground, UnusableCreationRepliesWarnAboutDuplicateWork) {
    const auto previous = workspace.root() / "previous.m4a";
    std::ofstream(previous) << "previous audio";
    application->store().save_note_audio(note.id, {previous});
    std::size_t outcomes = 0;
    for (const auto& response : std::vector<std::string>{"", audio_http_error(408), audio_http_error(500),
        audio_http_error(503), "HTTP/1.1 200 OK\r\nContent-Length: 1000\r\nConnection: close\r\n\r\n{\"name\":\"batches/", audio_json({{"name", "batches/invalid"}}),
        http_response("application/json", "private service body old-key")}) {
        SCOPED_TRACE(response);
        MockHttpServer upload({audio_json({{"file", {{"name", audio_file}}}})});
        MockHttpServer server({"HTTP/1.1 200 OK\r\nx-goog-upload-url: http://127.0.0.1:"
            + std::to_string(upload.port()) + "/secret\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", response});
        configure(server.port()); activate(); upload.start(); server.start();
        (void)application->start_background_note_audio(note.id, "Kore", {"Paragraph"}, epoch);
        ASSERT_TRUE(finished()); upload.join(); server.join();
        EXPECT_EQ(server.requests().size(), 2u);
        EXPECT_EQ(upload.requests().size(), 1u);
        EXPECT_TRUE(waits_snapshot().empty());
        EXPECT_EQ(application->store().get_note_audio_clip(note.id, 0).bytes, "previous audio");
        const auto events = application->list_events(epoch);
        ASSERT_EQ(events.size(), ++outcomes);
        EXPECT_EQ(events[0].severity, EventSeverity::warning);
        EXPECT_NE(events[0].message.find("Audio batch creation for"), std::string::npos);
        EXPECT_NE(events[0].message.find("Submission status unknown"), std::string::npos);
        EXPECT_NE(events[0].message.find("Starting again may generate audio twice"), std::string::npos);
        EXPECT_EQ(events[0].message.find("Check Events"), std::string::npos);
        EXPECT_EQ(events[0].message.find("private service body"), std::string::npos);
    }
    EXPECT_EQ(log_text().find("private service body"), std::string::npos);
    EXPECT_EQ(completions.load(), 0);
}

TEST_F(NoteAudioBackground, PermanentSubmissionRejectionsRetireWithoutRetrying) {
    for (const bool creation : {false, true}) {
        MockHttpServer upload(creation ? std::vector<std::string>{audio_json({{"file", {{"name", audio_file}}}})}
            : std::vector<std::string>{});
        std::vector<std::string> responses;
        if (creation) responses.push_back("HTTP/1.1 200 OK\r\nx-goog-upload-url: http://127.0.0.1:"
            + std::to_string(upload.port()) + "/secret\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        responses.push_back(audio_http_error(400));
        MockHttpServer server(std::move(responses));
        configure(server.port()); activate(); upload.start(); server.start();
        (void)application->start_background_note_audio(note.id, "Kore", {"Paragraph"}, epoch);
        ASSERT_TRUE(finished()); upload.join(); server.join();
        EXPECT_TRUE(waits_snapshot().empty());
        EXPECT_EQ(server.requests().size(), creation ? 2u : 1u);
        EXPECT_EQ(upload.requests().size(), creation ? 1u : 0u);
        const auto event = application->list_events(epoch).front();
        EXPECT_EQ(event.severity, EventSeverity::error);
        EXPECT_NE(event.message.find(creation ? "Audio batch creation failed" : "Audio upload failed"), std::string::npos);
        EXPECT_NE(event.message.find("HTTP 400"), std::string::npos);
    }
}

TEST_F(NoteAudioBackground, SuccessfulMaintenanceResumesTheRegisteredBatchOnce) {
    for (const bool delete_forum : {false, true}) {
        std::string forum;
        if (delete_forum) {
            forum = application->create_forum({.display_name = "Disposable", .persona_id = "reader"}, epoch).summary.id;
        }
        MockHttpServer server({audio_json(audio_operation()), audio_json(audio_operation()),
            http_response("application/jsonl", audio_results())});
        server.pause_before_response(1);
        const auto attempt = saved_attempt(server.port());
        server.start(); activate();
        ASSERT_TRUE(server.wait_for_requests(1, 2s));
        const auto old_epoch = epoch;
        if (delete_forum) application->delete_forum(forum, epoch);
        else (void)application->update_vault("Test", {.display_name = "Test"}, epoch);
        epoch = application->context_epoch();
        EXPECT_GT(epoch, old_epoch);
        EXPECT_EQ(application->check_context(old_epoch), ErrorCode::vault_changed);
        ASSERT_TRUE(application->store().get_note_audio_attempt_for_note(note.id));
        EXPECT_EQ(application->store().get_note_audio_attempt_for_note(note.id)->id, attempt.id);
        server.resume_responses();
        ASSERT_TRUE(finished()); server.join();
        ASSERT_EQ(server.requests().size(), 3u);
        EXPECT_TRUE(server.requests()[0].starts_with("GET /v1beta/" + audio_batch));
        EXPECT_TRUE(server.requests()[1].starts_with("GET /v1beta/" + audio_batch));
        EXPECT_TRUE(server.requests()[2].starts_with("GET /download/"));
        EXPECT_EQ(application->get_note_audio_info(note.id, epoch)["count"], 1);
        EXPECT_EQ(completed_epoch.load(), epoch);
    }
    EXPECT_EQ(completions.load(), 2);
    const auto events = application->list_events(epoch);
    EXPECT_EQ(events.size(), 4u);
    EXPECT_EQ(std::count_if(events.begin(), events.end(), [](const auto& event) { return event.severity == EventSeverity::info; }), 2);
    EXPECT_EQ(std::count_if(events.begin(), events.end(), [](const auto& event) { return event.severity == EventSeverity::success; }), 2);
}

TEST_F(NoteAudioBackground, SuccessfulMaintenanceRetiresAnInterruptedSubmissionWithoutAnotherPost) {
    MockHttpServer server({"HTTP/1.1 200 OK\r\nx-goog-upload-url: http://127.0.0.1:1/secret\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"});
    server.pause_before_response(1);
    configure(server.port()); activate(); server.start();
    (void)application->start_background_note_audio(note.id, "Kore", {"Paragraph"}, epoch);
    ASSERT_TRUE(server.wait_for_requests(1, 2s));
    (void)application->update_vault("Test", {.display_name = "Test"}, epoch);
    epoch = application->context_epoch();
    server.resume_responses(); server.join();
    ASSERT_TRUE(finished());
    EXPECT_EQ(server.requests().size(), 1u);
    const auto events = application->list_events(epoch);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].severity, EventSeverity::warning);
    EXPECT_NE(events[0].message.find("possibly before submission started"), std::string::npos);
    EXPECT_NE(events[0].message.find("If it was submitted, starting again may generate audio twice"), std::string::npos);
    EXPECT_EQ(events[0].message.find("CHA stopped"), std::string::npos);
}

TEST_F(NoteAudioBackground, VaultSwitchesRecoverOnlyTheCurrentAttemptWithOverlappingIds) {
    MockHttpServer origin({audio_json(audio_operation()), audio_json(audio_operation()),
        http_response("application/jsonl", audio_results())});
    auto other_operation = audio_operation();
    const auto other_batch = "batches/" + std::string(32, 'c');
    other_operation["name"] = other_batch;
    MockHttpServer other({audio_json(other_operation), http_response("application/jsonl", audio_results())});
    origin.pause_before_response(1); other.pause_before_response(1);
    (void)application->create_vault({.display_name = "Other"}, epoch);
    epoch = application->switch_vault("Other", {}, epoch).context_epoch;
    const auto other_note = application->create_note_item({}, NoteItemKind::note, "Other chapter", epoch);
    (void)application->set_note_read_only(other_note.id, true, epoch);
    (void)application->replace_api_key_value(key_id, "other-key", epoch);
    const auto other_attempt = *application->store().accept_note_audio(other_note.id, 1,
        "http://127.0.0.1:" + std::to_string(other.port()), key_id, [](const auto&) {});
    ASSERT_TRUE(application->store().register_note_audio(other_attempt.id, other_batch, "Kore"));
    epoch = application->switch_vault("Test", {}, epoch).context_epoch;
    const auto original_attempt = saved_attempt(origin.port());
    EXPECT_EQ(note.id, other_note.id);
    EXPECT_EQ(original_attempt.id, other_attempt.id);
    origin.start(); other.start(); activate();
    ASSERT_TRUE(origin.wait_for_requests(1, 2s));
    const auto original_epoch = epoch;
    epoch = application->switch_vault("Other", {}, epoch).context_epoch;
    ASSERT_TRUE(other.wait_for_requests(1, 2s));
    // The old reply is complete, but its worker no longer owns this context.
    origin.resume_responses();
    EXPECT_TRUE(application->background_note_audio_status(other_note.id, epoch)["pending"]);
    EXPECT_EQ(application->get_note_audio_info(other_note.id, epoch)["count"], 0);
    EXPECT_EQ(completions.load(), 0);
    other.resume_responses();
    ASSERT_TRUE(finished()); other.join();
    EXPECT_EQ(completions.load(), 1);
    EXPECT_EQ(completed_id.load(), other_note.id);
    EXPECT_EQ(completed_epoch.load(), epoch);
    ASSERT_EQ(application->list_events(epoch).size(), 2u);
    EXPECT_NE(application->list_events(epoch)[0].message.find("Other chapter"), std::string::npos);
    epoch = application->switch_vault("Test", {}, epoch).context_epoch;
    EXPECT_GT(epoch, original_epoch);
    ASSERT_TRUE(finished()); origin.join();
    EXPECT_EQ(completions.load(), 2);
    EXPECT_EQ(completed_epoch.load(), epoch);
    EXPECT_EQ(application->get_note_audio_info(note.id, epoch)["count"], 1);
    ASSERT_EQ(application->list_events(epoch).size(), 2u);
    EXPECT_NE(application->list_events(epoch)[0].message.find("“Chapter”"), std::string::npos);
    ASSERT_EQ(origin.requests().size(), 3u);
    ASSERT_EQ(other.requests().size(), 2u);
    EXPECT_TRUE(origin.requests()[0].starts_with("GET /v1beta/" + audio_batch));
    EXPECT_TRUE(origin.requests()[1].starts_with("GET /v1beta/" + audio_batch));
    for (const auto& request : origin.requests()) {
        EXPECT_TRUE(request.starts_with("GET "));
        EXPECT_NE(request.find("old-key"), std::string::npos);
        EXPECT_EQ(request.find("other-key"), std::string::npos);
    }
    for (const auto& request : other.requests()) {
        EXPECT_TRUE(request.starts_with("GET "));
        EXPECT_NE(request.find("other-key"), std::string::npos);
    }
}

TEST_F(NoteAudioBackground, SwitchingBackRetiresAnUnregisteredAttemptOnlyInItsOriginatingVault) {
    (void)application->create_vault({.display_name = "Other"}, epoch);
    MockHttpServer server({"HTTP/1.1 200 OK\r\nx-goog-upload-url: http://127.0.0.1:1/secret\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"});
    server.pause_before_response(1);
    configure(server.port()); activate(); server.start();
    (void)application->start_background_note_audio(note.id, "Kore", {"Paragraph"}, epoch);
    ASSERT_TRUE(server.wait_for_requests(1, 2s));
    epoch = application->switch_vault("Other", {}, epoch).context_epoch;
    EXPECT_FALSE(application->background_note_audio_status(note.id, epoch)["pending"]);
    EXPECT_TRUE(application->list_events(epoch).empty());
    server.resume_responses(); server.join();
    epoch = application->switch_vault("Test", {}, epoch).context_epoch;
    ASSERT_TRUE(finished());
    const auto events = application->list_events(epoch);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].severity, EventSeverity::warning);
    EXPECT_NE(events[0].message.find("“Chapter”"), std::string::npos);
    EXPECT_NE(events[0].message.find("If it was submitted, starting again may generate audio twice"), std::string::npos);
    EXPECT_EQ(server.requests().size(), 1u);
    EXPECT_EQ(completions.load(), 0);
}

TEST_F(NoteAudioBackground, UploadEtagPublicationDoesNotResetTheRecoveryWarning) {
    const std::string etag = "HTTP/1.1 200 OK\r\nETag: \"saved\"\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    MockHttpServer backup({http_response("text/plain", ""), etag, etag});
    backup.pause_before_response(2);
    (void)application->save_r2_storage({.display_name = "Backups",
        .url = "http://127.0.0.1:" + std::to_string(backup.port()) + "/bucket/",
        .access_key_id = "access", .secret_key = "secret"}, epoch);
    activate(); backup.start();
    auto uploading = std::async(std::launch::async, [this, starting_epoch = epoch] {
        return application->upload_database({}, starting_epoch);
    });
    const bool upload_started = backup.wait_for_requests(2, 2s);
    if (!upload_started) {
        backup.resume_responses();
        (void)uploading.get();
        FAIL() << "The backup upload did not start";
    }
    epoch = application->context_epoch();
    MockHttpServer server({audio_http_error(401), audio_http_error(401), audio_json(audio_operation()),
        http_response("application/jsonl", audio_results())});
    auto warned = std::make_shared<std::promise<void>>();
    auto warning_ready = warned->get_future();
    std::promise<void> release;
    on_wait = [warned, released = release.get_future().share()](auto index) {
        if (index == 1) { warned->set_value(); released.wait(); }
    };
    saved_attempt(server.port()); server.start(); activate();
    const auto warning_status = warning_ready.wait_for(2s);
    backup.resume_responses();
    (void)uploading.get(); backup.join();
    release.set_value();
    ASSERT_EQ(warning_status, std::future_status::ready);
    ASSERT_TRUE(finished()); server.join();
    const auto events = application->list_events(epoch);
    EXPECT_EQ(std::count_if(events.begin(), events.end(), [](const auto& event) { return event.severity == EventSeverity::warning; }), 1);
    const auto warning = std::find_if(events.begin(), events.end(), [](const auto& event) { return event.severity == EventSeverity::warning; });
    ASSERT_NE(warning, events.end());
    EXPECT_NE(warning->message.find("HTTP 401"), std::string::npos);
    EXPECT_NE(warning->message.find("Settings → API keys"), std::string::npos);
    EXPECT_NE(log_text().find("HTTP 401"), std::string::npos);
    EXPECT_EQ(log_text().find("old-key"), std::string::npos);
}

TEST_F(NoteAudioBackground, PostCommitAudioReadFailureDoesNotRejectTheNoteChange) {
    for (const bool remove : {false, true}) {
        (void)application->set_note_read_only(note.id, true, epoch);
        MockHttpServer server({audio_json(audio_operation())});
        server.pause_before_response(1);
        saved_attempt(server.port()); server.start(); activate();
        ASSERT_TRUE(server.wait_for_requests(1, 2s));
        force_next_workspace_config_fault(WorkspaceConfigFault::note_audio_read);
        if (remove) EXPECT_NO_THROW(application->delete_note_item(note.id, epoch));
        else EXPECT_NO_THROW((void)application->set_note_read_only(note.id, false, epoch));
        ASSERT_TRUE(finished());
        server.resume_responses(); server.join();
        const auto events = application->list_events(epoch);
        EXPECT_EQ(events[0].severity, EventSeverity::warning);
        EXPECT_NE(events[0].message.find("abandoned"), std::string::npos);
        EXPECT_EQ(completions.load(), 0);
    }
    EXPECT_TRUE(application->list_note_items(epoch).empty());
    EXPECT_NE(log_text().find("after the note change committed"), std::string::npos);
}

TEST_F(NoteAudioBackground, MainDaemonDoesNotRecoverEvenWithACallback) {
    const auto attempt = saved_attempt(1, false);
    application.reset();
    auto command = make_command(workspace, path); command.chaweb_host = true;
    application = Application::open(command);
    activate();
    EXPECT_EQ(application->store().get_note_audio_attempt_for_note(note.id)->id, attempt.id);
    EXPECT_TRUE(application->list_events(application->context_epoch()).empty());
}

#endif

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
