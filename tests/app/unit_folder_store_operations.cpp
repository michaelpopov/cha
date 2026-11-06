#include "app/application.h"
#include "storage/folder_store_storage.h"
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
#include <utility>
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

// Upload preflight lists the key's stores before it creates a store.
bool store_list_request(const FolderStoreHttpRequest& request) {
    return request.method == "GET" && (request.url.find("/vector_stores?limit=") != std::string::npos
        || request.url.find("/fileSearchStores?pageSize=") != std::string::npos);
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
        settings.test_hooks.folder_store_worker_finished = [this](std::int64_t id) {
            { std::lock_guard lock(finished_mutex_); finished_.insert(id); }
            finished_cv_.notify_all();
        };
        settings.test_hooks.folder_store_wait = [this](int) {
            {
                std::lock_guard lock(wait_mutex_);
                ++waits_;
            }
            wait_cv_.notify_all();
            if (hold_wait_.valid()) hold_wait_.wait_for(5s);
        };
        application_ = Application::open(make_command(workspace_, database_), {}, settings);
        epoch_ = application_->context_epoch();
        key_ = application_->create_api_key(
            {.display_name = "OpenAI", .value = "sk-test", .cloud_store = "openai"}, epoch_).id;
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

    std::string store_name() const { return "Research"; }

    FolderStoreTransport openai_transport(bool fail_index = false, bool not_found = false) {
        return [=](const FolderStoreHttpRequest& request, const std::atomic_bool&) {
            if (store_list_request(request)) return json_ok(R"({"data":[]})");
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
                return json_ok(R"({"id":"vs_1","object":"vector_store","name":"Research","status":"completed"})");
            }
            if (request.multipart.size() == 2) return json_ok(R"({"id":"file_1"})");
            if (request.url.find("/files/file_1") != std::string::npos) {
                return json_ok(fail_index ? R"({"status":"failed"})" : R"({"status":"completed"})");
            }
            return json_ok(R"({"id":"vsf_1","status":"in_progress"})");
        };
    }

    void reopen(RuntimeSettings settings) {
        application_->request_shutdown();
        EXPECT_TRUE(application_->join_shutdown(2s));
        application_.reset();
        settings.test_hooks.folder_store_worker_finished = [this](std::int64_t id) {
            { std::lock_guard lock(finished_mutex_); finished_.insert(id); }
            finished_cv_.notify_all();
        };
        { std::lock_guard lock(finished_mutex_); finished_.clear(); }
        if (!settings.test_hooks.folder_store_transport) {
            settings.test_hooks.folder_store_transport = [](const auto&, const auto&) {
                return json_ok(R"({"data":[],"fileSearchStores":[]})");
            };
        }
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

OperationReply::Result wait_cloud_reply(const std::shared_ptr<OperationReply>& reply) {
    auto ready = std::make_shared<std::promise<void>>();
    auto future = ready->get_future();
    reply->set_ready_callback([ready] { ready->set_value(); });
    if (future.wait_for(5s) != std::future_status::ready) {
        ADD_FAILURE() << "Cloud store request did not finish";
        return OperationReply::Failure{};
    }
    return *reply->peek();
}

nlohmann::json cloud_json(const std::shared_ptr<OperationReply>& reply) {
    const auto result = wait_cloud_reply(reply);
    if (const auto* value = std::get_if<nlohmann::json>(&result)) return *value;
    ADD_FAILURE() << std::get<OperationReply::Failure>(result).message;
    return nlohmann::json::object();
}

TEST_F(FolderStoreOperationsTest, StoreListAndDetailsShareOneDownloadUntilRefresh) {
    std::atomic_int calls{};
    std::atomic_bool refreshed{};
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto&) {
        ++calls;
        if (store_list_request(request)) return json_ok(R"({"data":[{"id":"vs_1","name":"Research"}]})");
        return json_ok(nlohmann::json{{"id", "vs_1"}, {"name", refreshed.load() ? "Archive" : "Research"},
            {"description", "Notes"}, {"created_at", 1793697300}}.dump());
    });
    EXPECT_EQ(cloud_json(application_->list_cloud_stores(key_, epoch_))["items"][0]["name"], "Research");
    EXPECT_EQ(cloud_json(application_->get_cloud_store(key_, "vs_1", epoch_))["description"], "Notes");
    (void)cloud_json(application_->list_cloud_stores(key_, epoch_));
    EXPECT_EQ(calls.load(), 2);
    refreshed = true;
    EXPECT_EQ(cloud_json(application_->refresh_cloud_stores(key_, epoch_))["items"][0]["name"], "Archive");
    EXPECT_EQ(cloud_json(application_->get_cloud_store(key_, "vs_1", epoch_))["name"], "Archive");
    EXPECT_EQ(calls.load(), 4);
}

TEST_F(FolderStoreOperationsTest, FailedRefreshKeepsTheCompletePreviousSnapshot) {
    std::atomic_bool fail{};
    std::atomic_int calls{};
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto&) {
        ++calls;
        if (store_list_request(request)) return json_ok(fail.load()
            ? R"({"data":[{"id":"vs_2","name":"Replacement"}]})"
            : R"({"data":[{"id":"vs_1","name":"Research"}]})");
        if (fail.load()) return FolderStoreHttpResponse{.status = 503};
        return json_ok(R"({"id":"vs_1","name":"Research","created_at":1793697300})");
    });
    const auto original = cloud_json(application_->list_cloud_stores(key_, epoch_));
    fail = true;
    EXPECT_TRUE(std::holds_alternative<OperationReply::Failure>(wait_cloud_reply(application_->refresh_cloud_stores(key_, epoch_))));
    EXPECT_EQ(cloud_json(application_->list_cloud_stores(key_, epoch_)), original);
    EXPECT_EQ(cloud_json(application_->get_cloud_store(key_, "vs_1", epoch_))["name"], "Research");
    EXPECT_EQ(calls.load(), 4);
}

TEST_F(FolderStoreOperationsTest, InitialReadersShareTheLoadAndCachedReadsDoNotWaitForRefresh) {
    Gate gate;
    std::atomic_int calls{};
    application_->set_folder_store_transport_for_tests([&](const auto&, const auto&) {
        ++calls;
        gate.arrive_and_wait();
        return json_ok(R"({"data":[]})");
    });
    const auto first = application_->list_cloud_stores(key_, epoch_);
    ASSERT_TRUE(gate.wait_entered());
    const auto second = application_->list_cloud_stores(key_, epoch_);
    gate.release();
    EXPECT_TRUE(cloud_json(first)["items"].empty());
    EXPECT_TRUE(cloud_json(second)["items"].empty());
    EXPECT_EQ(calls.load(), 1);

    Gate refresh_gate;
    application_->set_folder_store_transport_for_tests([&](const auto&, const auto&) {
        ++calls;
        refresh_gate.arrive_and_wait();
        return json_ok(R"({"data":[]})");
    });
    const auto refresh = application_->refresh_cloud_stores(key_, epoch_);
    ASSERT_TRUE(refresh_gate.wait_entered());
    const auto cached = application_->list_cloud_stores(key_, epoch_);
    EXPECT_TRUE(cloud_json(cached)["items"].empty());
    refresh_gate.release();
    (void)cloud_json(refresh);
    EXPECT_EQ(calls.load(), 2);
}

TEST_F(FolderStoreOperationsTest, KeyReplacementDoesNotReuseAnOlderDownload) {
    Gate gate;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto&) {
        if (request.headers.front().value == "Bearer sk-test") {
            gate.arrive_and_wait();
            return store_list_request(request)
                ? json_ok(R"({"data":[{"id":"vs_old","name":"Old"}]})")
                : json_ok(R"({"id":"vs_old","name":"Old","created_at":1793697300})");
        }
        EXPECT_EQ(request.headers.front().value, "Bearer replacement");
        return json_ok(R"({"data":[]})");
    });
    const auto old = application_->list_cloud_stores(key_, epoch_);
    ASSERT_TRUE(gate.wait_entered());
    (void)application_->replace_api_key_value(key_, {.value = "replacement", .cloud_store = "openai"}, epoch_);
    const auto current = cloud_json(application_->list_cloud_stores(key_, epoch_));
    EXPECT_TRUE(current["items"].empty());
    gate.release();
    EXPECT_EQ(cloud_json(old)["items"][0]["name"], "Old");
    EXPECT_EQ(cloud_json(application_->list_cloud_stores(key_, epoch_)), current);
}

TEST_F(FolderStoreOperationsTest, StartupAndRefreshReadGoogleDocumentPagesOnce) {
    const auto gemini = application_->create_api_key(
        {.display_name = "Unused Gemini", .value = "google-key", .cloud_store = "google"}, epoch_).id;
    (void)application_->create_api_key({.display_name = "Models", .value = "model-key", .cloud_store = "none"}, epoch_);
    Gate gate;
    std::atomic_int lists{};
    std::atomic_int details{};
    std::atomic_int first_document_page{};
    std::atomic_int second_document_page{};
    std::atomic_bool refreshed{};
    RuntimeSettings settings;
    settings.test_hooks.folder_store_transport = [&](const auto& request, const auto&) {
        if (store_list_request(request)) {
            ++lists;
            gate.arrive_and_wait();
            if (request.url.find("vector_stores") != std::string::npos)
                return json_ok(R"({"data":[{"id":"vs_1","name":"Research"}]})");
            EXPECT_EQ(request.headers.front().value, "google-key");
            return json_ok(R"({"fileSearchStores":[{"name":"fileSearchStores/a","displayName":"Gemini"}]})");
        }
        if (request.url.find("/documents?") != std::string::npos) {
            if (request.url.find("pageToken=next") == std::string::npos) {
                ++first_document_page;
                return json_ok(R"({"documents":[{"name":"fileSearchStores/a/documents/note","displayName":"note.md"}],"nextPageToken":"next"})");
            }
            ++second_document_page;
            const auto metadata = nlohmann::json{{"name", "fileSearchStores/a/documents/meta"},
                {"displayName", "DESCRIPTION.txt"}, {"customMetadata", nlohmann::json::array({
                    {{"key", "name"}, {"stringListValue", {{"values", {refreshed.load() ? "Gemini archive" : "Gemini library"}}}}},
                    {{"key", "description"}, {"stringListValue", {{"values", {refreshed.load() ? "Updated notes" : "Original notes"}}}}},
                })}};
            return json_ok(nlohmann::json{{"documents", nlohmann::json::array({metadata})}}.dump());
        }
        ++details;
        if (request.url.ends_with("/vs_1"))
            return json_ok(R"({"id":"vs_1","name":"Research","created_at":1793697300})");
        return json_ok(R"({"name":"fileSearchStores/a","displayName":"Gemini","createTime":"2026-11-03T09:15:00Z"})");
    };
    reopen(settings); // Returns even while the provider is blocked.
    ASSERT_TRUE(gate.wait_entered());
    EXPECT_TRUE(application_->running());
    gate.release();
    EXPECT_EQ(cloud_json(application_->get_cloud_store(key_, "vs_1", epoch_))["name"], "Research");
    const auto initial = cloud_json(application_->get_cloud_store(gemini, "fileSearchStores/a", epoch_));
    EXPECT_EQ(initial["name"], "Gemini library");
    EXPECT_EQ(initial["description"], "Original notes");
    EXPECT_EQ(initial["created_at"], "2026-11-03T09:15:00Z");
    EXPECT_EQ(lists.load(), 2);
    EXPECT_EQ(details.load(), 2);
    EXPECT_EQ(first_document_page.load(), 1);
    EXPECT_EQ(second_document_page.load(), 1);

    refreshed = true;
    const auto listed = cloud_json(application_->refresh_cloud_stores(gemini, epoch_));
    EXPECT_EQ(listed["items"], nlohmann::json::array({{{"id", "fileSearchStores/a"}, {"name", "Gemini archive"}}}));
    const auto updated = cloud_json(application_->get_cloud_store(gemini, "fileSearchStores/a", epoch_));
    EXPECT_EQ(updated["description"], "Updated notes");
    (void)cloud_json(application_->list_cloud_stores(gemini, epoch_));
    EXPECT_EQ(cloud_json(application_->get_cloud_store(key_, "vs_1", epoch_))["name"], "Research");
    EXPECT_EQ(lists.load(), 3);
    EXPECT_EQ(details.load(), 3);
    EXPECT_EQ(first_document_page.load(), 2);
    EXPECT_EQ(second_document_page.load(), 2);
}

TEST_F(FolderStoreOperationsTest, StoreEditsAndDeletesUpdateTheCachedSnapshot) {
    std::string name = "Research";
    std::string description;
    std::atomic_int calls{};
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto&) {
        ++calls;
        if (store_list_request(request)) return json_ok(R"({"data":[{"id":"vs_1","name":"Research"}]})");
        if (request.method == "DELETE") return json_ok(R"({"deleted":true})");
        if (request.url.find("/files?") != std::string::npos) return json_ok(R"({"data":[]})");
        if (request.method == "POST") {
            const auto body = nlohmann::json::parse(request.body);
            name = body["name"];
            description = body["metadata"]["description"];
            return json_ok(R"({"id":"vs_1"})");
        }
        return json_ok(nlohmann::json{{"id", "vs_1"}, {"name", name}, {"description", description}, {"created_at", 1793697300}}.dump());
    });
    (void)cloud_json(application_->list_cloud_stores(key_, epoch_));
    (void)cloud_json(application_->update_cloud_store(key_, "vs_1", "Archive", "Updated", epoch_));
    const auto after_edit = calls.load();
    EXPECT_EQ(cloud_json(application_->list_cloud_stores(key_, epoch_))["items"][0]["name"], "Archive");
    EXPECT_EQ(cloud_json(application_->get_cloud_store(key_, "vs_1", epoch_))["description"], "Updated");
    EXPECT_EQ(calls.load(), after_edit);
    (void)cloud_json(application_->delete_cloud_store(key_, "vs_1", epoch_));
    const auto after_delete = calls.load();
    EXPECT_TRUE(cloud_json(application_->list_cloud_stores(key_, epoch_))["items"].empty());
    EXPECT_TRUE(std::holds_alternative<OperationReply::Failure>(wait_cloud_reply(application_->get_cloud_store(key_, "vs_1", epoch_))));
    EXPECT_EQ(calls.load(), after_delete);
}

TEST_F(FolderStoreOperationsTest, FailedUpdateVerificationDoesNotKeepValuesFromBeforeTheWrite) {
    std::string name = "Research";
    bool fail_verification = false;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto&) {
        if (store_list_request(request)) return json_ok(R"({"data":[{"id":"vs_1","name":"Research"}]})");
        if (request.method == "POST") {
            name = nlohmann::json::parse(request.body)["name"];
            fail_verification = true;
            return json_ok(R"({"id":"vs_1"})");
        }
        if (std::exchange(fail_verification, false)) return FolderStoreHttpResponse{.status = 503};
        return json_ok(nlohmann::json{{"id", "vs_1"}, {"name", name}, {"created_at", 1793697300}}.dump());
    });
    EXPECT_EQ(cloud_json(application_->get_cloud_store(key_, "vs_1", epoch_))["name"], "Research");
    EXPECT_TRUE(std::holds_alternative<OperationReply::Failure>(wait_cloud_reply(
        application_->update_cloud_store(key_, "vs_1", "Archive", "", epoch_))));
    EXPECT_EQ(cloud_json(application_->get_cloud_store(key_, "vs_1", epoch_))["name"], "Archive");
}

TEST_F(FolderStoreOperationsTest, FailedDeleteResponseDoesNotKeepADeletedStoreInTheCache) {
    bool deleted = false;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto&) {
        if (store_list_request(request)) return json_ok(deleted ? R"({"data":[]})"
            : R"({"data":[{"id":"vs_1","name":"Research"}]})");
        if (request.url.find("/files?") != std::string::npos) return json_ok(R"({"data":[]})");
        if (request.method == "DELETE") {
            deleted = true;
            return FolderStoreHttpResponse{.status = 503};
        }
        return json_ok(R"({"id":"vs_1","name":"Research","created_at":1793697300})");
    });
    (void)cloud_json(application_->list_cloud_stores(key_, epoch_));
    EXPECT_TRUE(std::holds_alternative<OperationReply::Failure>(wait_cloud_reply(
        application_->delete_cloud_store(key_, "vs_1", epoch_))));
    EXPECT_TRUE(cloud_json(application_->list_cloud_stores(key_, epoch_))["items"].empty());
}

TEST_F(FolderStoreOperationsTest, UploadInvalidationKeepsConcurrentEditsOnTheSameCacheEntry) {
    Gate update_gate;
    std::atomic_bool edited{};
    std::atomic_bool created{};
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        if (store_list_request(request)) {
            auto data = nlohmann::json::array({{{"id", "vs_existing"}, {"name", "Before"}}});
            if (created.load()) data.push_back({{"id", "vs_1"}, {"name", "Research"}});
            return json_ok(nlohmann::json{{"data", data}}.dump());
        }
        if (request.url.ends_with("/vs_existing")) {
            if (request.method == "POST") {
                update_gate.arrive_and_wait();
                edited = true;
                return json_ok(R"({"id":"vs_existing"})");
            }
            return json_ok(nlohmann::json{{"id", "vs_existing"},
                {"name", edited.load() ? "After" : "Before"}, {"created_at", 1793697300}}.dump());
        }
        if (request.method == "GET" && request.url.ends_with("/vs_1"))
            return json_ok(R"({"id":"vs_1","name":"Research","created_at":1793697300})");
        if (request.method == "POST" && request.url.ends_with("/vector_stores")) created = true;
        return openai_transport()(request, cancel);
    });
    (void)cloud_json(application_->list_cloud_stores(key_, epoch_));
    const auto update = application_->update_cloud_store(key_, "vs_existing", "After", "", epoch_);
    ASSERT_TRUE(update_gate.wait_entered());
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
    EXPECT_EQ(wait_status(folder_.id).result, "success");
    const auto reader = application_->list_cloud_stores(key_, epoch_);
    auto ready = std::make_shared<std::promise<void>>();
    auto future = ready->get_future();
    reader->set_ready_callback([ready] { ready->set_value(); });
    // The upload invalidates the snapshot. Its next load must follow the edit.
    EXPECT_EQ(future.wait_for(100ms), std::future_status::timeout);
    update_gate.release();
    EXPECT_EQ(cloud_json(update)["name"], "After");
    EXPECT_EQ(cloud_json(reader)["items"][0]["name"], "After");
    EXPECT_EQ(cloud_json(application_->get_cloud_store(key_, "vs_existing", epoch_))["name"], "After");
}

TEST_F(FolderStoreOperationsTest, UploadInvalidationRejectsAnOlderRefreshSnapshot) {
    std::atomic_bool refresh_blocked{};
    std::atomic_bool created{};
    Gate refresh_gate;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        if (store_list_request(request)) {
            if (refresh_blocked.exchange(false)) {
                refresh_gate.arrive_and_wait();
                return json_ok(R"({"data":[]})");
            }
            return json_ok(created.load() ? R"({"data":[{"id":"vs_1","name":"Research"}]})" : R"({"data":[]})");
        }
        if (request.method == "GET" && request.url.ends_with("/vs_1"))
            return json_ok(R"({"id":"vs_1","name":"Research","created_at":1793697300})");
        if (request.method == "POST" && request.url.ends_with("/vector_stores")) created = true;
        return openai_transport()(request, cancel);
    });
    (void)cloud_json(application_->list_cloud_stores(key_, epoch_));
    refresh_blocked = true;
    const auto refresh = application_->refresh_cloud_stores(key_, epoch_);
    ASSERT_TRUE(refresh_gate.wait_entered());
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
    EXPECT_EQ(wait_status(folder_.id).result, "success");
    refresh_gate.release();
    // The Refresh reply is not an error, but its older result is not cached.
    EXPECT_TRUE(cloud_json(refresh)["items"].empty());
    EXPECT_EQ(cloud_json(application_->list_cloud_stores(key_, epoch_))["items"][0]["name"], "Research");
}

TEST_F(FolderStoreOperationsTest, FailedUploadCleanupInvalidatesTheStoreList) {
    std::atomic_bool created{};
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        if (store_list_request(request))
            return json_ok(created.load() ? R"({"data":[{"id":"vs_1","name":"Research"}]})" : R"({"data":[]})");
        if (request.method == "GET" && request.url.ends_with("/vs_1"))
            return json_ok(R"({"id":"vs_1","name":"Research","created_at":1793697300})");
        if (request.method == "DELETE") return FolderStoreHttpResponse{.status = 403};
        if (request.method == "POST" && request.url.ends_with("/vector_stores")) created = true;
        return openai_transport(true)(request, cancel);
    });
    EXPECT_TRUE(cloud_json(application_->list_cloud_stores(key_, epoch_))["items"].empty());
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
    EXPECT_TRUE(wait_status(folder_.id).known_cleanup_remains);
    EXPECT_EQ(cloud_json(application_->list_cloud_stores(key_, epoch_))["items"][0]["id"], "vs_1");
}

TEST_F(FolderStoreOperationsTest, StoreRemovedDuringDetailDownloadDoesNotBlockTheOtherStores) {
    application_->set_folder_store_transport_for_tests([](const auto& request, const auto&) {
        if (store_list_request(request))
            return json_ok(R"({"data":[{"id":"vs_removed","name":"Removed"},{"id":"vs_1","name":"Research"}]})");
        if (request.url.ends_with("/vs_removed")) return FolderStoreHttpResponse{.status = 404,
            .body = R"({"error":{"type":"invalid_request_error","message":"missing"}})"};
        return json_ok(R"({"id":"vs_1","name":"Research","created_at":1793697300})");
    });
    EXPECT_EQ(cloud_json(application_->list_cloud_stores(key_, epoch_))["items"],
        nlohmann::json::array({{{"id", "vs_1"}, {"name", "Research"}}}));
}

TEST_F(FolderStoreOperationsTest, UploadsThroughOpenAiFakeHttpAndOnlyAnEventRecordsTheStore) {
    application_->set_folder_store_transport_for_tests(openai_transport());
    const auto started = application_->start_folder_store_upload(
        folder_.id, "openai", key_, "", epoch_);
    EXPECT_GT(started.operation_id, 0);
    const auto status = wait_status(folder_.id);
    ASSERT_TRUE(status.present);
    EXPECT_EQ(status.phase, "completed");
    EXPECT_EQ(status.result, "success");
    const auto events = application_->list_events(epoch_);
    EXPECT_TRUE(std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.message.starts_with("Uploaded 1 notes from folder “Research”")
            && event.message.ends_with(" store=vs_1 file=file_1.");
    }));
    // The folder keeps no link, so the next upload creates another store.
    application_->close_folder_store_operation(started.operation_id, epoch_);
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
    EXPECT_EQ(wait_status(folder_.id).result, "success");
}

TEST_F(FolderStoreOperationsTest, UploadChoicesIncludeUnreferencedKeysForTheirService) {
    const auto unused = application_->create_api_key(
        {.display_name = "Unused", .value = "unused-test-key", .cloud_store = "openai"}, epoch_).id;
    const auto gemini = application_->create_api_key(
        {.display_name = "Gemini", .value = "gemini-test-key", .cloud_store = "google"}, epoch_).id;
    const auto choices = application_->folder_store_choices(epoch_);
    ASSERT_EQ(choices.size(), 3U);
    for (const auto& [id, service] : {std::pair{key_, "openai"}, {unused, "openai"}, {gemini, "google"}}) {
        EXPECT_EQ(std::ranges::count_if(choices, [&](const auto& choice) {
            return choice.service == service && choice.api_key_id == id;
        }), 1);
    }
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        EXPECT_EQ(request.headers.front().value, "Bearer unused-test-key");
        return openai_transport()(request, cancel);
    });
    (void)application_->start_folder_store_upload(folder_.id, "openai", unused, "", epoch_);
    EXPECT_EQ(wait_status(folder_.id).result, "success");
}

TEST_F(FolderStoreOperationsTest, UploadChoicesOmitKeysWithoutCloudStore) {
    const auto model_only = application_->create_api_key(
        {.display_name = "Model only", .value = "model-test-key"}, epoch_).id;
    const auto has_key = [&](const std::string& id) {
        const auto choices = application_->folder_store_choices(epoch_);
        return std::ranges::any_of(choices, [&](const auto& choice) { return choice.api_key_id == id; });
    };
    EXPECT_FALSE(has_key(model_only));
    EXPECT_TRUE(has_key(key_));

    (void)application_->replace_api_key_value(model_only, {.cloud_store = "google"}, epoch_);
    EXPECT_TRUE(has_key(model_only));
    (void)application_->replace_api_key_value(key_, {.cloud_store = "none"}, epoch_);
    EXPECT_FALSE(has_key(key_));
}

TEST_F(FolderStoreOperationsTest, UploadRefusesKeyForAnotherCloudStore) {
    int requests = 0;
    application_->set_folder_store_transport_for_tests([&](const auto&, const auto&) { ++requests; return json_ok("{}"); });
    const auto refuse = [&](std::string service) {
        try {
            (void)application_->start_folder_store_upload(folder_.id, service, key_, "", epoch_);
            FAIL() << "Expected a key for another cloud store to fail admission";
        } catch (const ApplicationError& error) {
            EXPECT_EQ(error.what(), std::string("Set cloud store API key"));
        }
        EXPECT_FALSE(application_->folder_store_operation_status(folder_.id, epoch_).present);
    };
    refuse("google");
    (void)application_->replace_api_key_value(key_, {.cloud_store = "none"}, epoch_);
    refuse("openai");
    refuse("google");
    EXPECT_EQ(requests, 0);
}

TEST_F(FolderStoreOperationsTest, UploadRefusesANameThatAStoreOfTheKeyAlreadyHas) {
    std::vector<std::string> requests;
    std::string list_body = nlohmann::json{{"data", {{{"id", "vs_old"}, {"name", store_name()}}}}, {"has_more", false}}.dump();
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        requests.push_back(request.method + " " + request.url);
        if (store_list_request(request)) {
            EXPECT_EQ(request.headers.front().value, "Bearer sk-test");
            return json_ok(list_body);
        }
        return openai_transport()(request, cancel);
    });
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
    auto status = wait_status(folder_.id);
    EXPECT_EQ(status.result, "preflight");
    EXPECT_EQ(status.error, "A store with this name already exists.");
    EXPECT_EQ(requests, std::vector<std::string>{"GET https://api.openai.com/v1/vector_stores?limit=100"});
    application_->close_folder_store_operation(status.operation_id, epoch_);

    list_body = R"({"error":{"message":"private"}})";
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        if (store_list_request(request)) return FolderStoreHttpResponse{.status = 401, .body = list_body};
        ADD_FAILURE() << "Unexpected request after a failed store list: " << request.url;
        return openai_transport()(request, cancel);
    });
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
    status = wait_status(folder_.id);
    EXPECT_EQ(status.result, "preflight");
    EXPECT_EQ(status.error, "The saved API key was rejected.");
    application_->close_folder_store_operation(status.operation_id, epoch_);

    list_body = nlohmann::json{{"data", {{{"id", "vs_folder"}, {"name", "Research " + note_item_ref_id(folder_.id)}},
        {{"id", "vs_case"}, {"name", "research"}},
        {{"id", "vs_unnamed"}, {"name", nullptr}}}}, {"has_more", false}}.dump();
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        if (store_list_request(request)) return json_ok(list_body);
        return openai_transport()(request, cancel);
    });
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
    EXPECT_EQ(wait_status(folder_.id).result, "success");
}

TEST_F(FolderStoreOperationsTest, UploadDoesNotRequireCompatibleModelProvider) {
    auto provider = application_->get_provider("test", epoch_);
    nlohmann::json body = provider;
    for (const char* field : {"id", "used_by", "writable"}) body.erase(field);
    body["api"] = "chat_completions";
    (void)application_->update_provider("test", body, epoch_);
    application_->set_folder_store_transport_for_tests(openai_transport());
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
    EXPECT_EQ(wait_status(folder_.id).result, "success");
}

TEST_F(FolderStoreOperationsTest, PreparationFailureIsDistinctAndLeavesNoStore) {
    const auto empty = application_->create_note_item({}, NoteItemKind::folder, "Empty", epoch_);
    application_->set_folder_store_transport_for_tests(openai_transport());
    const auto started = application_->start_folder_store_upload(empty.id, "openai", key_, "", epoch_);
    const auto status = wait_status(empty.id);
    EXPECT_EQ(status.result, "preflight");
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
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
    const auto status = wait_status(folder_.id);
    EXPECT_EQ(status.phase, "failed");
    EXPECT_NE(status.error.find("Note “Log”"), std::string::npos);
    EXPECT_NE(status.error.find("Check the note content before uploading again."), std::string::npos);
    EXPECT_EQ(status.error.find("private note text"), std::string::npos);
    ASSERT_GE(deletes.size(), 2U);
}

TEST_F(FolderStoreOperationsTest, GenericNotFoundKeepsTheIdForRetryCleanup) {
    application_->set_folder_store_transport_for_tests([&](const FolderStoreHttpRequest& request, const std::atomic_bool& cancel) {
        if (request.method == "DELETE") return FolderStoreHttpResponse{.status = 404, .body = "<html>nope</html>"};
        return openai_transport(true)(request, cancel);
    });
    const auto operation = application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
    const auto failed = wait_status(folder_.id);
    EXPECT_EQ(failed.result, "error");
    EXPECT_TRUE(failed.known_cleanup_remains);
    EXPECT_EQ(failed.error.find("vs_1"), std::string::npos);
    const auto events = application_->list_events(epoch_);
    EXPECT_TRUE(std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.message.find("store=vs_1 file=file_1") != std::string::npos;
    }));
    application_->set_folder_store_transport_for_tests(openai_transport());
    EXPECT_EQ(application_->retry_folder_store_cleanup(operation.operation_id, epoch_).operation_id, operation.operation_id);
    const auto done = wait_status(folder_.id);
    EXPECT_EQ(done.result, "success");
    EXPECT_FALSE(done.known_cleanup_remains);
}

TEST_F(FolderStoreOperationsTest, CancelDuringIndexingCleansUp) {
    std::promise<void> hold;
    hold_wait_ = hold.get_future().share();
    application_->set_folder_store_transport_for_tests(
        [](const FolderStoreHttpRequest& request, const std::atomic_bool&) {
            FolderStoreHttpResponse response;
            response.status = 200;
            if (store_list_request(request)) return json_ok(R"({"data":[]})");
            if (request.method == "DELETE") return json_ok(R"({"deleted":true})");
            if (request.url.find("/vector_stores") != std::string::npos
                && request.url.find("/files") == std::string::npos && request.method == "POST") {
                return json_ok(R"({"id":"vs_1","object":"vector_store","name":"Research","status":"completed"})");
            }
            if (request.multipart.size() == 2) return json_ok(R"({"id":"file_1"})");
            if (request.url.find("/files/file_1") != std::string::npos) {
                return json_ok(R"({"status":"in_progress"})");
            }
            return json_ok(R"({"id":"vsf_1","status":"in_progress"})");
        });
    const auto started = application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
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
}

TEST_F(FolderStoreOperationsTest, FolderDeleteNeverCallsTheProvider) {
    application_->set_folder_store_transport_for_tests(openai_transport());
    const auto operation = application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
    ASSERT_EQ(wait_status(folder_.id).result, "success");
    application_->close_folder_store_operation(operation.operation_id, epoch_);
    int requests = 0;
    application_->set_folder_store_transport_for_tests([&](const auto&, const auto&) { ++requests; return json_ok("{}"); });
    application_->delete_note_item(folder_.id, epoch_);
    EXPECT_EQ(requests, 0);
}

TEST_F(FolderStoreOperationsTest, DrainDoesNotJoinAWorkerWaitingForTheLifecycleLock) {
    auto gate = std::make_shared<Gate>();
    RuntimeSettings settings;
    settings.test_hooks.folder_store_before_lifetime_lock = [gate] { gate->arrive_and_wait(); };
    reopen(settings);
    application_->set_folder_store_transport_for_tests(openai_transport());
    const auto operation = application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
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

TEST_F(FolderStoreOperationsTest, ChildVaultCopyKeepsChatFolderRefs) {
    {
        storage::SqliteDatabase source(database_, storage::SqliteDatabase::Mode::read_write);
        source.execute("INSERT INTO sessions (forum_key,session_id,label,updated_at,history_epoch,next_entry_id,next_request_id) SELECT forum_key,'store-ref','Store ref',0,1,1,1 FROM forums LIMIT 1");
        source.prepare("INSERT INTO session_folders (session_key, folder_id) SELECT session_key, ?1 FROM sessions LIMIT 1", folder_.id).run();
    }
    const auto copy = application_->create_vault(
        {.display_name = "Child", .copy_from = "Test"}, epoch_);
    storage::SqliteDatabase destination(copy.data, storage::SqliteDatabase::Mode::read_only);
    EXPECT_FALSE(destination.prepare("SELECT 1 FROM sqlite_master WHERE name = 'folder_stores'").step());
    auto refs = destination.prepare("SELECT folder_id FROM session_folders");
    ASSERT_TRUE(refs.step());
    EXPECT_EQ(refs.integer(0), folder_.id);
}

TEST_F(FolderStoreOperationsTest, LostAllocationResponseStopsWithoutAutomaticReplay) {
    int calls = 0;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto&) {
        if (store_list_request(request)) return json_ok(R"({"data":[]})");
        ++calls;
        FolderStoreHttpResponse response;
        response.error = "secret-key echoed with note body";
        return response;
    });
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, "private description", epoch_);
    const auto status = wait_status(folder_.id);
    EXPECT_EQ(status.result, "error");
    EXPECT_EQ(calls, 1);
    EXPECT_FALSE(status.known_cleanup_remains);
    EXPECT_NE(status.error.find("The store connection failed."), std::string::npos);
    EXPECT_NE(status.error.find("Some remote data could not be confirmed deleted."), std::string::npos);
    EXPECT_EQ(status.error.find("secret-key"), std::string::npos);
    const auto events = application_->list_events(epoch_);
    EXPECT_TRUE(std::any_of(events.begin(), events.end(), [&](const auto& event) {
        return event.message.find("Unconfirmed allocation") != std::string::npos
            && event.message.find("name=" + nlohmann::json(store_name()).dump()) != std::string::npos
            && event.message.find("private description") == std::string::npos;
    }));
}

TEST_F(FolderStoreOperationsTest, FailedCleanupKeepsTheReturnedIdsInEvents) {
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        if (request.method == "DELETE") return FolderStoreHttpResponse{.status = 403};
        return openai_transport(true)(request, cancel);
    });
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
    const auto status = wait_status(folder_.id);
    EXPECT_TRUE(status.known_cleanup_remains);
    EXPECT_EQ(status.error.find("vs_1"), std::string::npos);
    const auto events = application_->list_events(epoch_);
    EXPECT_TRUE(std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.message.find("store=vs_1 file=file_1") != std::string::npos;
    }));
}

TEST_F(FolderStoreOperationsTest, CancellationLetsSentAllocationFinishThenCleansWithSeparateFlag) {
    Gate gate;
    int deleted = 0;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        if (request.method == "POST" && request.url.ends_with("/vector_stores")) gate.arrive_and_wait();
        if (request.method == "DELETE") { EXPECT_FALSE(cancel.load()); ++deleted; }
        return openai_transport()(request, cancel);
    });
    const auto operation = application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
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
}

TEST_F(FolderStoreOperationsTest, PreflightReservationProtectsAncestors) {
    Gate gate;
    RuntimeSettings settings;
    settings.test_hooks.folder_store_worker_started = [&](auto, auto) { gate.arrive_and_wait(); };
    reopen(settings);
    const auto parent = application_->create_note_item({}, NoteItemKind::folder, "Parent", epoch_);
    const auto child = application_->create_note_item(parent.id, NoteItemKind::folder, "Child", epoch_);
    application_->set_folder_store_transport_for_tests(openai_transport());
    const auto operation = application_->start_folder_store_upload(child.id, "openai", key_, "", epoch_);
    ASSERT_TRUE(gate.wait_entered());
    EXPECT_THROW(application_->delete_note_item(parent.id, epoch_), ApplicationError);
    EXPECT_THROW((void)application_->start_folder_store_upload(child.id, "openai", key_, "", epoch_), ApplicationError);
    application_->cancel_folder_store_upload(operation.operation_id, epoch_);
    gate.release();
    EXPECT_EQ(wait_status(child.id).result, "cancelled");
    application_->delete_note_item(parent.id, epoch_);
}

TEST_F(FolderStoreOperationsTest, ReadyCommitWinsOverLaterCancellationAndCloseAcknowledgesStatus) {
    application_->set_folder_store_transport_for_tests(openai_transport());
    const auto operation = application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
    ASSERT_EQ(wait_status(folder_.id).result, "success");
    application_->cancel_folder_store_upload(operation.operation_id, epoch_);
    EXPECT_EQ(application_->folder_store_operation_status(folder_.id, epoch_).result, "success");
    application_->close_folder_store_operation(operation.operation_id, epoch_);
    EXPECT_FALSE(application_->folder_store_operation_status(folder_.id, epoch_).present);
}

TEST_F(FolderStoreOperationsTest, CleanupAttemptsEveryFileAndRetryUsesOnlyRemainingIds) {
    const auto second = application_->create_note_item(folder_.id, NoteItemKind::note, "Second", epoch_);
    application_->save_note(second.id, text_doc("second fact"), epoch_);
    int files = 0;
    std::vector<std::string> deletes;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        if (request.method == "DELETE") {
            deletes.push_back(request.url);
            return FolderStoreHttpResponse{.status = request.url.ends_with("file_1") ? 401 : 200, .body = R"({"deleted":true})"};
        }
        if (request.multipart.size() == 2) return json_ok(R"({"id":"file_)" + std::to_string(++files) + R"("})");
        if (store_list_request(request)) return json_ok(R"({"data":[]})");
        if (request.method == "GET") return json_ok(R"({"status":"failed"})");
        return openai_transport()(request, cancel);
    });
    const auto operation = application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
    const auto failed = wait_status(folder_.id);
    EXPECT_TRUE(failed.known_cleanup_remains);
    EXPECT_EQ(failed.error.find("file_1"), std::string::npos);
    EXPECT_EQ(failed.error.find("vs_1"), std::string::npos);
    EXPECT_EQ(deletes.size(), 3U);
    const auto events = application_->list_events(epoch_);
    EXPECT_TRUE(std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.message.ends_with("Resource=file_1. file=file_1")
            && event.message.find("vs_1") == std::string::npos && event.message.find("file_2") == std::string::npos;
    }));
    deletes.clear();
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto&) {
        deletes.push_back(request.url);
        return json_ok(R"({"deleted":true})");
    });
    (void)application_->retry_folder_store_cleanup(operation.operation_id, epoch_);
    EXPECT_EQ(wait_status(folder_.id).result, "success");
    ASSERT_EQ(deletes.size(), 1U);
    EXPECT_TRUE(deletes.front().ends_with("file_1"));
}

TEST_F(FolderStoreOperationsTest, EarlierNotFoundWarningSurvivesCleanupRetry) {
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        if (request.method != "DELETE") return openai_transport(true)(request, cancel);
        return FolderStoreHttpResponse{.status = request.url.ends_with("vs_1") ? 404 : 401,
            .body = R"({"error":{"type":"invalid_request_error","message":"No such resource"}})"};
    });
    const auto operation = application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
    const auto failed = wait_status(folder_.id);
    EXPECT_EQ(failed.phase, "failed");
    EXPECT_TRUE(failed.known_cleanup_remains);
    application_->set_folder_store_transport_for_tests(openai_transport());
    (void)application_->retry_folder_store_cleanup(operation.operation_id, epoch_);
    EXPECT_EQ(wait_status(folder_.id).result, "warning");
    const auto events = application_->list_events(epoch_);
    EXPECT_TRUE(std::any_of(events.begin(), events.end(), [](const auto& event) {
        return event.severity == EventSeverity::warning && event.message.find("was not found for") != std::string::npos;
    }));
}

TEST_F(FolderStoreOperationsTest, LaunchFailureReleasesAdmissionWithoutRemoteEffects) {
    RuntimeSettings settings;
    settings.test_hooks.folder_store_before_launch = [] { throw std::runtime_error("launch failure"); };
    reopen(settings);
    EXPECT_THROW((void)application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_), ApplicationError);
    EXPECT_FALSE(application_->folder_store_operation_status(folder_.id, epoch_).present);
    application_->delete_note_item(folder_.id, epoch_);
}

TEST_F(FolderStoreOperationsTest, GoogleDocumentCleanupFailureKeepsParentForRetry) {
    bool first_deleted = false, fail_second = true, parent_deleted = false;
    int first_deletes = 0;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto&) {
        if (request.url.ends_with("/v1beta/fileSearchStores")) return json_ok(R"({"name":"fileSearchStores/test"})");
        if (request.capture_headers) return FolderStoreHttpResponse{.status = 500};
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
    (void)application_->replace_api_key_value(key_, {.cloud_store = "google"}, epoch_);
    const auto first = application_->start_folder_store_upload(folder_.id, "google", key_, "", epoch_);
    EXPECT_EQ(wait_status(folder_.id).result, "error");
    EXPECT_TRUE(application_->folder_store_operation_status(folder_.id, epoch_).known_cleanup_remains);
    EXPECT_FALSE(parent_deleted);

    fail_second = false;
    (void)application_->retry_folder_store_cleanup(first.operation_id, epoch_);
    EXPECT_EQ(wait_status(folder_.id).result, "success");
    EXPECT_EQ(first_deletes, 1);
    EXPECT_TRUE(parent_deleted);
}

TEST_F(FolderStoreOperationsTest, GoogleTransfersBeforePollingAndWaitsForLaggingCounts) {
    // Store uploads work independently of the configured OpenAI model provider.
    const auto second = application_->create_note_item(folder_.id, NoteItemKind::note, "Second", epoch_);
    application_->save_note(second.id, text_doc("second fact"), epoch_);
    int uploads = 0, counts = 0, polls = 0;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto&) {
        if (store_list_request(request)) return json_ok("{}");
        if (request.method == "DELETE") return json_ok("{}");
        if (request.url.find("/documents?") != std::string::npos) return json_ok("{}");
        if (request.url.ends_with("/v1beta/fileSearchStores")) {
            EXPECT_EQ(nlohmann::json::parse(request.body).at("displayName"), "Research");
            return json_ok(R"({"name":"fileSearchStores/store1"})");
        }
        if (request.url.ends_with(":uploadToFileSearchStore")) {
            if (uploads == 0) {
                const auto metadata = nlohmann::json::parse(request.body).at("customMetadata");
                EXPECT_EQ(metadata.at(0).at("stringListValue").at("values"), nlohmann::json::array({"Research"}));
            }
            return json_ok("", {{"X-Goog-Upload-URL", "https://generativelanguage.googleapis.com/upload/session"}});
        }
        if (request.url.ends_with("/upload/session")) {
            ++uploads;
            return json_ok(nlohmann::json{{"name", "fileSearchStores/store1/upload/operations/op" + std::to_string(uploads)}}.dump());
        }
        EXPECT_EQ(uploads, 3);
        if (request.url.find("/operations/") != std::string::npos) {
            ++polls;
            return json_ok(nlohmann::json{{"name", request.url.substr(std::string("https://generativelanguage.googleapis.com/v1beta/").size())}, {"done", polls != 1}}.dump());
        }
        ++counts;
        return json_ok(counts == 1 ? R"({"activeDocumentsCount":"1","pendingDocumentsCount":"1"})" : R"({"activeDocumentsCount":"3"})");
    });
    (void)application_->replace_api_key_value(key_, {.cloud_store = "google"}, epoch_);
    (void)application_->start_folder_store_upload(folder_.id, "google", key_, "", epoch_);
    const auto status = wait_status(folder_.id);
    EXPECT_EQ(status.result, "success");
    EXPECT_EQ(status.completed, 2);
    EXPECT_EQ(status.total, 2);
    EXPECT_EQ(uploads, 3);
    EXPECT_EQ(polls, 4);
    EXPECT_EQ(counts, 2);
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
        if (store_list_request(request)) return json_ok("{}");
        if (request.url.find("/documents?") != std::string::npos) return json_ok("{}");
        if (request.method == "DELETE") {
            ++deleted;
            EXPECT_TRUE(request.url.ends_with("/fileSearchStores/test?force=true"));
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
    (void)application_->replace_api_key_value(key_, {.cloud_store = "google"}, epoch_);
    (void)application_->start_folder_store_upload(folder_.id, "google", key_, "", epoch_);
    const auto done = wait_status(folder_.id);
    EXPECT_EQ(done.result, "cancelled");
    EXPECT_EQ(finalized, 0);
    EXPECT_EQ(deleted, 1);
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
        if (store_list_request(request)) return json_ok("{}");
        if (request.url.find("/documents?") != std::string::npos)
            return json_ok(R"({"documents":[{"name":"fileSearchStores/test/documents/doc1"}]})");
        if (request.method == "DELETE") {
            deleted.push_back(request.url);
            EXPECT_FALSE(cancel.load());
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
    (void)application_->replace_api_key_value(key_, {.cloud_store = "google"}, epoch_);
    (void)application_->start_folder_store_upload(folder_.id, "google", key_, "", epoch_);
    const auto done = wait_status(folder_.id);
    EXPECT_EQ(done.result, "cancelled");
    EXPECT_EQ(finalized, 1);
    EXPECT_EQ(deleted, (std::vector<std::string>{
        "https://generativelanguage.googleapis.com/v1beta/fileSearchStores/test/documents/doc1?force=true",
        "https://generativelanguage.googleapis.com/v1beta/fileSearchStores/test?force=true"}));
}

TEST_F(FolderStoreOperationsTest, VaultSwitchDuringAllocationStopsTheWorker) {
    Gate gate;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        if (request.method == "POST") gate.arrive_and_wait();
        return openai_transport()(request, cancel);
    });
    const auto operation = application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
    ASSERT_TRUE(gate.wait_entered());
    (void)application_->create_vault({.display_name = "Switched"}, epoch_);
    (void)application_->switch_vault("Switched", {}, epoch_);
    epoch_ = application_->context_epoch();
    gate.release();
    {
        std::unique_lock lock(finished_mutex_);
        ASSERT_TRUE(finished_cv_.wait_for(lock, 5s, [&] { return finished_.contains(operation.operation_id); }));
    }
    EXPECT_FALSE(application_->folder_store_operation_status(folder_.id, epoch_).present);
}

TEST_F(FolderStoreOperationsTest, VaultSwitchStopsCloudStoreDeletionBeforeTheNextProviderCall) {
    Gate gate;
    std::mutex mutex;
    std::vector<std::string> requests;
    bool switched = false;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto&) {
        // The copied vault starts its own background cache load.
        if (store_list_request(request)) return json_ok(R"({"data":[]})");
        {
            const std::lock_guard lock(mutex);
            if (switched) ADD_FAILURE() << "Provider call after the vault switch: " << request.url;
            requests.push_back(request.method + " " + request.url);
        }
        if (request.method == "GET") return json_ok(R"({"data":[{"id":"file_1"},{"id":"file_2"}],"has_more":false})");
        gate.arrive_and_wait();
        return json_ok(R"({"deleted":true})");
    });
    const auto reply = application_->delete_cloud_store(key_, "vs_1", epoch_);
    ASSERT_TRUE(gate.wait_entered());
    (void)application_->create_vault({.display_name = "Switched"}, epoch_);
    (void)application_->switch_vault("Switched", {}, epoch_);
    epoch_ = application_->context_epoch();
    {
        const std::lock_guard lock(mutex);
        switched = true;
    }
    gate.release();
    for (const auto deadline = std::chrono::steady_clock::now() + 5s;
         !reply->peek() && std::chrono::steady_clock::now() < deadline;) std::this_thread::sleep_for(2ms);
    const auto result = reply->peek();
    ASSERT_TRUE(result);
    const auto* failure = std::get_if<OperationReply::Failure>(&*result);
    ASSERT_NE(failure, nullptr);
    EXPECT_EQ(failure->code, ErrorCode::operation_cancelled);
    const std::lock_guard lock(mutex);
    EXPECT_EQ(requests, (std::vector<std::string>{
        "GET https://api.openai.com/v1/vector_stores/vs_1/files?limit=100",
        "DELETE https://api.openai.com/v1/files/file_1"}));
}

TEST_F(FolderStoreOperationsTest, SavedSnapshotSurvivesSourceEditsAndUsesCurrentKeyEachRequest) {
    const auto items = application_->list_note_items(epoch_);
    const auto note = *std::find_if(items.begin(), items.end(), [&](const auto& item) { return item.parent_id == folder_.id && item.kind == NoteItemKind::note; });
    bool uploaded = false;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        if (store_list_request(request)) return json_ok(R"({"data":[]})");
        if (request.method == "POST" && request.url.ends_with("/vector_stores")) {
            (void)application_->replace_api_key_value(key_, {.value = "replacement-key", .cloud_store = "openai"}, epoch_);
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
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
    EXPECT_EQ(wait_status(folder_.id).result, "success");
    EXPECT_TRUE(uploaded);
}

TEST_F(FolderStoreOperationsTest, ShutdownDeadlineRetainsDependenciesUntilLateWorkerFinishes) {
    auto gate = std::make_shared<Gate>();
    application_->set_folder_store_transport_for_tests([gate](const auto&, const auto&) {
        gate->arrive_and_wait();
        return json_ok(R"({"id":"vs_late"})");
    });
    const auto operation = application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
    ASSERT_TRUE(gate->wait_entered());
    application_->request_shutdown();
    EXPECT_FALSE(application_->join_shutdown(0ms));
    gate->release();
    {
        std::unique_lock lock(finished_mutex_);
        ASSERT_TRUE(finished_cv_.wait_for(lock, 5s, [&] { return finished_.contains(operation.operation_id); }));
    }
    EXPECT_TRUE(application_->join_shutdown(2s));
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
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
    const auto status = wait_status(folder_.id);
    EXPECT_EQ(status.result, "error");
    EXPECT_EQ(status.phase, "failed");
}

TEST_F(FolderStoreOperationsTest, CancelCannotMissThePollWakeupAfterPredicateRead) {
    auto gate = std::make_shared<Gate>();
    auto once = std::make_shared<std::once_flag>();
    RuntimeSettings settings;
    settings.folder_store_poll_interval = 3s;
    settings.test_hooks.folder_store_poll_predicate_read = [gate, once] {
        std::call_once(*once, [&] { gate->arrive_and_wait(); });
    };
    reopen(settings);
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        if (store_list_request(request)) return json_ok(R"({"data":[]})");
        if (request.method == "GET") return json_ok(R"({"status":"in_progress"})");
        return openai_transport()(request, cancel);
    });
    const auto operation = application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
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
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        if (request.method == "DELETE") return FolderStoreHttpResponse{.status = 401};
        return openai_transport(true)(request, cancel);
    });
    const auto first = application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
    ASSERT_EQ(wait_status(folder_.id).result, "error");
    auto gate = std::make_shared<Gate>();
    application_->set_folder_store_transport_for_tests([gate](const auto&, const auto&) {
        gate->arrive_and_wait(); return json_ok(R"({"deleted":true})");
    });
    (void)application_->retry_folder_store_cleanup(first.operation_id, epoch_);
    ASSERT_TRUE(gate->wait_entered());
    const auto retry = application_->folder_store_operation_status(folder_.id, epoch_);
    EXPECT_FALSE(retry.terminal); EXPECT_TRUE(retry.result.empty());
    EXPECT_EQ(retry.phase, "cleaning");
    EXPECT_THROW((void)application_->retry_folder_store_cleanup(first.operation_id, epoch_), ApplicationError);
    gate->release();
    EXPECT_EQ(wait_status(folder_.id).result, "success");
    application_->close_folder_store_operation(first.operation_id, epoch_);
    EXPECT_THROW(application_->close_folder_store_operation(first.operation_id, epoch_), ApplicationError);
    EXPECT_THROW(application_->cancel_folder_store_upload(first.operation_id, epoch_), ApplicationError);
    EXPECT_THROW((void)application_->retry_folder_store_cleanup(first.operation_id, epoch_), ApplicationError);
}

TEST_F(FolderStoreOperationsTest, FailedRetryLaunchRetainsACompleteFailureStatusAndReservation) {
    auto fail_launch = std::make_shared<bool>(false);
    RuntimeSettings settings;
    settings.test_hooks.folder_store_before_launch = [fail_launch] { if (*fail_launch) throw std::runtime_error("launch failure"); };
    reopen(settings);
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        if (request.method == "DELETE") return FolderStoreHttpResponse{.status = 401};
        return openai_transport(true)(request, cancel);
    });
    const auto operation = application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
    ASSERT_EQ(wait_status(folder_.id).result, "error");
    *fail_launch = true;
    EXPECT_THROW((void)application_->retry_folder_store_cleanup(operation.operation_id, epoch_), ApplicationError);
    const auto status = application_->folder_store_operation_status(folder_.id, epoch_);
    EXPECT_TRUE(status.terminal);
    EXPECT_EQ(status.phase, "failed"); EXPECT_EQ(status.result, "error");
    EXPECT_FALSE(status.error.empty());
    EXPECT_TRUE(status.known_cleanup_remains);
    application_->close_folder_store_operation(operation.operation_id, epoch_);
    EXPECT_FALSE(application_->folder_store_operation_status(folder_.id, epoch_).present);
}

TEST_F(FolderStoreOperationsTest, UploadFailureWithFinishedCleanupLeavesNoCleanup) {
    int requests = 0;
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        ++requests;
        return openai_transport(true)(request, cancel);
    });
    const auto preflight = application_->start_folder_store_upload(folder_.id, "openai", "missing", "", epoch_);
    auto status = wait_status(folder_.id);
    EXPECT_EQ(status.result, "preflight"); EXPECT_FALSE(status.known_cleanup_remains);
    EXPECT_EQ(requests, 0);
    application_->close_folder_store_operation(preflight.operation_id, epoch_);
    // A failed remote upload whose own cleanup succeeded also leaves no cleanup.
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
    status = wait_status(folder_.id);
    EXPECT_EQ(status.result, "error"); EXPECT_FALSE(status.known_cleanup_remains);
}

TEST_F(FolderStoreOperationsTest, SecondaryProcessCannotMutateOperations) {
    auto secondary = Application::open(make_command(workspace_, database_));
    const auto epoch = secondary->context_epoch();
    EXPECT_NO_THROW((void)secondary->folder_store_choices(epoch));
    EXPECT_FALSE(secondary->folder_store_operation_status(folder_.id, epoch).present);
    EXPECT_THROW((void)secondary->start_folder_store_upload(folder_.id, "openai", key_, "", epoch), SecondaryProcessError);
    EXPECT_THROW((void)secondary->retry_folder_store_cleanup(1, epoch), SecondaryProcessError);
    EXPECT_THROW(secondary->cancel_folder_store_upload(1, epoch), SecondaryProcessError);
    EXPECT_THROW(secondary->close_folder_store_operation(1, epoch), SecondaryProcessError);
    secondary->request_shutdown();
    EXPECT_TRUE(secondary->join_shutdown(2s));
}

TEST_F(FolderStoreOperationsTest, FailedIndexingKeepsNotFoundCleanupWarningInStatus) {
    application_->set_folder_store_transport_for_tests(openai_transport(true, true));
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
    const auto status = wait_status(folder_.id);
    EXPECT_EQ(status.result, "error");
    EXPECT_TRUE(status.terminal);
    EXPECT_FALSE(status.known_cleanup_remains);
    EXPECT_NE(status.error.find("Indexing did not complete."), std::string::npos);
    EXPECT_NE(status.error.find("Some remote data could not be confirmed deleted."), std::string::npos);
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
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, "", epoch_);
    const auto status = wait_status(folder_.id);
    EXPECT_EQ(status.result, "error");
    EXPECT_FALSE(status.known_cleanup_remains);
    EXPECT_NE(status.error.find("Some remote data could not be confirmed deleted."), std::string::npos);
}

TEST_F(FolderStoreOperationsTest, MetadataValidationPrecedesReservationAndRemoteAllocation) {
    int requests = 0;
    application_->set_folder_store_transport_for_tests([&](const auto&, const auto&) { ++requests; return json_ok("{}"); });
    const auto reject = [&](std::string service, std::string description, std::string message) {
        try {
            (void)application_->start_folder_store_upload(folder_.id, service, key_, description, epoch_);
            FAIL() << "Expected invalid metadata to fail admission";
        } catch (const ApplicationError& error) { EXPECT_EQ(error.what(), message); }
        EXPECT_FALSE(application_->folder_store_operation_status(folder_.id, epoch_).present);
        EXPECT_EQ(requests, 0);
    };
    reject("openai", std::string(511, 'x') + "\n😀", "Description must be 512 characters or fewer for OpenAI.");
    reject("openai", std::string(511, 'x') + "\n\n", "Description must be 512 characters or fewer for OpenAI.");
    reject("google", std::string("\xc0\xaf", 2), "Description must be valid UTF-8.");
    // A missing key gives asynchronous preflight failure, proving local admission
    // accepts the boundary without allocating a remote store.
    for (const auto& service : {"openai", "google"}) {
        const auto started = application_->start_folder_store_upload(folder_.id, service, "missing",
            service == std::string("openai") ? std::string(510, 'a') + "\n😀" : std::string(2000, 'a'), epoch_);
        EXPECT_EQ(wait_status(folder_.id).result, "preflight");
        application_->close_folder_store_operation(started.operation_id, epoch_);
    }
    EXPECT_EQ(requests, 0);
}

TEST_F(FolderStoreOperationsTest, ChosenMetadataSurvivesFolderRenameBeforeCapture) {
    Gate gate;
    RuntimeSettings settings;
    settings.folder_store_poll_interval = 0ms;
    settings.test_hooks.folder_store_worker_started = [&](auto, auto) { gate.arrive_and_wait(); };
    reopen(settings);
    const auto transport = openai_transport();
    application_->set_folder_store_transport_for_tests([&](const auto& request, const auto& cancel) {
        if (request.url.ends_with("/vector_stores") && request.method == "POST") {
            const auto body = nlohmann::json::parse(request.body);
            EXPECT_EQ(body.at("name"), store_name());
            EXPECT_EQ(body.at("description"), " first\n\n第二段 😀 ");
        }
        return transport(request, cancel);
    });
    (void)application_->start_folder_store_upload(folder_.id, "openai", key_, " first\n\n第二段 😀 ", epoch_);
    ASSERT_TRUE(gate.wait_entered());
    (void)application_->rename_note_item(folder_.id, "Renamed", epoch_);
    gate.release();
    EXPECT_EQ(wait_status(folder_.id).result, "success");
}

TEST_F(FolderStoreOperationsTest, GoogleMetadataFailuresCleanStoreAndKeepMetadataContext) {
    for (const std::string stage : {"start", "finalize", "poll", "extra", "missing", "cleanup"}) {
        SCOPED_TRACE(stage);
        int deletes = 0, starts = 0, waits = 0;
        application_->set_folder_store_transport_for_tests([&](const auto& request, const auto&) {
            if (request.url.find("/documents?") != std::string::npos) return json_ok("{}");
            if (request.method == "DELETE") { ++deletes; return stage == "cleanup" ? FolderStoreHttpResponse{.status = 500} : json_ok("{}"); }
            if (request.url.ends_with("/v1beta/fileSearchStores")) return json_ok(R"({"name":"fileSearchStores/test"})");
            if (request.capture_headers) {
                ++starts;
                if (stage == "start" || stage == "cleanup") return FolderStoreHttpResponse{.status = 400, .body = R"({"error":{"message":"private description"}})"};
                return json_ok("", {{"X-Goog-Upload-URL", "https://generativelanguage.googleapis.com/upload/session"}});
            }
            if (request.url.ends_with("/upload/session")) {
                if (stage == "finalize") return FolderStoreHttpResponse{.status = 400, .body = R"({"error":{"message":"private description"}})"};
                return json_ok(nlohmann::json{{"name", "fileSearchStores/test/upload/operations/op" + std::to_string(starts)}, {"done", stage != "poll"}}.dump());
            }
            if (request.url.find("/operations/") != std::string::npos)
                return json_ok(nlohmann::json{{"name", "fileSearchStores/test/upload/operations/op1"}, {"done", true}, {"error", {{"code", 3}, {"message", "private description"}}}}.dump());
            if (stage == "missing") {
                if (++waits == 2) application_->cancel_folder_store_upload(application_->folder_store_operation_status(folder_.id, epoch_).operation_id, epoch_);
                return json_ok(R"({"activeDocumentsCount":"1"})");
            }
            return json_ok(R"({"activeDocumentsCount":"3"})");
        });
        (void)application_->replace_api_key_value(key_, {.cloud_store = "google"}, epoch_);
        const auto started = application_->start_folder_store_upload(folder_.id, "google", key_, "private description", epoch_);
        const auto status = wait_status(folder_.id);
        EXPECT_EQ(status.result, stage == "missing" ? "cancelled" : "error");
        EXPECT_GE(deletes, 1);
        EXPECT_EQ(status.error.find("private description"), std::string::npos);
        if (stage == "start" || stage == "finalize" || stage == "cleanup")
            EXPECT_TRUE(status.error.starts_with("Store metadata: The provider rejected the store name or description."));
        if (stage == "poll") EXPECT_TRUE(status.error.starts_with("Store metadata: "));
        EXPECT_EQ(status.known_cleanup_remains, stage == "cleanup");
        application_->close_folder_store_operation(started.operation_id, epoch_);
    }
}


TEST_F(FolderStoreOperationsTest, StoreCreationRejectionIsSafeAndRetainsOtherErrorReasons) {
    for (const int status_code : {400, 401, 429}) {
        application_->set_folder_store_transport_for_tests([&](const auto& request, const auto&) {
            if (store_list_request(request)) return json_ok(R"({"data":[]})");
            return FolderStoreHttpResponse{.status = status_code,
                .body = R"({"error":{"message":"private description"}})"};
        });
        const auto started = application_->start_folder_store_upload(folder_.id, "openai", key_, "private description", epoch_);
        const auto status = wait_status(folder_.id);
        EXPECT_EQ(status.result, "error");
        EXPECT_EQ(status.error.find("private description"), std::string::npos);
        if (status_code == 400) EXPECT_EQ(status.error, "The provider rejected the store name or description.");
        if (status_code == 401) EXPECT_EQ(status.error, "The saved API key was rejected.");
        if (status_code == 429) EXPECT_TRUE(status.error.starts_with("The provider quota or rate limit was reached."));
        application_->close_folder_store_operation(started.operation_id, epoch_);
    }
    application_->set_folder_store_transport_for_tests([](const auto& request, const auto&) {
        if (store_list_request(request)) return json_ok("{}");
        return FolderStoreHttpResponse{.status = 400, .body = R"({"error":{"code":400,"status":"INVALID_ARGUMENT","message":"private description",)"
            R"("details":[{"@type":"type.googleapis.com/google.rpc.ErrorInfo","reason":"API_KEY_INVALID"}]}})"};
    });
    (void)application_->replace_api_key_value(key_, {.cloud_store = "google"}, epoch_);
    const auto started = application_->start_folder_store_upload(folder_.id, "google", key_, "private description", epoch_);
    const auto status = wait_status(folder_.id);
    EXPECT_EQ(status.result, "error");
    EXPECT_EQ(status.error, "The saved API key was rejected.");
    application_->close_folder_store_operation(started.operation_id, epoch_);
}

} // namespace
} // namespace cha::app
