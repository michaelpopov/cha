#include "app/application.h"
#include "storage/sqlite_storage.h"
#include "support/test_workspace.h"
#include "workspace/workspace_config_store.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <set>
#include <iomanip>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cha::app {
namespace {

using namespace std::chrono_literals;

ApplicationCommand make_command(
    const test::TestWorkspace& workspace, const std::filesystem::path& database) {
    const std::filesystem::path config_directory = workspace.root() / "cha-config";
    std::filesystem::create_directories(config_directory);
    {
        std::ofstream app(config_directory / "app.toml");
        app << "vault = \"Test\"\n[logging]\nfile = \"runtime.log\"\nlevel = \"off\"\n";
    }
    {
        std::ofstream vault(config_directory / "test.toml");
        vault << "vault_name = \"Test\"\ndata = " << std::quoted(database.string()) << "\n";
    }
    const auto loaded = load_configuration_directory(config_directory);
    const auto* vault = find_vault(loaded.vaults, loaded.startup_vault);
    return {
        .config_directory = loaded.directory,
        .vaults = loaded.vaults,
        .vault = *vault,
        .log_file = loaded.log_file,
        .log_level = loaded.log_level,
        .warnings = loaded.warnings,
    };
}

nlohmann::json text_doc(std::string_view text) {
    return {{"type", "doc"}, {"content", nlohmann::json::array({
        {{"type", "paragraph"}, {"content", nlohmann::json::array({{{"type", "text"}, {"text", text}}})}},
    })}};
}

FolderStoreHttpResponse json_ok(std::string body, std::vector<FolderStoreHttpHeader> headers = {}) {
    FolderStoreHttpResponse response;
    response.status = 200;
    response.body = std::move(body);
    response.headers = std::move(headers);
    return response;
}

class Gate {
public:
    ~Gate() { release(); }
    void arrive_and_wait() {
        std::unique_lock lock(mutex_);
        entered_ = true;
        cv_.notify_all();
        cv_.wait_for(lock, 5s, [&] { return released_; });
    }
    bool wait_entered() {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, 5s, [&] { return entered_; });
    }
    void release() {
        { std::lock_guard lock(mutex_); released_ = true; }
        cv_.notify_all();
    }
private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool entered_{};
    bool released_{};
};

class FolderStoreOperationsTest : public testing::Test {
protected:
    void SetUp() override {
        database_ = test::import_test_database(workspace_.root());
        RuntimeSettings settings;
        settings.folder_store_poll_interval = 0ms;
        settings.folder_store_worker_finished = [this](std::int64_t id) {
            { std::lock_guard lock(finished_mutex_); finished_.insert(id); }
            finished_cv_.notify_all();
        };
        settings.folder_store_wait = [this](int) {
            {
                std::lock_guard lock(wait_mutex_);
                ++waits_;
            }
            wait_cv_.notify_all();
            if (hold_wait_.valid()) hold_wait_.wait_for(5s);
        };
        application_ = Application::open(make_command(workspace_, database_), {}, settings);
        epoch_ = application_->context_epoch();
        key_ = application_->create_api_key({.display_name = "OpenAI", .value = "sk-test"}, epoch_).id;
        auto provider = application_->get_provider("test", epoch_);
        nlohmann::json body = provider;
        for (const char* field : {"id", "used_by", "writable"}) body.erase(field);
        body["mode"] = "net";
        body["host"] = "api.openai.com";
        body["port"] = 443;
        body["https"] = true;
        body["api"] = "responses";
        body["auth"] = "none";
        body["base_path"] = "";
        body["api_key"] = key_;
        (void)application_->update_provider("test", body, epoch_);
        folder_ = application_->create_note_item({}, NoteItemKind::folder, "Research", epoch_);
        const auto note = application_->create_note_item(folder_.id, NoteItemKind::note, "Log", epoch_);
        application_->save_note(note.id, text_doc("snapshot fact"), epoch_);
    }

    void TearDown() override {
        if (application_) {
            application_->request_shutdown();
            (void)application_->join_shutdown(2s);
        }
    }

    void execute_sql(std::string_view sql) {
        storage::SqliteDatabase db(database_, storage::SqliteDatabase::Mode::read_write);
        db.execute(sql);
    }

    Application::FolderStoreOperationStatus wait_status(std::int64_t folder_id) {
        const auto initial = application_->folder_store_operation_status(folder_id, epoch_);
        if (!initial.present) return initial;
        std::unique_lock lock(finished_mutex_);
        if (!finished_cv_.wait_for(lock, 5s, [&] { const auto phase = application_->folder_store_operation_status(folder_id, epoch_).phase;
            return finished_.contains(initial.operation_id) && (phase == "completed" || phase == "failed" || phase == "cancelled" || phase == "warning"); })) {
            ADD_FAILURE() << "Folder store worker did not finish";
        }
        lock.unlock();
        return application_->folder_store_operation_status(folder_id, epoch_);
    }

    FolderStoreTransport openai_transport(bool fail_index = false, bool not_found = false) {
        return [=](const FolderStoreHttpRequest& request, const std::atomic_bool&) {
            if (request.method == "DELETE") {
                if (not_found) {
                    FolderStoreHttpResponse response;
                    response.status = 404;
                    response.body = R"({"error":{"message":"missing","type":"invalid_request_error"}})";
                    return response;
                }
                return json_ok(R"({"deleted":true})");
            }
            if (request.url.find("/vector_stores") != std::string::npos
                && request.method == "POST" && request.url.find("/files") == std::string::npos) {
                return json_ok(R"({"id":"vs_1","object":"vector_store","name":"Research ##00000B","status":"completed"})");
            }
            if (request.multipart.size() == 2) return json_ok(R"({"id":"file_1"})");
            if (request.url.find("/files/file_1") != std::string::npos) {
                return json_ok(fail_index ? R"({"status":"failed"})" : R"({"status":"completed"})");
            }
            return json_ok(R"({"id":"vsf_1","status":"in_progress"})");
        };
    }

    std::int64_t seed_store(std::int64_t folder, std::optional<std::string> remote = "vs_1",
        std::vector<std::string> files = {"file_1"}, bool unknown = false) {
        storage::SqliteDatabase db(database_, storage::SqliteDatabase::Mode::read_write);
        storage::SqliteTransaction tx(db);
        const auto id = folder_store_register(db, folder, FolderStoreService::openai, key_, 1);
        if (remote) folder_store_save_remote_id(db, id, *remote);
        for (const auto& file : files) folder_store_add_file_id(db, id, file);
        if (unknown) folder_store_set_uncertainty(db, id, true);
        tx.commit();
        return id;
    }

    void reopen(RuntimeSettings settings) {
        application_->request_shutdown();
        EXPECT_TRUE(application_->join_shutdown(2s));
        application_.reset();
        settings.folder_store_worker_finished = [this](std::int64_t id) {
            { std::lock_guard lock(finished_mutex_); finished_.insert(id); }
            finished_cv_.notify_all();
        };
        { std::lock_guard lock(finished_mutex_); finished_.clear(); }
        application_ = Application::open(make_command(workspace_, database_), {}, settings);
        epoch_ = application_->context_epoch();
    }

    test::TestWorkspace workspace_;
    std::filesystem::path database_;
    std::unique_ptr<Application> application_;
    std::uint64_t epoch_{};
    std::string key_;
    NoteItem folder_;
    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
    int waits_{};
    std::shared_future<void> hold_wait_;
    std::mutex finished_mutex_;
    std::condition_variable finished_cv_;
    std::set<std::int64_t> finished_;
};

TEST_F(FolderStoreOperationsTest, UploadsAndRemovesThroughOpenAiFakeHttp) {
    application_->set_folder_store_transport_for_tests(openai_transport());
    const auto started = application_->start_folder_store_upload(
        folder_.id, "openai", key_, epoch_);
    EXPECT_GT(started.operation_id, 0);
    const auto status = wait_status(folder_.id);
    ASSERT_TRUE(status.present);
    EXPECT_EQ(status.phase, "completed");
    EXPECT_EQ(status.result, "success");
    const auto record = application_->folder_store_record(folder_.id, epoch_);
    ASSERT_TRUE(record);
    EXPECT_EQ(record->state, FolderStoreState::ready);
    EXPECT_EQ(record->remote_store_id, "vs_1");
    const auto removed = application_->start_folder_store_remove(folder_.id, record->id, epoch_);
    const auto done = wait_status(folder_.id);
    ASSERT_TRUE(done.present);
    EXPECT_EQ(done.phase, "completed");
    EXPECT_EQ(done.result, "success");
    EXPECT_FALSE(application_->folder_store_record(folder_.id, epoch_));
    (void)removed;
}

TEST_F(FolderStoreOperationsTest, PreparationFailureIsDistinctAndLeavesNoStore) {
    const auto empty = application_->create_note_item({}, NoteItemKind::folder, "Empty", epoch_);
    application_->set_folder_store_transport_for_tests(openai_transport());
    const auto started = application_->start_folder_store_upload(empty.id, "openai", key_, epoch_);
    const auto status = wait_status(empty.id);
    EXPECT_EQ(status.result, "preflight");
    EXPECT_FALSE(application_->folder_store_record(empty.id, epoch_));
    (void)started;
}

TEST_F(FolderStoreOperationsTest, IndexFailureCleansOwnedFiles) {
    std::vector<std::string> deletes;
    application_->set_folder_store_transport_for_tests(
        [&](const FolderStoreHttpRequest& request, const std::atomic_bool& cancel) {
            if (request.method == "DELETE") deletes.push_back(request.url);
            if (request.method == "GET" && request.url.ends_with("/files/file_1")) {
                return json_ok(R"({"status":"failed","last_error":{"code":"invalid_file","message":"private note text"}})");
            }
            return openai_transport(true)(request, cancel);
        });
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, epoch_);
    const auto status = wait_status(folder_.id);
    EXPECT_EQ(status.phase, "failed");
    EXPECT_NE(status.error.find("Note “Log”"), std::string::npos);
    EXPECT_NE(status.error.find("Check the note content before uploading again."), std::string::npos);
    EXPECT_EQ(status.error.find("private note text"), std::string::npos);
    EXPECT_FALSE(application_->folder_store_record(folder_.id, epoch_));
    ASSERT_GE(deletes.size(), 2U);
}

TEST_F(FolderStoreOperationsTest, StructuredNotFoundWarnsAndGeneric404KeepsTheId) {
    application_->set_folder_store_transport_for_tests(openai_transport());
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, epoch_);
    wait_status(folder_.id);
    auto record = application_->folder_store_record(folder_.id, epoch_);
    ASSERT_TRUE(record);
    application_->set_folder_store_transport_for_tests(openai_transport(false, true));
    (void)application_->start_folder_store_remove(folder_.id, record->id, epoch_);
    const auto warned = wait_status(folder_.id);
    ASSERT_TRUE(warned.present);
    EXPECT_EQ(warned.phase, "warning");
    EXPECT_EQ(warned.result, "warning");
    EXPECT_FALSE(application_->folder_store_record(folder_.id, epoch_));
    const auto events = application_->list_events(epoch_);
    ASSERT_FALSE(events.empty());

    const auto folder2 = application_->create_note_item({}, NoteItemKind::folder, "Other", epoch_);
    const auto note = application_->create_note_item(folder2.id, NoteItemKind::note, "N", epoch_);
    application_->save_note(note.id, text_doc("x"), epoch_);
    application_->set_folder_store_transport_for_tests(openai_transport());
    (void)application_->start_folder_store_upload(folder2.id, "openai", key_, epoch_);
    wait_status(folder2.id);
    record = application_->folder_store_record(folder2.id, epoch_);
    ASSERT_TRUE(record);
    application_->set_folder_store_transport_for_tests([](const FolderStoreHttpRequest& request, const std::atomic_bool&) {
        FolderStoreHttpResponse response;
        if (request.method == "DELETE") {
            response.status = 404;
            response.body = "<html>nope</html>";
            return response;
        }
        response.status = 200;
        response.body = R"({"id":"x","status":"completed"})";
        return response;
    });
    (void)application_->start_folder_store_remove(folder2.id, record->id, epoch_);
    wait_status(folder2.id);
    EXPECT_TRUE(application_->folder_store_record(folder2.id, epoch_));
}

TEST_F(FolderStoreOperationsTest, CancelDuringIndexingCleansUp) {
    std::promise<void> hold;
    hold_wait_ = hold.get_future().share();
    application_->set_folder_store_transport_for_tests(
        [](const FolderStoreHttpRequest& request, const std::atomic_bool&) {
            FolderStoreHttpResponse response;
            response.status = 200;
            if (request.method == "DELETE") return json_ok(R"({"deleted":true})");
            if (request.url.find("/vector_stores") != std::string::npos
                && request.url.find("/files") == std::string::npos && request.method == "POST") {
                return json_ok(R"({"id":"vs_1","object":"vector_store","name":"Research ##00000B","status":"completed"})");
            }
            if (request.multipart.size() == 2) return json_ok(R"({"id":"file_1"})");
            if (request.url.find("/files/file_1") != std::string::npos) {
                return json_ok(R"({"status":"in_progress"})");
            }
            return json_ok(R"({"id":"vsf_1","status":"in_progress"})");
        });
    const auto started = application_->start_folder_store_upload(folder_.id, "openai", key_, epoch_);
    {
        std::unique_lock lock(wait_mutex_);
        ASSERT_TRUE(wait_cv_.wait_for(lock, 2s, [&] { return waits_ > 0; }));
    }
    application_->cancel_folder_store_upload(started.operation_id, epoch_);
    hold.set_value();
    const auto status = wait_status(folder_.id);
    ASSERT_TRUE(status.present);
    EXPECT_EQ(status.phase, "cancelled");
    EXPECT_EQ(status.result, "cancelled");
    EXPECT_FALSE(application_->folder_store_record(folder_.id, epoch_));
}

TEST_F(FolderStoreOperationsTest, FolderDeleteRemovesIndependentChildStores) {
    application_->set_folder_store_transport_for_tests(openai_transport());
    const auto child = application_->create_note_item(folder_.id, NoteItemKind::folder, "Child", epoch_);
    const auto child_note = application_->create_note_item(child.id, NoteItemKind::note, "Child note", epoch_);
    application_->save_note(child_note.id, text_doc("child"), epoch_);
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, epoch_);
    wait_status(folder_.id);
    (void)application_->start_folder_store_upload(child.id, "openai", key_, epoch_);
    wait_status(child.id);
    EXPECT_EQ(application_->folder_store_count_in_subtree(folder_.id, epoch_), 2);
    EXPECT_THROW(application_->delete_note_item(folder_.id, epoch_), ApplicationError);
    const auto started = application_->start_folder_delete(folder_.id, 2, epoch_);
    wait_status(folder_.id);
    EXPECT_EQ(application_->folder_store_count_in_subtree(folder_.id, epoch_), 0);
    (void)started;
}

TEST_F(FolderStoreOperationsTest, ChangedConfirmationCountDoesNotDelete) {
    application_->set_folder_store_transport_for_tests(openai_transport());
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, epoch_);
    wait_status(folder_.id);
    EXPECT_THROW((void)application_->start_folder_delete(folder_.id, 99, epoch_), ApplicationError);
    EXPECT_TRUE(application_->folder_store_record(folder_.id, epoch_));
}

TEST_F(FolderStoreOperationsTest, DrainDoesNotJoinAWorkerWaitingForTheLifecycleLock) {
    auto gate = std::make_shared<Gate>();
    RuntimeSettings settings;
    settings.folder_store_before_lifetime_lock = [gate] { gate->arrive_and_wait(); };
    reopen(settings);
    application_->set_folder_store_transport_for_tests(openai_transport());
    const auto operation = application_->start_folder_store_upload(folder_.id, "openai", key_, epoch_);
    ASSERT_TRUE(gate->wait_entered());
    const auto started = std::chrono::steady_clock::now();
    (void)application_->create_vault({.display_name = "Other"}, epoch_);
    (void)application_->switch_vault("Other", {}, epoch_);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
    gate->release();
    epoch_ = application_->context_epoch();
    std::unique_lock lock(finished_mutex_);
    EXPECT_TRUE(finished_cv_.wait_for(lock, 5s, [&] { return finished_.contains(operation.operation_id); }));
}

TEST_F(FolderStoreOperationsTest, ChildVaultCopyClearsOwnershipAndKeepsFolderRefs) {
    application_->set_folder_store_transport_for_tests(openai_transport());
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, epoch_);
    wait_status(folder_.id);
    {
        storage::SqliteDatabase source(database_, storage::SqliteDatabase::Mode::read_write);
        source.execute("INSERT INTO sessions (forum_key,session_id,label,updated_at,history_epoch,next_entry_id,next_request_id) SELECT forum_key,'store-ref','Store ref',0,1,1,1 FROM forums LIMIT 1");
        source.prepare("INSERT INTO session_folders (session_key, folder_id) SELECT session_key, ?1 FROM sessions LIMIT 1", folder_.id).run();
    }
    const auto copy = application_->create_vault(
        {.display_name = "Child", .copy_from = "Test"}, epoch_);
    storage::SqliteDatabase destination(copy.data, storage::SqliteDatabase::Mode::read_only);
    auto stores = destination.prepare("SELECT COUNT(*) FROM folder_stores");
    ASSERT_TRUE(stores.step());
    EXPECT_EQ(stores.integer(0), 0);
    auto refs = destination.prepare("SELECT folder_id FROM session_folders");
    ASSERT_TRUE(refs.step());
    EXPECT_EQ(refs.integer(0), folder_.id);
    EXPECT_TRUE(application_->folder_store_record(folder_.id, epoch_));
}

TEST_F(FolderStoreOperationsTest, AllocationRecordsUncertaintyAndIdsBeforeNextRequest) {
    int allocations = 0;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        const auto record = application_->folder_store_record(folder_.id, epoch_);
        EXPECT_TRUE(record);
        if (request.url.ends_with("/vector_stores") || !request.multipart.empty()) {
            EXPECT_TRUE(record->remote_outcome_unknown);
            ++allocations;
        } else if (request.method == "POST") {
            EXPECT_EQ(record->remote_store_id, "vs_1");
            EXPECT_EQ(record->file_ids, std::vector<std::string>({"file_1"}));
            EXPECT_FALSE(record->remote_outcome_unknown);
        }
        return openai_transport()(request, cancel);
    });
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, epoch_);
    EXPECT_EQ(wait_status(folder_.id).result, "success");
    EXPECT_EQ(allocations, 2);
}

TEST_F(FolderStoreOperationsTest, LostAllocationResponseStopsWithoutAutomaticReplay) {
    int calls = 0;
    application_->set_folder_store_transport_for_tests([&](const auto&, const auto&) {
        ++calls;
        FolderStoreHttpResponse response;
        response.error = "secret-key echoed with note body";
        return response;
    });
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, epoch_);
    const auto status = wait_status(folder_.id);
    EXPECT_EQ(status.result, "error");
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(status.known_cleanup_remains);
    EXPECT_NE(status.error.find("The store connection failed."), std::string::npos);
    EXPECT_NE(status.error.find("Some remote data could not be confirmed deleted."), std::string::npos);
    EXPECT_FALSE(application_->folder_store_record(folder_.id, epoch_));
    EXPECT_EQ(status.error.find("secret-key"), std::string::npos);
    const auto events = application_->list_events(epoch_);
    EXPECT_TRUE(std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.message.find("Unconfirmed allocation") != std::string::npos;
    }));
}

TEST_F(FolderStoreOperationsTest, UnpersistedReturnedIdIsStillCleaned) {
    std::vector<std::string> deletes;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        auto response = openai_transport()(request, cancel);
        if (request.url.ends_with("/vector_stores") && request.method == "POST")
            execute_sql("CREATE TRIGGER reject_id BEFORE UPDATE OF remote_store_id ON folder_stores "
                "BEGIN SELECT RAISE(ABORT, 'write failure'); END");
        if (request.method == "DELETE") { execute_sql("DROP TRIGGER IF EXISTS reject_id"); deletes.push_back(request.url); }
        return response;
    });
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, epoch_);
    EXPECT_EQ(wait_status(folder_.id).result, "error");
    EXPECT_FALSE(application_->folder_store_record(folder_.id, epoch_));
    ASSERT_EQ(deletes.size(), 1U);
    EXPECT_TRUE(deletes.front().ends_with("/vs_1"));
}

TEST_F(FolderStoreOperationsTest, FailedPersistenceAndCleanupRetainTheReturnedIdInEvents) {
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        if (request.method == "DELETE") { execute_sql("DROP TRIGGER IF EXISTS reject_id"); return FolderStoreHttpResponse{.status = 403}; }
        auto response = openai_transport()(request, cancel);
        execute_sql("CREATE TRIGGER reject_id BEFORE UPDATE OF remote_store_id ON folder_stores "
            "BEGIN SELECT RAISE(ABORT, 'write failure'); END");
        return response;
    });
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, epoch_);
    const auto status = wait_status(folder_.id);
    EXPECT_TRUE(status.known_cleanup_remains);
    EXPECT_EQ(status.error.find("vs_1"), std::string::npos);
    const auto events = application_->list_events(epoch_);
    EXPECT_TRUE(std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.message.find("vs_1") != std::string::npos;
    }));
    const auto record = application_->folder_store_record(folder_.id, epoch_);
    ASSERT_TRUE(record);
    EXPECT_EQ(record->remote_store_id, "vs_1");
}

TEST_F(FolderStoreOperationsTest, CancellationLetsSentAllocationFinishThenCleansWithSeparateFlag) {
    Gate gate;
    int deleted = 0;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        if (request.method == "POST" && request.url.ends_with("/vector_stores")) gate.arrive_and_wait();
        if (request.method == "DELETE") { EXPECT_FALSE(cancel.load()); ++deleted; }
        return openai_transport()(request, cancel);
    });
    const auto operation = application_->start_folder_store_upload(folder_.id, "openai", key_, epoch_);
    ASSERT_TRUE(gate.wait_entered());
    EXPECT_EQ(application_->folder_store_operation_status(folder_.id, epoch_).phase, "uploading");
    application_->cancel_folder_store_upload(operation.operation_id, epoch_);
    // The allocation is still blocked, so the worker cannot have entered cleanup.
    for (int poll = 0; poll < 3; ++poll) {
        const auto cancelling = application_->folder_store_operation_status(folder_.id, epoch_);
        EXPECT_EQ(cancelling.phase, "cleaning");
        EXPECT_FALSE(cancelling.terminal);
    }
    EXPECT_THROW(application_->close_folder_store_operation(operation.operation_id, epoch_), ApplicationError);
    gate.release();
    EXPECT_EQ(wait_status(folder_.id).result, "cancelled");
    EXPECT_EQ(deleted, 1);
    EXPECT_FALSE(application_->folder_store_record(folder_.id, epoch_));
}

TEST_F(FolderStoreOperationsTest, PreflightReservationProtectsAncestors) {
    Gate gate;
    RuntimeSettings settings;
    settings.folder_store_worker_started = [&](auto, auto) { gate.arrive_and_wait(); };
    reopen(settings);
    const auto parent = application_->create_note_item({}, NoteItemKind::folder, "Parent", epoch_);
    const auto child = application_->create_note_item(parent.id, NoteItemKind::folder, "Child", epoch_);
    application_->set_folder_store_transport_for_tests(openai_transport());
    const auto operation = application_->start_folder_store_upload(child.id, "openai", key_, epoch_);
    ASSERT_TRUE(gate.wait_entered());
    EXPECT_THROW(application_->delete_note_item(parent.id, epoch_), ApplicationError);
    EXPECT_THROW((void)application_->start_folder_store_upload(child.id, "openai", key_, epoch_), ApplicationError);
    application_->cancel_folder_store_upload(operation.operation_id, epoch_);
    gate.release();
    EXPECT_EQ(wait_status(child.id).result, "cancelled");
    application_->delete_note_item(parent.id, epoch_);
}

TEST_F(FolderStoreOperationsTest, ReadyCommitWinsOverLaterCancellationAndCloseAcknowledgesStatus) {
    application_->set_folder_store_transport_for_tests(openai_transport());
    const auto operation = application_->start_folder_store_upload(folder_.id, "openai", key_, epoch_);
    ASSERT_EQ(wait_status(folder_.id).result, "success");
    application_->cancel_folder_store_upload(operation.operation_id, epoch_);
    EXPECT_EQ(application_->folder_store_record(folder_.id, epoch_)->state, FolderStoreState::ready);
    application_->close_folder_store_operation(operation.operation_id, epoch_);
    EXPECT_FALSE(application_->folder_store_operation_status(folder_.id, epoch_).present);
}

TEST_F(FolderStoreOperationsTest, CleanupAttemptsEveryFileAndRetryUsesOnlyRemainingIds) {
    const auto id = seed_store(folder_.id, "vs_1", {"file_1", "file_2"});
    std::vector<std::string> deletes;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto&) {
        deletes.push_back(request.url);
        FolderStoreHttpResponse response;
        response.status = request.url.ends_with("file_1") ? 401 : 200;
        response.body = R"({"deleted":true})";
        return response;
    });
    (void)application_->start_folder_store_remove(folder_.id, id, epoch_);
    const auto failed = wait_status(folder_.id);
    EXPECT_TRUE(failed.known_cleanup_remains);
    EXPECT_EQ(failed.error.find("file_1"), std::string::npos);
    EXPECT_EQ(failed.error.find("vs_1"), std::string::npos);
    EXPECT_EQ(failed.error.find("file_2"), std::string::npos);
    const auto events = application_->list_events(epoch_);
    ASSERT_FALSE(events.empty());
    EXPECT_NE(events.front().message.find("file_1"), std::string::npos);
    EXPECT_EQ(events.front().message.find("vs_1"), std::string::npos);
    EXPECT_EQ(events.front().message.find("file_2"), std::string::npos);
    EXPECT_EQ(deletes.size(), 3U);
    auto record = application_->folder_store_record(folder_.id, epoch_);
    ASSERT_TRUE(record);
    EXPECT_FALSE(record->remote_store_id);
    EXPECT_EQ(record->file_ids, std::vector<std::string>({"file_1"}));
    deletes.clear();
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto&) {
        deletes.push_back(request.url);
        return json_ok(R"({"deleted":true})");
    });
    (void)application_->start_folder_store_remove(folder_.id, id, epoch_);
    EXPECT_EQ(wait_status(folder_.id).result, "success");
    ASSERT_EQ(deletes.size(), 1U);
    EXPECT_TRUE(deletes.front().ends_with("file_1"));
}

TEST_F(FolderStoreOperationsTest, EarlierNotFoundWarningSurvivesASeparateRemoveRetry) {
    const auto id = seed_store(folder_.id);
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto&) {
        FolderStoreHttpResponse response;
        response.status = request.url.ends_with("vs_1") ? 404 : 401;
        response.body = R"({"error":{"type":"invalid_request_error","message":"No such resource"}})";
        return response;
    });
    (void)application_->start_folder_store_remove(folder_.id, id, epoch_);
    EXPECT_EQ(wait_status(folder_.id).phase, "failed");
    application_->set_folder_store_transport_for_tests(openai_transport());
    (void)application_->start_folder_store_remove(folder_.id, id, epoch_);
    EXPECT_EQ(wait_status(folder_.id).result, "success");
    const auto events = application_->list_events(epoch_);
    EXPECT_TRUE(std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.severity == EventSeverity::warning && event.message.find("was not found for") != std::string::npos;
    }));
}

TEST_F(FolderStoreOperationsTest, DeleteFailureRetainsReservationAndDeleteAnywayRollback) {
    const auto id = seed_store(folder_.id);
    application_->set_folder_store_transport_for_tests([](const auto&, const auto&) { FolderStoreHttpResponse response; response.status = 403; return response; });
    const auto operation = application_->start_folder_delete(folder_.id, 1, epoch_);
    EXPECT_EQ(wait_status(folder_.id).phase, "failed");
    EXPECT_THROW((void)application_->create_note_item(folder_.id, NoteItemKind::note, "Blocked", epoch_), ApplicationError);
    EXPECT_THROW((void)application_->start_folder_store_remove(folder_.id, id, epoch_), ApplicationError);
    execute_sql("CREATE TRIGGER reject_warning BEFORE INSERT ON event_history BEGIN SELECT RAISE(ABORT, 'warning failure'); END");
    EXPECT_THROW(application_->delete_folder_anyway(operation.operation_id, epoch_), std::exception);
    EXPECT_TRUE(application_->folder_store_record(folder_.id, epoch_));
    execute_sql("DROP TRIGGER reject_warning");
    application_->delete_folder_anyway(operation.operation_id, epoch_);
    const auto status = application_->folder_store_operation_status(folder_.id, epoch_);
    EXPECT_TRUE(status.folder_deleted);
    EXPECT_EQ(status.result, "warning");
    EXPECT_FALSE(application_->folder_store_record(folder_.id, epoch_));
    application_->close_folder_store_operation(operation.operation_id, epoch_);
}

TEST_F(FolderStoreOperationsTest, RetryDeleteUsesTheOriginalOperationIdentity) {
    seed_store(folder_.id);
    application_->set_folder_store_transport_for_tests([](const auto&, const auto&) { FolderStoreHttpResponse response; response.status = 401; return response; });
    const auto operation = application_->start_folder_delete(folder_.id, 1, epoch_);
    EXPECT_EQ(wait_status(folder_.id).phase, "failed");
    application_->set_folder_store_transport_for_tests(openai_transport());
    const auto retried = application_->retry_folder_delete(operation.operation_id, epoch_);
    EXPECT_EQ(retried.operation_id, operation.operation_id);
    EXPECT_TRUE(wait_status(folder_.id).folder_deleted);
    application_->close_folder_store_operation(operation.operation_id, epoch_);
    EXPECT_THROW(application_->delete_folder_anyway(operation.operation_id, epoch_), ApplicationError);
}

TEST_F(FolderStoreOperationsTest, UnknownOnlyDeleteNeedsNoKeyAndFinishesWithWarning) {
    seed_store(folder_.id, {}, {}, true);
    application_->delete_api_key(key_, epoch_);
    int requests = 0;
    application_->set_folder_store_transport_for_tests([&](const auto&, const auto&) { ++requests; return json_ok("{}"); });
    const auto operation = application_->start_folder_delete(folder_.id, 1, epoch_);
    const auto status = wait_status(folder_.id);
    EXPECT_TRUE(status.folder_deleted);
    EXPECT_EQ(status.result, "warning");
    EXPECT_EQ(requests, 0);
    EXPECT_THROW(application_->delete_folder_anyway(operation.operation_id, epoch_), ApplicationError);
}

TEST_F(FolderStoreOperationsTest, LaunchFailureReleasesAdmissionWithoutRemoteEffects) {
    RuntimeSettings settings;
    settings.folder_store_before_launch = [] { throw std::runtime_error("launch failure"); };
    reopen(settings);
    EXPECT_THROW((void)application_->start_folder_store_upload(folder_.id, "openai", key_, epoch_), ApplicationError);
    EXPECT_FALSE(application_->folder_store_operation_status(folder_.id, epoch_).present);
    EXPECT_FALSE(application_->folder_store_record(folder_.id, epoch_));
    application_->delete_note_item(folder_.id, epoch_);
}

TEST_F(FolderStoreOperationsTest, GoogleDocumentCleanupFailureKeepsParentForRetry) {
    std::int64_t id;
    {
        storage::SqliteDatabase db(database_, storage::SqliteDatabase::Mode::read_write);
        storage::SqliteTransaction tx(db);
        id = folder_store_register(db, folder_.id, FolderStoreService::google, key_, 2);
        folder_store_save_remote_id(db, id, "fileSearchStores/test");
        tx.commit();
    }
    bool first_deleted = false, fail_second = true, parent_deleted = false;
    int first_deletes = 0;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto&) {
        if (request.method == "GET") {
            auto documents = nlohmann::json::array();
            if (!first_deleted) documents.push_back({{"name", "fileSearchStores/test/documents/one"}});
            documents.push_back({{"name", "fileSearchStores/test/documents/two"}});
            return json_ok(nlohmann::json{{"documents", documents}}.dump());
        }
        EXPECT_EQ(request.method, "DELETE");
        if (request.url.ends_with("/documents/one?force=true")) {
            first_deleted = true;
            ++first_deletes;
        } else if (request.url.ends_with("/documents/two?force=true")) {
            if (fail_second) return FolderStoreHttpResponse{.status = 503, .body = R"({"error":{"status":"UNAVAILABLE"}})"};
        } else {
            EXPECT_TRUE(first_deleted);
            EXPECT_FALSE(fail_second);
            EXPECT_TRUE(request.url.ends_with("/fileSearchStores/test?force=true"));
            parent_deleted = true;
        }
        return json_ok("{}");
    });
    const auto first = application_->start_folder_store_remove(folder_.id, id, epoch_);
    EXPECT_EQ(wait_status(folder_.id).result, "error");
    EXPECT_TRUE(application_->folder_store_operation_status(folder_.id, epoch_).known_cleanup_remains);
    ASSERT_TRUE(application_->folder_store_record(folder_.id, epoch_));
    EXPECT_EQ(application_->folder_store_record(folder_.id, epoch_)->remote_store_id, "fileSearchStores/test");
    EXPECT_FALSE(parent_deleted);
    application_->close_folder_store_operation(first.operation_id, epoch_);

    fail_second = false;
    (void)application_->start_folder_store_remove(folder_.id, id, epoch_);
    EXPECT_EQ(wait_status(folder_.id).result, "success");
    EXPECT_EQ(first_deletes, 1);
    EXPECT_TRUE(parent_deleted);
    EXPECT_FALSE(application_->folder_store_record(folder_.id, epoch_));
}

TEST_F(FolderStoreOperationsTest, GoogleTransfersBeforePollingAndWaitsForLaggingCounts) {
    auto provider = application_->get_provider("test", epoch_);
    nlohmann::json body = provider;
    for (const char* field : {"id", "used_by", "writable"}) body.erase(field);
    body["host"] = "generativelanguage.googleapis.com";
    body["api"] = "google";
    body["base_path"] = "/v1beta";
    (void)application_->update_provider("test", body, epoch_);
    const auto second = application_->create_note_item(folder_.id, NoteItemKind::note, "Second", epoch_);
    application_->save_note(second.id, text_doc("second fact"), epoch_);
    int uploads = 0, counts = 0, polls = 0;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto&) {
        if (request.method == "DELETE") return json_ok("{}");
        if (request.url.find("/documents?") != std::string::npos) return json_ok("{}");
        if (request.url.ends_with("/v1beta/fileSearchStores")) return json_ok(R"({"name":"fileSearchStores/store1"})");
        if (request.url.ends_with(":uploadToFileSearchStore")) return json_ok("", {{"X-Goog-Upload-URL", "https://generativelanguage.googleapis.com/upload/session"}});
        if (request.url.ends_with("/upload/session")) {
            ++uploads;
            return json_ok(nlohmann::json{{"name", "fileSearchStores/store1/upload/operations/op" + std::to_string(uploads)}}.dump());
        }
        EXPECT_EQ(uploads, 2);
        if (request.url.find("/operations/") != std::string::npos) {
            ++polls;
            return json_ok(nlohmann::json{{"name", request.url.substr(std::string("https://generativelanguage.googleapis.com/v1beta/").size())}, {"done", true}}.dump());
        }
        ++counts;
        return json_ok(counts == 1 ? R"({"activeDocumentsCount":"1","pendingDocumentsCount":"1"})" : R"({"activeDocumentsCount":"2"})");
    });
    (void)application_->start_folder_store_upload(folder_.id, "google", key_, epoch_);
    EXPECT_EQ(wait_status(folder_.id).result, "success");
    EXPECT_EQ(uploads, 2);
    EXPECT_EQ(polls, 2);
    EXPECT_EQ(counts, 2);
    const auto record = application_->folder_store_record(folder_.id, epoch_);
    ASSERT_TRUE(record);
    (void)application_->start_folder_store_remove(folder_.id, record->id, epoch_);
    EXPECT_EQ(wait_status(folder_.id).result, "success");
}

TEST_F(FolderStoreOperationsTest, GoogleCancelDuringResumableStartDoesNotUploadNoteBytes) {
    nlohmann::json body = application_->get_provider("test", epoch_);
    for (const char* field : {"id", "used_by", "writable"}) body.erase(field);
    body["host"] = "generativelanguage.googleapis.com";
    body["api"] = "google";
    body["base_path"] = "/v1beta";
    (void)application_->update_provider("test", body, epoch_);
    int finalized = 0, deleted = 0;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto&) {
        if (request.url.find("/documents?") != std::string::npos) return json_ok("{}");
        if (request.method == "DELETE") {
            ++deleted;
            EXPECT_TRUE(request.url.ends_with("/fileSearchStores/test?force=true"));
            const auto record = application_->folder_store_record(folder_.id, epoch_);
            EXPECT_TRUE(record && record->state == FolderStoreState::incomplete);
            return json_ok("{}");
        }
        if (request.url.ends_with("/v1beta/fileSearchStores"))
            return json_ok(R"({"name":"fileSearchStores/test"})");
        if (request.capture_headers) {
            const auto operation = application_->folder_store_operation_status(folder_.id, epoch_);
            application_->cancel_folder_store_upload(operation.operation_id, epoch_);
            return json_ok("", {{"X-Goog-Upload-URL", "https://generativelanguage.googleapis.com/upload/session"}});
        }
        if (request.url.ends_with("/upload/session")) {
            ++finalized;
            return json_ok(R"({"name":"fileSearchStores/test/upload/operations/op1"})");
        }
        ADD_FAILURE() << "Unexpected request after cancellation: " << request.url;
        return json_ok("{}");
    });
    (void)application_->start_folder_store_upload(folder_.id, "google", key_, epoch_);
    const auto done = wait_status(folder_.id);
    EXPECT_EQ(done.result, "cancelled");
    EXPECT_EQ(finalized, 0);
    EXPECT_EQ(deleted, 1);
    EXPECT_FALSE(application_->folder_store_record(folder_.id, epoch_));
}

TEST_F(FolderStoreOperationsTest, GoogleCancelDuringFinalizeWaitsForResponseAndCleansStore) {
    nlohmann::json body = application_->get_provider("test", epoch_);
    for (const char* field : {"id", "used_by", "writable"}) body.erase(field);
    body["host"] = "generativelanguage.googleapis.com";
    body["api"] = "google";
    body["base_path"] = "/v1beta";
    (void)application_->update_provider("test", body, epoch_);
    int finalized = 0;
    std::vector<std::string> deleted;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        if (request.url.find("/documents?") != std::string::npos)
            return json_ok(R"({"documents":[{"name":"fileSearchStores/test/documents/doc1"}]})");
        if (request.method == "DELETE") {
            deleted.push_back(request.url);
            EXPECT_FALSE(cancel.load());
            const auto record = application_->folder_store_record(folder_.id, epoch_);
            EXPECT_TRUE(record && record->state == FolderStoreState::incomplete);
            return json_ok("{}");
        }
        if (request.url.ends_with("/v1beta/fileSearchStores"))
            return json_ok(R"({"name":"fileSearchStores/test"})");
        if (request.capture_headers)
            return json_ok("", {{"X-Goog-Upload-URL", "https://generativelanguage.googleapis.com/upload/session"}});
        if (request.url.ends_with("/upload/session")) {
            ++finalized;
            const auto operation = application_->folder_store_operation_status(folder_.id, epoch_);
            application_->cancel_folder_store_upload(operation.operation_id, epoch_);
            EXPECT_FALSE(cancel.load()) << "An already-sent allocation must return its resource ID";
            return json_ok(R"({"name":"fileSearchStores/test/upload/operations/op1","done":true})");
        }
        ADD_FAILURE() << "Unexpected request after cancellation: " << request.url;
        return json_ok("{}");
    });
    (void)application_->start_folder_store_upload(folder_.id, "google", key_, epoch_);
    const auto done = wait_status(folder_.id);
    EXPECT_EQ(done.result, "cancelled");
    EXPECT_EQ(finalized, 1);
    EXPECT_EQ(deleted, (std::vector<std::string>{
        "https://generativelanguage.googleapis.com/v1beta/fileSearchStores/test/documents/doc1?force=true",
        "https://generativelanguage.googleapis.com/v1beta/fileSearchStores/test?force=true"}));
    EXPECT_FALSE(application_->folder_store_record(folder_.id, epoch_));
}

TEST_F(FolderStoreOperationsTest, VaultSwitchDuringAllocationLeavesOnlyOriginalUncertainty) {
    Gate gate;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        if (request.method == "POST") gate.arrive_and_wait();
        return openai_transport()(request, cancel);
    });
    const auto operation = application_->start_folder_store_upload(folder_.id, "openai", key_, epoch_);
    ASSERT_TRUE(gate.wait_entered());
    (void)application_->create_vault({.display_name = "Switched"}, epoch_);
    (void)application_->switch_vault("Switched", {}, epoch_);
    epoch_ = application_->context_epoch();
    gate.release();
    {
        std::unique_lock lock(finished_mutex_);
        ASSERT_TRUE(finished_cv_.wait_for(lock, 5s, [&] { return finished_.contains(operation.operation_id); }));
    }
    EXPECT_TRUE(application_->list_folder_stores(epoch_).empty());
    storage::SqliteDatabase old(database_, storage::SqliteDatabase::Mode::read_only);
    const auto record = folder_store_get_by_folder(old, folder_.id);
    ASSERT_TRUE(record);
    EXPECT_TRUE(record->remote_outcome_unknown);
    EXPECT_FALSE(record->remote_store_id);
}

TEST_F(FolderStoreOperationsTest, SavedSnapshotSurvivesSourceEditsAndUsesCurrentKeyEachRequest) {
    const auto items = application_->list_note_items(epoch_);
    const auto note = *std::find_if(items.begin(), items.end(), [&](const auto& item) { return item.parent_id == folder_.id && item.kind == NoteItemKind::note; });
    bool uploaded = false;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        if (request.method == "POST" && request.url.ends_with("/vector_stores")) {
            (void)application_->replace_api_key_value(key_, "replacement-key", epoch_);
            application_->save_note(note.id, text_doc("edited after capture"), epoch_);
            application_->delete_note_item(note.id, epoch_);
        } else {
            EXPECT_EQ(request.headers.front().value, "Bearer replacement-key");
        }
        if (!request.multipart.empty()) {
            uploaded = true;
            EXPECT_NE(request.multipart.back().data.find("snapshot fact"), std::string::npos);
            EXPECT_EQ(request.multipart.back().data.find("edited after capture"), std::string::npos);
        }
        return openai_transport()(request, cancel);
    });
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, epoch_);
    EXPECT_EQ(wait_status(folder_.id).result, "success");
    EXPECT_TRUE(uploaded);
}

TEST_F(FolderStoreOperationsTest, RemoveUsesStoredKeyWhenNoProviderIsEligible) {
    const auto id = seed_store(folder_.id);
    auto provider = application_->get_provider("test", epoch_);
    nlohmann::json body = provider;
    for (const char* field : {"id", "used_by", "writable"}) body.erase(field);
    body["host"] = "proxy.example";
    (void)application_->update_provider("test", body, epoch_);
    EXPECT_TRUE(application_->folder_store_choices(epoch_).empty());
    application_->set_folder_store_transport_for_tests(openai_transport());
    (void)application_->start_folder_store_remove(folder_.id, id, epoch_);
    EXPECT_EQ(wait_status(folder_.id).result, "success");
}

TEST_F(FolderStoreOperationsTest, CannotCloseActiveCleanupAndCloseFailedDeleteReleasesMembership) {
    const auto id = seed_store(folder_.id);
    auto gate = std::make_shared<Gate>();
    application_->set_folder_store_transport_for_tests([gate](const auto&, const auto&) {
        gate->arrive_and_wait();
        return FolderStoreHttpResponse{.status = 401};
    });
    const auto operation = application_->start_folder_delete(folder_.id, 1, epoch_);
    ASSERT_TRUE(gate->wait_entered());
    EXPECT_THROW(application_->close_folder_store_operation(operation.operation_id, epoch_), ApplicationError);
    EXPECT_THROW((void)application_->import_note_book(folder_.id, "Blocked", {{"N", text_doc("x")}}, epoch_), ApplicationError);
    gate->release();
    EXPECT_EQ(wait_status(folder_.id).phase, "failed");
    application_->close_folder_store_operation(operation.operation_id, epoch_);
    EXPECT_FALSE(application_->folder_store_operation_status(folder_.id, epoch_).present);
    EXPECT_NO_THROW((void)application_->create_note_item(folder_.id, NoteItemKind::note, "Allowed", epoch_));
    EXPECT_EQ(application_->folder_store_record(folder_.id, epoch_)->id, id);
}

TEST_F(FolderStoreOperationsTest, FailedChildCopyRemovesOnlyUnpublishedDestination) {
    seed_store(folder_.id);
    execute_sql("CREATE TRIGGER reject_reset BEFORE DELETE ON folder_stores BEGIN SELECT RAISE(ABORT, 'ownership reset failure'); END");
    EXPECT_THROW((void)application_->create_vault({.display_name = "Child", .copy_from = "Test"}, epoch_), std::exception);
    EXPECT_TRUE(application_->folder_store_record(folder_.id, epoch_));
    execute_sql("DROP TRIGGER reject_reset");
    const auto child = application_->create_vault({.display_name = "Child", .copy_from = "Test"}, epoch_);
    EXPECT_TRUE(std::filesystem::exists(child.data));
    storage::SqliteDatabase db(child.data, storage::SqliteDatabase::Mode::read_only);
    EXPECT_TRUE(folder_store_list(db).empty());
}

TEST_F(FolderStoreOperationsTest, PasswordProtectionKeepsOwnershipAndReopenMakesNoProviderCalls) {
    seed_store(folder_.id);
    int calls = 0;
    application_->set_folder_store_transport_for_tests([&](const auto&, const auto&) { ++calls; return json_ok("{}"); });
    (void)application_->update_vault("Test", {.display_name = "Test", .password = "secret"}, epoch_);
    epoch_ = application_->context_epoch();
    EXPECT_TRUE(application_->folder_store_record(folder_.id, epoch_));
    EXPECT_EQ(calls, 0);
    storage::SqliteDatabase db(database_, storage::SqliteDatabase::Mode::read_only, "secret");
    EXPECT_EQ(folder_store_get_by_folder(db, folder_.id)->remote_store_id, "vs_1");
}

TEST_F(FolderStoreOperationsTest, ShutdownDeadlineRetainsDependenciesUntilLateWorkerFinishes) {
    auto gate = std::make_shared<Gate>();
    application_->set_folder_store_transport_for_tests([gate](const auto&, const auto&) {
        gate->arrive_and_wait();
        return json_ok(R"({"id":"vs_late"})");
    });
    const auto operation = application_->start_folder_store_upload(folder_.id, "openai", key_, epoch_);
    ASSERT_TRUE(gate->wait_entered());
    application_->request_shutdown();
    EXPECT_FALSE(application_->join_shutdown(0ms));
    gate->release();
    {
        std::unique_lock lock(finished_mutex_);
        ASSERT_TRUE(finished_cv_.wait_for(lock, 5s, [&] { return finished_.contains(operation.operation_id); }));
    }
    EXPECT_TRUE(application_->join_shutdown(2s));
    storage::SqliteDatabase db(database_, storage::SqliteDatabase::Mode::read_only);
    const auto record = folder_store_get_by_folder(db, folder_.id);
    ASSERT_TRUE(record);
    EXPECT_FALSE(record->remote_store_id);
    EXPECT_TRUE(record->remote_outcome_unknown);
}

TEST_F(FolderStoreOperationsTest, IndexFailureStaysAnErrorWhenCancelArrivesDuringCleanup) {
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        if (request.method == "DELETE") {
            const auto operation = application_->folder_store_operation_status(folder_.id, epoch_).operation_id;
            application_->cancel_folder_store_upload(operation, epoch_);
            EXPECT_FALSE(cancel.load());
        }
        return openai_transport(true)(request, cancel);
    });
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, epoch_);
    const auto status = wait_status(folder_.id);
    EXPECT_EQ(status.result, "error");
    EXPECT_EQ(status.phase, "failed");
}

TEST_F(FolderStoreOperationsTest, SubtreeRetryKeepsWarningFromAnAlreadyRemovedStore) {
    const auto child = application_->create_note_item(folder_.id, NoteItemKind::folder, "Child", epoch_);
    seed_store(folder_.id, "vs_missing", {});
    seed_store(child.id, "vs_denied", {});
    application_->set_folder_store_transport_for_tests([](const auto& request, const auto&) {
        if (request.url.ends_with("vs_missing")) {
            return FolderStoreHttpResponse{.status = 404,
                .body = R"({"error":{"type":"invalid_request_error","message":"No such vector store"}})"};
        }
        return FolderStoreHttpResponse{.status = 401};
    });
    const auto operation = application_->start_folder_delete(folder_.id, 2, epoch_);
    const auto failed = wait_status(folder_.id);
    EXPECT_EQ(failed.phase, "failed");
    EXPECT_EQ(failed.result, "error");
    EXPECT_FALSE(failed.folder_deleted);
    EXPECT_FALSE(application_->folder_store_record(folder_.id, epoch_));
    ASSERT_TRUE(application_->folder_store_record(child.id, epoch_));

    int deletes = 0;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto&) {
        EXPECT_TRUE(request.url.ends_with("vs_denied"));
        ++deletes;
        return json_ok(R"({"deleted":true})");
    });
    const auto retried = application_->retry_folder_delete(operation.operation_id, epoch_);
    EXPECT_EQ(retried.operation_id, operation.operation_id);
    const auto done = wait_status(folder_.id);
    EXPECT_TRUE(done.folder_deleted);
    EXPECT_EQ(done.phase, "warning");
    EXPECT_EQ(done.result, "warning");
    EXPECT_EQ(deletes, 1);
}

TEST_F(FolderStoreOperationsTest, ChildDeletionPreservesAncestorStoreAndItsResources) {
    const auto child = application_->create_note_item(folder_.id, NoteItemKind::folder, "Child", epoch_);
    const auto ancestor_id = seed_store(folder_.id, "vs_ancestor", {"file_ancestor"});
    seed_store(child.id, "vs_child", {"file_child"});
    std::vector<std::string> deleted;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto&) {
        deleted.push_back(request.url);
        return json_ok(R"({"deleted":true})");
    });
    (void)application_->start_folder_delete(child.id, 1, epoch_);
    const auto done = wait_status(child.id);
    EXPECT_EQ(done.result, "folder_deleted");
    EXPECT_TRUE(done.folder_deleted);
    ASSERT_EQ(deleted.size(), 2U);
    EXPECT_TRUE(deleted[0].ends_with("vs_child"));
    EXPECT_TRUE(deleted[1].ends_with("file_child"));
    const auto ancestor = application_->folder_store_record(folder_.id, epoch_);
    ASSERT_TRUE(ancestor);
    EXPECT_EQ(ancestor->id, ancestor_id);
    EXPECT_EQ(ancestor->remote_store_id, "vs_ancestor");
    EXPECT_EQ(ancestor->file_ids, std::vector<std::string>({"file_ancestor"}));
}

TEST_F(FolderStoreOperationsTest, CancelCannotMissThePollWakeupAfterPredicateRead) {
    auto gate = std::make_shared<Gate>();
    auto once = std::make_shared<std::once_flag>();
    RuntimeSettings settings;
    settings.folder_store_poll_interval = 3s;
    settings.folder_store_poll_predicate_read = [gate, once] {
        std::call_once(*once, [&] { gate->arrive_and_wait(); });
    };
    reopen(settings);
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        if (request.method == "GET") return json_ok(R"({"status":"in_progress"})");
        return openai_transport()(request, cancel);
    });
    const auto operation = application_->start_folder_store_upload(folder_.id, "openai", key_, epoch_);
    ASSERT_TRUE(gate->wait_entered());
    std::promise<void> cancelling;
    auto attempted = cancelling.get_future();
    auto cancel = std::async(std::launch::async, [&] {
        cancelling.set_value();
        application_->cancel_folder_store_upload(operation.operation_id, epoch_);
    });
    EXPECT_EQ(attempted.wait_for(2s), std::future_status::ready);
    // The notifier must wait for the same mutex as the predicate/wait handoff.
    EXPECT_EQ(cancel.wait_for(50ms), std::future_status::timeout);
    gate->release();
    EXPECT_EQ(cancel.wait_for(1s), std::future_status::ready);
    cancel.get();
    {
        std::unique_lock lock(finished_mutex_);
        EXPECT_TRUE(finished_cv_.wait_for(lock, 1s, [&] { return finished_.contains(operation.operation_id); }));
    }
    const auto done = wait_status(folder_.id);
    EXPECT_EQ(done.phase, "cancelled");
    EXPECT_EQ(done.result, "cancelled");
}


TEST_F(FolderStoreOperationsTest, StaleOperationIdsCannotCancelOrCloseReplacementAndRetryHasNoTerminalResult) {
    const auto id = seed_store(folder_.id);
    application_->set_folder_store_transport_for_tests([](const auto&, const auto&) { return FolderStoreHttpResponse{.status = 401}; });
    const auto first = application_->start_folder_delete(folder_.id, 1, epoch_);
    ASSERT_EQ(wait_status(folder_.id).result, "error");
    auto gate = std::make_shared<Gate>();
    application_->set_folder_store_transport_for_tests([gate](const auto&, const auto&) {
        gate->arrive_and_wait(); return json_ok(R"({"deleted":true})");
    });
    (void)application_->retry_folder_delete(first.operation_id, epoch_);
    ASSERT_TRUE(gate->wait_entered());
    const auto retry = application_->folder_store_operation_status(folder_.id, epoch_);
    EXPECT_FALSE(retry.terminal); EXPECT_TRUE(retry.result.empty());
    gate->release();
    EXPECT_TRUE(wait_status(folder_.id).folder_deleted);
    application_->close_folder_store_operation(first.operation_id, epoch_);
    EXPECT_THROW(application_->close_folder_store_operation(first.operation_id, epoch_), ApplicationError);
    EXPECT_THROW(application_->cancel_folder_store_upload(first.operation_id, epoch_), ApplicationError);
    (void)id;
}

TEST_F(FolderStoreOperationsTest, SavedKeyUsageIncludesIncompleteAndReadyStoresAfterKeyAndFolderEdits) {
    const auto id = seed_store(folder_.id);
    const auto usage = [&](const ApiKeyDetail& detail, std::string_view name) {
        EXPECT_NE(std::find(detail.used_by.begin(), detail.used_by.end(), name), detail.used_by.end());
    };
    const auto find = [&]() -> ApiKeyDetail {
        const auto list = application_->list_api_keys(epoch_);
        return *std::find_if(list.begin(), list.end(), [&](const auto& key) { return key.id == key_; });
    };
    usage(find(), "Folder store: Research (OpenAI)");
    usage(application_->rename_api_key(key_, "Renamed key", epoch_), "Folder store: Research (OpenAI)");
    (void)application_->rename_note_item(folder_.id, "Renamed folder", epoch_);
    usage(application_->replace_api_key_value(key_, "replacement", epoch_), "Folder store: Renamed folder (OpenAI)");
    {
        storage::SqliteDatabase db(database_, storage::SqliteDatabase::Mode::read_write);
        folder_store_mark_ready(db, id);
    }
    usage(find(), "Folder store: Renamed folder (OpenAI)");
    EXPECT_NO_THROW(application_->delete_api_key(key_, epoch_));
    EXPECT_EQ(application_->folder_store_record(folder_.id, epoch_)->api_key_id, key_);
}


TEST_F(FolderStoreOperationsTest, FailedRetryLaunchRetainsACompleteFailureStatusAndReservation) {
    auto fail_launch = std::make_shared<bool>(false);
    RuntimeSettings settings;
    settings.folder_store_before_launch = [fail_launch] { if (*fail_launch) throw std::runtime_error("launch failure"); };
    reopen(settings);
    (void)seed_store(folder_.id);
    application_->set_folder_store_transport_for_tests([](const auto&, const auto&) { return FolderStoreHttpResponse{.status = 401}; });
    const auto operation = application_->start_folder_delete(folder_.id, 1, epoch_);
    ASSERT_EQ(wait_status(folder_.id).result, "error");
    *fail_launch = true;
    EXPECT_THROW((void)application_->retry_folder_delete(operation.operation_id, epoch_), ApplicationError);
    const auto status = application_->folder_store_operation_status(folder_.id, epoch_);
    EXPECT_TRUE(status.terminal);
    EXPECT_EQ(status.phase, "failed"); EXPECT_EQ(status.result, "error");
    EXPECT_FALSE(status.error.empty()); EXPECT_FALSE(status.folder_deleted);
    EXPECT_TRUE(status.known_cleanup_remains);
    EXPECT_THROW((void)application_->create_note_item(folder_.id, NoteItemKind::note, "Blocked", epoch_), ApplicationError);
    application_->close_folder_store_operation(operation.operation_id, epoch_);
    EXPECT_NO_THROW((void)application_->create_note_item(folder_.id, NoteItemKind::note, "Allowed", epoch_));
}


TEST_F(FolderStoreOperationsTest, ClosePreservesUnsavedOwnershipUntilItCanBeRecorded) {
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        if (request.method == "DELETE") return FolderStoreHttpResponse{.status = 403};
        auto response = openai_transport()(request, cancel);
        if (request.url.ends_with("/vector_stores") && request.method == "POST") {
            storage::SqliteDatabase db(database_, storage::SqliteDatabase::Mode::read_write);
            db.execute("CREATE TRIGGER block_remote_id BEFORE UPDATE OF remote_store_id ON folder_stores "
                "BEGIN SELECT RAISE(ABORT, 'injected write failure'); END");
        }
        return response;
    });
    const auto operation = application_->start_folder_store_upload(folder_.id, "openai", key_, epoch_);
    const auto status = wait_status(folder_.id);
    EXPECT_EQ(status.phase, "failed"); EXPECT_TRUE(status.known_cleanup_remains);
    ASSERT_TRUE(application_->folder_store_record(folder_.id, epoch_));
    EXPECT_FALSE(application_->folder_store_record(folder_.id, epoch_)->remote_store_id);
    EXPECT_THROW(application_->close_folder_store_operation(operation.operation_id, epoch_), ApplicationError);
    EXPECT_TRUE(application_->folder_store_operation_status(folder_.id, epoch_).present);
    {
        storage::SqliteDatabase db(database_, storage::SqliteDatabase::Mode::read_write);
        db.execute("DROP TRIGGER block_remote_id");
    }
    application_->close_folder_store_operation(operation.operation_id, epoch_);
    EXPECT_FALSE(application_->folder_store_operation_status(folder_.id, epoch_).present);
    EXPECT_EQ(application_->folder_store_record(folder_.id, epoch_)->remote_store_id, "vs_1");
}

TEST_F(FolderStoreOperationsTest, FailedRemoveLaunchPreservesPreviousOperationAndUnsavedOwnership) {
    auto fail_launch = std::make_shared<bool>(false);
    RuntimeSettings settings;
    settings.folder_store_before_launch = [fail_launch] {
        if (*fail_launch) throw std::runtime_error("injected launch failure");
    };
    reopen(settings);
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        if (request.method == "DELETE") return FolderStoreHttpResponse{.status = 403};
        auto response = openai_transport()(request, cancel);
        if (request.url.ends_with("/vector_stores") && request.method == "POST") {
            storage::SqliteDatabase db(database_, storage::SqliteDatabase::Mode::read_write);
            db.execute("CREATE TRIGGER block_remote_id BEFORE UPDATE OF remote_store_id ON folder_stores "
                "BEGIN SELECT RAISE(ABORT, 'injected write failure'); END");
        }
        return response;
    });
    const auto upload = application_->start_folder_store_upload(folder_.id, "openai", key_, epoch_);
    const auto failed = wait_status(folder_.id);
    ASSERT_EQ(failed.result, "error");
    ASSERT_TRUE(failed.known_cleanup_remains);
    const auto record = application_->folder_store_record(folder_.id, epoch_);
    ASSERT_TRUE(record);
    ASSERT_FALSE(record->remote_store_id);

    *fail_launch = true;
    EXPECT_THROW((void)application_->start_folder_store_remove(folder_.id, record->id, epoch_), ApplicationError);
    const auto retained = application_->folder_store_operation_status(folder_.id, epoch_);
    EXPECT_TRUE(retained.present);
    EXPECT_EQ(retained.operation_id, upload.operation_id);
    EXPECT_TRUE(retained.terminal);
    EXPECT_TRUE(retained.known_cleanup_remains);
    {
        storage::SqliteDatabase db(database_, storage::SqliteDatabase::Mode::read_write);
        db.execute("DROP TRIGGER block_remote_id");
    }
    *fail_launch = false;
    std::vector<std::string> deleted;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        if (request.method == "DELETE") deleted.push_back(request.url);
        return openai_transport()(request, cancel);
    });
    (void)application_->start_folder_store_remove(folder_.id, record->id, epoch_);
    const auto done = wait_status(folder_.id);
    EXPECT_EQ(deleted, std::vector<std::string>({"https://api.openai.com/v1/vector_stores/vs_1"}));
    EXPECT_EQ(done.result, "warning");
    EXPECT_FALSE(done.known_cleanup_remains);
    EXPECT_FALSE(application_->folder_store_record(folder_.id, epoch_));
}

TEST_F(FolderStoreOperationsTest, RemoveRetriesUnsavedIdsWithoutClosingOrPersistingThemFirst) {
    for (const bool file : {false, true}) {
        SCOPED_TRACE(file ? "unsaved file" : "unsaved store");
        application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
            if (request.method == "DELETE") return FolderStoreHttpResponse{.status = 403};
            auto response = openai_transport()(request, cancel);
            if ((file && !request.multipart.empty())
                || (!file && request.url.ends_with("/vector_stores") && request.method == "POST")) {
                storage::SqliteDatabase db(database_, storage::SqliteDatabase::Mode::read_write);
                db.execute(file
                    ? "CREATE TRIGGER block_returned_id BEFORE INSERT ON folder_store_files "
                      "BEGIN SELECT RAISE(ABORT, 'injected write failure'); END"
                    : "CREATE TRIGGER block_returned_id BEFORE UPDATE OF remote_store_id ON folder_stores "
                      "WHEN NEW.remote_store_id IS NOT NULL "
                      "BEGIN SELECT RAISE(ABORT, 'injected write failure'); END");
            }
            return response;
        });
        const auto upload = application_->start_folder_store_upload(folder_.id, "openai", key_, epoch_);
        const auto failed = wait_status(folder_.id);
        ASSERT_EQ(failed.result, "error");
        ASSERT_TRUE(failed.known_cleanup_remains);
        const auto record = application_->folder_store_record(folder_.id, epoch_);
        ASSERT_TRUE(record);
        EXPECT_TRUE(record->remote_outcome_unknown);
        if (file) EXPECT_TRUE(record->file_ids.empty());
        else EXPECT_FALSE(record->remote_store_id);
        EXPECT_THROW(application_->close_folder_store_operation(upload.operation_id, epoch_), ApplicationError);

        // Keep the write fault active: cleanup must use the returned IDs in memory.
        std::vector<std::string> deleted;
        application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
            if (request.method == "DELETE") deleted.push_back(request.url);
            return openai_transport()(request, cancel);
        });
        const auto remove = application_->start_folder_store_remove(folder_.id, record->id, epoch_);
        const auto done = wait_status(folder_.id);
        auto expected = std::vector<std::string>{"https://api.openai.com/v1/vector_stores/vs_1"};
        if (file) expected.push_back("https://api.openai.com/v1/files/file_1");
        EXPECT_EQ(deleted, expected);
        EXPECT_EQ(done.result, "warning");
        EXPECT_FALSE(done.known_cleanup_remains);
        EXPECT_FALSE(application_->folder_store_record(folder_.id, epoch_));
        application_->close_folder_store_operation(remove.operation_id, epoch_);
        storage::SqliteDatabase db(database_, storage::SqliteDatabase::Mode::read_write);
        db.execute("DROP TRIGGER block_returned_id");
    }
}


TEST_F(FolderStoreOperationsTest, UploadFailureDoesNotReportAnUntouchedChildStoreAsCleanup) {
    const auto child = application_->create_note_item(folder_.id, NoteItemKind::folder, "Child", epoch_);
    const auto child_store = seed_store(child.id, "vs_child", {});
    {
        storage::SqliteDatabase db(database_, storage::SqliteDatabase::Mode::read_write);
        folder_store_mark_ready(db, child_store);
    }
    int requests = 0;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        ++requests;
        EXPECT_FALSE(request.url.ends_with("vs_child"));
        return openai_transport(true)(request, cancel);
    });
    const auto preflight = application_->start_folder_store_upload(folder_.id, "openai", "missing", epoch_);
    auto status = wait_status(folder_.id);
    EXPECT_EQ(status.result, "preflight"); EXPECT_FALSE(status.known_cleanup_remains);
    EXPECT_EQ(requests, 0);
    application_->close_folder_store_operation(preflight.operation_id, epoch_);
    // A failed remote upload whose own cleanup succeeded also leaves no cleanup.
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, epoch_);
    status = wait_status(folder_.id);
    EXPECT_EQ(status.result, "error"); EXPECT_FALSE(status.known_cleanup_remains);
    EXPECT_FALSE(application_->folder_store_record(folder_.id, epoch_));
    const auto retained = application_->folder_store_record(child.id, epoch_);
    ASSERT_TRUE(retained);
    EXPECT_EQ(retained->state, FolderStoreState::ready);
    EXPECT_EQ(retained->remote_store_id, "vs_child");
}

TEST_F(FolderStoreOperationsTest, FailedRemoveRechecksOnlyItsOwnStoreRecord) {
    const auto child = application_->create_note_item(folder_.id, NoteItemKind::folder, "Child", epoch_);
    const auto child_store = seed_store(child.id, "vs_child", {});
    const auto parent_store = seed_store(folder_.id, {}, {});
    {
        storage::SqliteDatabase db(database_, storage::SqliteDatabase::Mode::read_write);
        folder_store_mark_ready(db, child_store);
        db.execute("CREATE TRIGGER block_store_delete BEFORE DELETE ON folder_stores "
            "BEGIN SELECT RAISE(ABORT, 'injected local failure'); END");
    }
    application_->set_folder_store_transport_for_tests([](const auto&, const auto&) {
        ADD_FAILURE() << "Remove has no remote resources";
        return FolderStoreHttpResponse{};
    });
    (void)application_->start_folder_store_remove(folder_.id, parent_store, epoch_);
    const auto status = wait_status(folder_.id);
    EXPECT_EQ(status.phase, "failed"); EXPECT_FALSE(status.known_cleanup_remains);
    EXPECT_TRUE(application_->folder_store_record(folder_.id, epoch_));
    EXPECT_EQ(application_->folder_store_record(child.id, epoch_)->state, FolderStoreState::ready);
    {
        storage::SqliteDatabase db(database_, storage::SqliteDatabase::Mode::read_write);
        db.execute("DROP TRIGGER block_store_delete");
        folder_store_save_remote_id(db, parent_store, "vs_parent");
        db.execute("CREATE TRIGGER block_incomplete BEFORE UPDATE OF state ON folder_stores "
            "BEGIN SELECT RAISE(ABORT, 'injected local failure'); END");
    }
    // If failure precedes cleanup, the parent's retained IDs must still count.
    (void)application_->start_folder_store_remove(folder_.id, parent_store, epoch_);
    const auto with_resources = wait_status(folder_.id);
    EXPECT_EQ(with_resources.phase, "failed"); EXPECT_TRUE(with_resources.known_cleanup_remains);
    {
        storage::SqliteDatabase db(database_, storage::SqliteDatabase::Mode::read_write);
        db.execute("DROP TRIGGER block_incomplete");
    }
}

TEST_F(FolderStoreOperationsTest, SecondaryProcessCanReadStoresButCannotMutateOperations) {
    const auto record = seed_store(folder_.id);
    auto secondary = Application::open(make_command(workspace_, database_));
    const auto epoch = secondary->context_epoch();
    EXPECT_NO_THROW((void)secondary->folder_store_choices(epoch));
    EXPECT_FALSE(secondary->folder_store_operation_status(folder_.id, epoch).present);
    EXPECT_THROW((void)secondary->start_folder_store_upload(folder_.id, "openai", key_, epoch), SecondaryProcessError);
    EXPECT_THROW((void)secondary->start_folder_store_remove(folder_.id, record, epoch), SecondaryProcessError);
    EXPECT_THROW(secondary->cancel_folder_store_upload(1, epoch), SecondaryProcessError);
    EXPECT_THROW(secondary->close_folder_store_operation(1, epoch), SecondaryProcessError);
    EXPECT_THROW((void)secondary->start_folder_delete(folder_.id, 1, epoch), SecondaryProcessError);
    EXPECT_THROW((void)secondary->retry_folder_delete(1, epoch), SecondaryProcessError);
    EXPECT_THROW(secondary->delete_folder_anyway(1, epoch), SecondaryProcessError);
    EXPECT_EQ(application_->folder_store_record(folder_.id, epoch_)->id, record);
    secondary->request_shutdown();
    EXPECT_TRUE(secondary->join_shutdown(2s));
}

TEST_F(FolderStoreOperationsTest, FailedIndexingKeepsNotFoundCleanupWarningInStatus) {
    application_->set_folder_store_transport_for_tests(openai_transport(true, true));
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, epoch_);
    const auto status = wait_status(folder_.id);
    EXPECT_EQ(status.result, "error");
    EXPECT_TRUE(status.terminal);
    EXPECT_FALSE(status.known_cleanup_remains);
    EXPECT_NE(status.error.find("Indexing did not complete."), std::string::npos);
    EXPECT_NE(status.error.find("Some remote data could not be confirmed deleted."), std::string::npos);
    EXPECT_FALSE(application_->folder_store_record(folder_.id, epoch_));
}

TEST_F(FolderStoreOperationsTest, FailedFileAllocationKeepsUncertaintyInStatusAfterKnownCleanup) {
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        if (!request.multipart.empty()) {
            FolderStoreHttpResponse response;
            response.status = 503;
            return response;
        }
        return openai_transport()(request, cancel);
    });
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, epoch_);
    const auto status = wait_status(folder_.id);
    EXPECT_EQ(status.result, "error");
    EXPECT_FALSE(status.known_cleanup_remains);
    EXPECT_NE(status.error.find("Some remote data could not be confirmed deleted."), std::string::npos);
    EXPECT_FALSE(application_->folder_store_record(folder_.id, epoch_));
}

TEST_F(FolderStoreOperationsTest, RemoveFailureHasOneShortErrorAndKeepsResourceDiagnosticsInEvents) {
    std::vector<std::string> files;
    for (int i = 0; i < 100; ++i) files.push_back("file_" + std::to_string(i));
    const auto id = seed_store(folder_.id, "vs_1", files);
    application_->delete_api_key(key_, epoch_);
    (void)application_->start_folder_store_remove(folder_.id, id, epoch_);
    const auto status = wait_status(folder_.id);
    EXPECT_EQ(status.error, "Could not remove “Research” from OpenAI: The saved API key is missing or empty.");
    EXPECT_TRUE(status.known_cleanup_remains);
    const auto events = application_->list_events(epoch_);
    ASSERT_FALSE(events.empty());
    EXPECT_NE(events.front().message.find("vs_1"), std::string::npos);
    EXPECT_NE(events.front().message.find("file_0"), std::string::npos);
    EXPECT_NE(events.front().message.find("file_99"), std::string::npos);
    EXPECT_NE(events.front().message.find(key_), std::string::npos);
    EXPECT_EQ(application_->folder_store_record(folder_.id, epoch_)->file_ids.size(), 100U);
}

TEST_F(FolderStoreOperationsTest, FailedFolderDeleteSummarizesActualCountsByService) {
    const auto child = application_->create_note_item(folder_.id, NoteItemKind::folder, "Child", epoch_);
    const auto google = application_->create_note_item(folder_.id, NoteItemKind::folder, "Google", epoch_);
    seed_store(folder_.id);
    seed_store(child.id);
    {
        storage::SqliteDatabase db(database_, storage::SqliteDatabase::Mode::read_write);
        storage::SqliteTransaction tx(db);
        const auto id = folder_store_register(db, google.id, FolderStoreService::google, key_, 1);
        folder_store_save_remote_id(db, id, "fileSearchStores/test");
        tx.commit();
    }
    application_->delete_api_key(key_, epoch_);
    (void)application_->start_folder_delete(folder_.id, 3, epoch_);
    const auto status = wait_status(folder_.id);
    EXPECT_EQ(status.error, "Could not remove 2 OpenAI stores and 1 Google store in “Research”: The saved API key is missing or empty.");
    EXPECT_EQ(status.total, 3);
    EXPECT_EQ(status.completed, 0);
    EXPECT_FALSE(status.folder_deleted);
    EXPECT_TRUE(status.known_cleanup_remains);
    EXPECT_EQ(application_->folder_store_count_in_subtree(folder_.id, epoch_), 3);
}
} // namespace
} // namespace cha::app
