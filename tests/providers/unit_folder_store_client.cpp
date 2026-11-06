#include "providers/folder_store_client.h"
#include "providers/api_key_store.h"
#include "support/test_workspace.h"
#include "support/mock_http_server.h"
#include "util/logging.h"
#include <fstream>
#include "workspace/workspace_config_store.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

namespace cha {
namespace {

ModelBackendConfig openai_config(std::string key_id = "api_key_1") {
    ModelBackendConfig config;
    config.host = "api.openai.com";
    config.port = 443;
    config.base_path = {};
    config.mode = Mode::net;
    config.https = true;
    config.api = ProviderApi::responses;
    config.auth = ProviderAuth::none;
    config.api_key_id = std::move(key_id);
    return config;
}

ModelBackendConfig google_config(std::string key_id = "api_key_1") {
    ModelBackendConfig config;
    config.host = "generativelanguage.googleapis.com";
    config.port = 443;
    config.base_path = "/v1beta";
    config.mode = Mode::net;
    config.https = true;
    config.api = ProviderApi::google;
    config.auth = ProviderAuth::none;
    config.api_key_id = std::move(key_id);
    return config;
}

class FolderStoreClientTest : public testing::Test {
protected:
    void SetUp() override {
        database_ = test::import_test_database(workspace_.root());
        config_ = WorkspaceConfigStore::open(database_);
        keys_ = std::make_unique<ApiKeyStore>(*config_);
        openai_key_ = keys_->create("OpenAI", "sk-test").id;
        google_key_ = keys_->create("Google", "google-test").id;
    }

    test::TestWorkspace workspace_;
    std::filesystem::path database_;
    std::unique_ptr<WorkspaceConfigStore> config_;
    std::unique_ptr<ApiKeyStore> keys_;
    std::string openai_key_;
    std::string google_key_;
};

TEST_F(FolderStoreClientTest, MatchesExactOpenAiAndGoogleEndpoints) {
    auto openai = openai_config(openai_key_);
    auto matched = folder_store_match(openai, keys_->list());
    ASSERT_TRUE(matched.service);
    EXPECT_EQ(*matched.service, FolderStoreService::openai);
    EXPECT_EQ(matched.api_key_id, openai_key_);

    openai.host = "API.OpenAI.COM";
    EXPECT_TRUE(folder_store_match(openai, keys_->list()).service);

    openai.host = "proxy.api.openai.com";
    EXPECT_FALSE(folder_store_match(openai, keys_->list()).service);

    openai = openai_config(openai_key_);
    openai.port = 8443;
    EXPECT_FALSE(folder_store_match(openai, keys_->list()).service);
    openai = openai_config(openai_key_);
    openai.https = false;
    EXPECT_FALSE(folder_store_match(openai, keys_->list()).service);
    openai = openai_config(openai_key_);
    openai.mode = Mode::test;
    EXPECT_FALSE(folder_store_match(openai, keys_->list()).service);
    openai = openai_config(openai_key_);
    openai.api = ProviderApi::chat_completions;
    EXPECT_FALSE(folder_store_match(openai, keys_->list()).service);
    openai = openai_config(openai_key_);
    openai.auth = ProviderAuth::openai_subscription;
    EXPECT_FALSE(folder_store_match(openai, keys_->list()).service);
    openai = openai_config(openai_key_);
    openai.base_path = "/v1";
    EXPECT_FALSE(folder_store_match(openai, keys_->list()).service);

    auto google = google_config(google_key_);
    matched = folder_store_match(google, keys_->list());
    ASSERT_TRUE(matched.service);
    EXPECT_EQ(*matched.service, FolderStoreService::google);

    google.base_path = "/v1";
    EXPECT_FALSE(folder_store_match(google, keys_->list()).service);
}

TEST_F(FolderStoreClientTest, ResolvesLegacyNamesAndRejectsMissingKeys) {
    auto config = openai_config();
    config.api_key_id.clear();
    config.api_key_env = "OpenAI";
    auto matched = folder_store_match(config, keys_->list());
    ASSERT_TRUE(matched.service);
    EXPECT_EQ(matched.api_key_id, openai_key_);

    config = openai_config({});
    EXPECT_FALSE(folder_store_match(config, keys_->list()).service);

    config = openai_config("missing");
    EXPECT_FALSE(folder_store_match(config, keys_->list()).service);
}

TEST_F(FolderStoreClientTest, ClassifiesEndpointsBeforeAuthenticationAndKeyValidation) {
    for (auto config : {openai_config("missing"), google_config("missing")}) {
        const auto expected = config.api == ProviderApi::responses
            ? FolderStoreService::openai : FolderStoreService::google;
        EXPECT_EQ(folder_store_endpoint_service(config), expected);
        EXPECT_FALSE(folder_store_match(config, keys_->list()).service);
        config.auth = ProviderAuth::openai_subscription;
        EXPECT_EQ(folder_store_endpoint_service(config), expected);
        EXPECT_FALSE(folder_store_match(config, keys_->list()).service);
        config.https = false;
        EXPECT_FALSE(folder_store_endpoint_service(config));
        config.https = true;
        config.port = 8443;
        EXPECT_FALSE(folder_store_endpoint_service(config));
        config.port = 443;
        config.base_path = "/proxy";
        EXPECT_FALSE(folder_store_endpoint_service(config));
    }
    auto config = openai_config(openai_key_);
    config.host = "API.OpenAI.COM";
    EXPECT_EQ(folder_store_endpoint_service(config), FolderStoreService::openai);
    config.api = ProviderApi::chat_completions;
    EXPECT_FALSE(folder_store_endpoint_service(config));
    config = google_config(google_key_);
    config.host = "proxy.example.com";
    EXPECT_FALSE(folder_store_endpoint_service(config));
    config = openai_config(openai_key_);
    config.mode = Mode::test;
    EXPECT_FALSE(folder_store_endpoint_service(config));
}

TEST(FolderStoreHttp, OpenAiSequenceUsesFixedOriginsAndMarkdownMultipart) {
    std::vector<FolderStoreHttpRequest> seen;
    FolderStoreClient client([&](const FolderStoreHttpRequest& request, const std::atomic_bool&) {
        seen.push_back(request);
        FolderStoreHttpResponse response;
        response.status = 200;
        if (request.url.find("/vector_stores") != std::string::npos
            && request.method == "POST" && request.url.find("/files") == std::string::npos) {
            response.body = R"({"id":"vs_1","object":"vector_store","name":"Research ##00000B","status":"completed"})";
        } else if (request.url.ends_with("/files") && request.method == "POST"
            && request.multipart.size() == 2) {
            EXPECT_EQ(request.multipart[0].data, "assistants");
            EXPECT_EQ(request.multipart[1].mime_type, "text/markdown");
            response.body = R"({"id":"file_1"})";
        } else if (request.url.find("/vector_stores/vs_1/files/file_1") != std::string::npos) {
            response.body = R"({"id":"file_1","status":"completed"})";
        } else if (request.url.find("/vector_stores/vs_1/files") != std::string::npos) {
            response.body = R"({"id":"vsf_1","status":"in_progress"})";
        } else if (request.method == "DELETE") {
            response.body = R"({"deleted":true})";
        }
        return response;
    });
    std::atomic_bool cancel{false};
    EXPECT_EQ(client.openai_create_store("Research ##00000B", "", "sk", cancel).id, "vs_1");
    EXPECT_EQ(client.openai_upload_file("note-1.md", "# Note", "sk", cancel).id, "file_1");
    EXPECT_EQ(client.openai_attach_file("vs_1", "file_1", "sk", cancel).kind, FolderStoreRemoteKind::ok);
    EXPECT_EQ(client.openai_inspect_file("vs_1", "file_1", "sk", cancel).status, "completed");
    EXPECT_EQ(client.openai_delete_store("vs_1", "sk", cancel).kind, FolderStoreRemoteKind::ok);
    EXPECT_EQ(client.openai_delete_file("file_1", "sk", cancel).kind, FolderStoreRemoteKind::ok);
    ASSERT_GE(seen.size(), 6U);
    EXPECT_EQ(seen[0].headers[0].name, "Authorization");
    EXPECT_TRUE(seen[0].url.starts_with("https://api.openai.com/v1/"));
}

TEST(FolderStoreHttp, IndexFailureAndStructuredNotFound) {
    FolderStoreClient client([](const FolderStoreHttpRequest& request, const std::atomic_bool&) {
        FolderStoreHttpResponse response;
        if (request.url.find("/files/file_bad") != std::string::npos) {
            response.status = 200;
            response.body = R"({"status":"failed"})";
        } else if (request.method == "DELETE" && request.url.find("/files/") != std::string::npos) {
            response.status = 404;
            response.body = R"({"error":{"message":"No such file","type":"invalid_request_error"}})";
        } else {
            response.status = 404;
            response.body = "<html>not found</html>";
        }
        return response;
    });
    std::atomic_bool cancel{false};
    EXPECT_EQ(client.openai_inspect_file("vs_1", "file_bad", "sk", cancel).kind, FolderStoreRemoteKind::failed);
    EXPECT_EQ(client.openai_delete_file("file_1", "sk", cancel).kind, FolderStoreRemoteKind::not_found);
    EXPECT_EQ(client.openai_delete_store("vs_1", "sk", cancel).kind, FolderStoreRemoteKind::failed);
}

TEST(FolderStoreHttp, GoogleUploadValidatesOperationAndCounts) {
    int inspect = 0;
    FolderStoreClient client([&](const FolderStoreHttpRequest& request, const std::atomic_bool&) {
        FolderStoreHttpResponse response;
        response.status = 200;
        if (request.url.find("fileSearchStores") != std::string::npos && request.method == "POST"
            && request.url.find("upload") == std::string::npos) {
            response.body = R"({"name":"fileSearchStores/store1"})";
        } else if (request.url.find(":uploadToFileSearchStore") != std::string::npos) {
            response.headers.push_back(
                {"X-Goog-Upload-URL", "https://generativelanguage.googleapis.com/upload/session"});
        } else if (request.url.find("/upload/session") != std::string::npos) {
            response.body =
                R"({"name":"fileSearchStores/store1/upload/operations/op1"})";
        } else if (request.url.find("/upload/operations/op1") != std::string::npos) {
            response.body = R"({"name":"fileSearchStores/store1/upload/operations/op1","done":true})";
        } else if (request.url.find("/documents?") != std::string::npos) {
            response.body = "{}";
        } else if (request.method == "GET") {
            ++inspect;
            response.body = inspect < 2
                ? R"({"activeDocumentsCount":"0","pendingDocumentsCount":"1","failedDocumentsCount":"0"})"
                : R"({"activeDocumentsCount":"1","pendingDocumentsCount":"0","failedDocumentsCount":"0"})";
        } else if (request.method == "DELETE") {
            EXPECT_NE(request.url.find("force=true"), std::string::npos);
            response.body = R"({})";
        }
        return response;
    });
    std::atomic_bool cancel{false};
    const auto created = client.google_create_store("Research", "key", cancel);
    EXPECT_EQ(created.id, "fileSearchStores/store1");
    const auto uploaded = client.google_upload_document(
        created.id, "note.md", "# Note", "key", cancel, cancel);
    EXPECT_EQ(uploaded.id, "fileSearchStores/store1/upload/operations/op1");
    EXPECT_TRUE(client.google_inspect_operation(uploaded.id, "key", cancel).done);
    auto short_count = client.google_inspect_store(created.id, "key", cancel);
    EXPECT_EQ(short_count.active, 0);
    EXPECT_EQ(short_count.pending, 1);
    auto ready = client.google_inspect_store(created.id, "key", cancel);
    EXPECT_EQ(ready.active, 1);
    EXPECT_EQ(ready.pending, 0);
    EXPECT_EQ(client.google_delete_store(created.id, "key", cancel).kind, FolderStoreRemoteKind::ok);
}

TEST(FolderStoreHttp, GoogleDeletesAllDocumentPagesBeforeDeletingStore) {
    std::vector<std::string> requests;
    FolderStoreClient client([&](const FolderStoreHttpRequest& request, const std::atomic_bool&) {
        requests.push_back(request.method + " " + request.url);
        EXPECT_EQ(request.headers.size(), 1u);
        EXPECT_EQ(request.headers.front().name, "x-goog-api-key");
        EXPECT_EQ(request.headers.front().value, "key");
        if (request.method == "GET") {
            return FolderStoreHttpResponse{.status = 200, .body = requests.size() == 1
                ? R"({"documents":[{"name":"fileSearchStores/store1/documents/one"}],"nextPageToken":"next+/=&"})"
                : R"({"documents":[{"name":"fileSearchStores/store1/documents/two"}]})"};
        }
        return FolderStoreHttpResponse{.status = 200, .body = "{}"};
    });
    std::atomic_bool cancel{false};
    EXPECT_EQ(client.google_delete_store("fileSearchStores/store1", "key", cancel).kind, FolderStoreRemoteKind::ok);
    EXPECT_EQ(requests, (std::vector<std::string>{
        "GET https://generativelanguage.googleapis.com/v1beta/fileSearchStores/store1/documents?pageSize=20",
        "GET https://generativelanguage.googleapis.com/v1beta/fileSearchStores/store1/documents?pageSize=20&pageToken=next%2B%2F%3D%26",
        "DELETE https://generativelanguage.googleapis.com/v1beta/fileSearchStores/store1/documents/one?force=true",
        "DELETE https://generativelanguage.googleapis.com/v1beta/fileSearchStores/store1/documents/two?force=true",
        "DELETE https://generativelanguage.googleapis.com/v1beta/fileSearchStores/store1?force=true"}));
}

std::vector<std::string> store_rows(const FolderStoreList& list) {
    std::vector<std::string> rows;
    for (const auto& store : list.stores) rows.push_back(store.id + "=" + store.name);
    return rows;
}

TEST(FolderStoreHttp, ListsStoresFromEveryPageForBothServices) {
    std::vector<std::string> requests;
    FolderStoreClient client([&](const FolderStoreHttpRequest& request, const std::atomic_bool&) {
        requests.push_back(request.method + " " + request.url + " " + request.headers.front().name + "=" + request.headers.front().value);
        if (request.url.find("/documents?") != std::string::npos)
            return FolderStoreHttpResponse{.status = 200, .body = "{}"};
        if (request.url.find("/vector_stores") != std::string::npos) {
            return FolderStoreHttpResponse{.status = 200, .body = request.url.ends_with("limit=100")
                ? R"({"data":[{"id":"vs_1","name":"One"},{"id":"vs_2","name":null}],"last_id":"vs_2","has_more":true})"
                : R"({"data":[{"id":"vs_3","name":"Two"}],"last_id":"vs_3","has_more":false})"};
        }
        return FolderStoreHttpResponse{.status = 200, .body = request.url.ends_with("pageSize=20")
            ? R"({"fileSearchStores":[{"name":"fileSearchStores/a","displayName":"Alpha"},{"name":"fileSearchStores/b"}],"nextPageToken":"next+/="})"
            : R"({"fileSearchStores":[{"name":"fileSearchStores/c","displayName":"Gamma"}]})"};
    });
    std::atomic_bool cancel{false};
    auto found = client.list_stores(FolderStoreService::openai, "sk-key", cancel);
    EXPECT_EQ(found.result.kind, FolderStoreRemoteKind::ok);
    EXPECT_EQ(store_rows(found), (std::vector<std::string>{"vs_1=One", "vs_2=", "vs_3=Two"}));
    found = client.list_stores(FolderStoreService::google, "g-key", cancel);
    EXPECT_EQ(found.result.kind, FolderStoreRemoteKind::ok);
    EXPECT_EQ(store_rows(found), (std::vector<std::string>{
        "fileSearchStores/a=Alpha", "fileSearchStores/b=", "fileSearchStores/c=Gamma"}));
    EXPECT_EQ(requests, (std::vector<std::string>{
        "GET https://api.openai.com/v1/vector_stores?limit=100 Authorization=Bearer sk-key",
        "GET https://api.openai.com/v1/vector_stores?limit=100&after=vs_2 Authorization=Bearer sk-key",
        "GET https://generativelanguage.googleapis.com/v1beta/fileSearchStores?pageSize=20 x-goog-api-key=g-key",
        "GET https://generativelanguage.googleapis.com/v1beta/fileSearchStores?pageSize=20&pageToken=next%2B%2F%3D x-goog-api-key=g-key",
        "GET https://generativelanguage.googleapis.com/v1beta/fileSearchStores/a/documents?pageSize=20 x-goog-api-key=g-key",
        "GET https://generativelanguage.googleapis.com/v1beta/fileSearchStores/b/documents?pageSize=20 x-goog-api-key=g-key",
        "GET https://generativelanguage.googleapis.com/v1beta/fileSearchStores/c/documents?pageSize=20 x-goog-api-key=g-key"}));

    requests.clear();
    found = client.list_stores(FolderStoreService::google, "g-key", cancel, /*resolve_metadata=*/false);
    ASSERT_EQ(found.result.kind, FolderStoreRemoteKind::ok);
    EXPECT_EQ(store_rows(found), (std::vector<std::string>{
        "fileSearchStores/a=Alpha", "fileSearchStores/b=", "fileSearchStores/c=Gamma"}));
    EXPECT_EQ(requests, (std::vector<std::string>{
        "GET https://generativelanguage.googleapis.com/v1beta/fileSearchStores?pageSize=20 x-goog-api-key=g-key",
        "GET https://generativelanguage.googleapis.com/v1beta/fileSearchStores?pageSize=20&pageToken=next%2B%2F%3D x-goog-api-key=g-key"}));

    FolderStoreClient empty([](const FolderStoreHttpRequest&, const std::atomic_bool&) {
        return FolderStoreHttpResponse{.status = 200, .body = "{}"};
    });
    found = empty.list_stores(FolderStoreService::google, "g-key", cancel);
    EXPECT_EQ(found.result.kind, FolderStoreRemoteKind::ok);
    EXPECT_TRUE(found.stores.empty());
}

TEST(FolderStoreHttp, StoreNameListingRejectsInvalidPages) {
    const std::vector<std::pair<FolderStoreService, std::string>> bodies{
        {FolderStoreService::openai, R"({})"},
        {FolderStoreService::openai, R"({"data":{}})"},
        {FolderStoreService::openai, R"({"data":[1]})"},
        {FolderStoreService::openai, R"({"data":[{"id":"vs_1","name":1}]})"},
        {FolderStoreService::openai, R"({"data":[{"name":"No ID"}]})"},
        {FolderStoreService::openai, R"({"data":[{"id":"vs/../files"}]})"},
        {FolderStoreService::google, R"({"fileSearchStores":[{"name":"other/a"}]})"},
        {FolderStoreService::openai, R"({"data":[],"has_more":"yes"})"},
        {FolderStoreService::openai, R"({"data":[],"has_more":true})"},
        {FolderStoreService::openai, R"({"data":[],"has_more":true,"last_id":"vs_1"})"},
        {FolderStoreService::google, R"({"fileSearchStores":[{"name":"fileSearchStores/a","displayName":2}]})"},
        {FolderStoreService::google, R"({"nextPageToken":12})"},
        {FolderStoreService::google, R"({"nextPageToken":"repeated"})"}};
    std::atomic_bool cancel{false};
    for (const auto& [service, body] : bodies) {
        SCOPED_TRACE(body);
        int requests = 0;
        FolderStoreClient client([&](const FolderStoreHttpRequest& request, const std::atomic_bool&) {
            ++requests;
            EXPECT_EQ(request.method, "GET");
            EXPECT_LE(requests, 2);
            return FolderStoreHttpResponse{.status = 200, .body = body};
        });
        const auto found = client.list_stores(service, "key", cancel);
        EXPECT_EQ(found.result.kind, FolderStoreRemoteKind::failed);
        EXPECT_TRUE(found.stores.empty());
    }
}

TEST(FolderStoreHttp, ReadsOpenAiStoreDetailsWithUtcCreationTime) {
    std::vector<std::string> requests;
    std::string body = R"({"id":"vs_1","name":"Research","description":"Line one\nLine two","created_at":1793697300})";
    FolderStoreClient client([&](const FolderStoreHttpRequest& request, const std::atomic_bool&) {
        requests.push_back(request.method + " " + request.url + " " + request.headers.front().value);
        return FolderStoreHttpResponse{.status = 200, .body = body};
    });
    std::atomic_bool cancel{false};
    auto detail = client.get_store(FolderStoreService::openai, "vs_1", "sk-key", cancel);
    ASSERT_EQ(detail.result.kind, FolderStoreRemoteKind::ok);
    EXPECT_EQ(detail.id, "vs_1");
    EXPECT_EQ(detail.name, "Research");
    EXPECT_EQ(detail.description, "Line one\nLine two");
    EXPECT_EQ(detail.created_at, "2026-11-03T09:15:00Z");
    EXPECT_EQ(requests, std::vector<std::string>{"GET https://api.openai.com/v1/vector_stores/vs_1 Bearer sk-key"});

    body = R"({"id":"vs_1","name":null,"description":null,"created_at":0})";
    detail = client.get_store(FolderStoreService::openai, "vs_1", "sk-key", cancel);
    ASSERT_EQ(detail.result.kind, FolderStoreRemoteKind::ok);
    EXPECT_EQ(detail.name, "");
    EXPECT_EQ(detail.description, "");
    // An edited description in metadata replaces the native one, also when it is empty.
    body = R"({"id":"vs_1","description":"Native","metadata":{"description":"Edited"},"created_at":0})";
    EXPECT_EQ(client.get_store(FolderStoreService::openai, "vs_1", "sk-key", cancel).description, "Edited");
    body = R"({"id":"vs_1","description":"Native","metadata":{"description":""},"created_at":0})";
    EXPECT_EQ(client.get_store(FolderStoreService::openai, "vs_1", "sk-key", cancel).description, "");
    body = R"({"id":"vs_1","description":"Native","metadata":{"owner":"other"},"created_at":0})";
    EXPECT_EQ(client.get_store(FolderStoreService::openai, "vs_1", "sk-key", cancel).description, "Native");
    for (const auto& invalid : {R"({"id":"vs_2","created_at":1})", R"({"id":"vs_1","created_at":"today"})",
             R"({"id":"vs_1","name":5,"created_at":1})", R"({"id":"vs_1"})"}) {
        SCOPED_TRACE(invalid);
        body = invalid;
        EXPECT_EQ(client.get_store(FolderStoreService::openai, "vs_1", "sk-key", cancel).result.kind,
            FolderStoreRemoteKind::failed);
    }
    requests.clear();
    EXPECT_EQ(client.get_store(FolderStoreService::openai, "../files", "sk-key", cancel).result.kind,
        FolderStoreRemoteKind::failed);
    EXPECT_EQ(client.get_store(FolderStoreService::google, "vs_1", "g-key", cancel).result.kind,
        FolderStoreRemoteKind::failed);
    EXPECT_TRUE(requests.empty());
}

TEST(FolderStoreHttp, UpdatesOpenAiStoreAndVerifiesPersistedFields) {
    nlohmann::json saved{{"id", "vs_1"}, {"name", "Before"}, {"description", "Old"},
        {"metadata", {{"owner", "other"}}}, {"created_at", 1793697300}};
    std::vector<std::string> requests;
    bool ignore_metadata = false, fail_read = false;
    FolderStoreClient client([&](const auto& request, const auto&) {
        requests.push_back(request.method);
        EXPECT_EQ(request.url, "https://api.openai.com/v1/vector_stores/vs_1");
        EXPECT_EQ(request.headers.front().value, "Bearer sk-key");
        if (fail_read) return FolderStoreHttpResponse{.status = 503, .body = R"({"error":{"code":"server_error"}})"};
        if (request.method == "POST") {
            // The modify API has no description field. OpenAI replaces the whole metadata map.
            const auto body = nlohmann::json::parse(request.body);
            EXPECT_EQ(body.size(), 2);
            EXPECT_FALSE(body.contains("description"));
            saved["name"] = body["name"];
            if (!ignore_metadata) saved["metadata"] = body["metadata"];
        }
        return FolderStoreHttpResponse{.status = 200, .body = saved.dump()};
    });
    std::atomic_bool cancel{false};
    auto detail = client.update_store(FolderStoreService::openai, "vs_1", "Renamed", "New\nNotes", "sk-key", cancel);
    EXPECT_EQ(detail.result.kind, FolderStoreRemoteKind::ok);
    EXPECT_EQ(detail.name, "Renamed");
    EXPECT_EQ(detail.description, "New\nNotes");
    EXPECT_EQ(detail.created_at, "2026-11-03T09:15:00Z");
    EXPECT_EQ(requests, (std::vector<std::string>{"GET", "POST", "GET"}));
    EXPECT_EQ(saved["description"], "Old");
    EXPECT_EQ(saved["metadata"], (nlohmann::json{{"owner", "other"}, {"description", "New\nNotes"}}));
    detail = client.update_store(FolderStoreService::openai, "vs_1", "Renamed", "", "sk-key", cancel);
    EXPECT_EQ(detail.result.kind, FolderStoreRemoteKind::ok);
    EXPECT_EQ(detail.description, "");
    ignore_metadata = true;
    detail = client.update_store(FolderStoreService::openai, "vs_1", "Renamed", "Other", "sk-key", cancel);
    EXPECT_EQ(detail.result.kind, FolderStoreRemoteKind::failed);
    EXPECT_NE(detail.result.message.find("did not apply all changes"), std::string::npos);
    requests.clear();
    fail_read = true;
    EXPECT_EQ(client.update_store(FolderStoreService::openai, "vs_1", "x", "", "sk-key", cancel).result.kind,
        FolderStoreRemoteKind::failed);
    EXPECT_EQ(requests, std::vector<std::string>{"GET"});
    requests.clear();
    EXPECT_EQ(client.update_store(FolderStoreService::openai, "../files", "x", "", "sk-key", cancel).result.kind,
        FolderStoreRemoteKind::failed);
    cancel = true;
    EXPECT_EQ(client.update_store(FolderStoreService::openai, "vs_1", "x", "", "sk-key", cancel).result.kind,
        FolderStoreRemoteKind::cancelled);
    EXPECT_TRUE(requests.empty());
}

TEST(FolderStoreHttp, ReplacesGeminiMetadataAfterIndexingAndUsesItInListsAndDetails) {
    for (const std::string description : {"New\nNotes", ""}) {
        SCOPED_TRACE(description);
        auto metadata = nlohmann::json::parse(R"({"name":"fileSearchStores/a/documents/old","displayName":"DESCRIPTION.txt",
            "customMetadata":[{"key":"name","stringListValue":{"values":["Before"]}}]})");
        nlohmann::json replacement;
        auto extra_metadata = metadata;
        extra_metadata["name"] = "fileSearchStores/a/documents/old2";
        bool extra_exists = description.empty(); // A retry also cleans a previous replacement left behind.
        bool deleted = false, indexed = false;
        int polls = 0;
        FolderStoreClient client([&](const auto& request, const auto&) {
            EXPECT_EQ(request.headers.front().value, "g-key");
            if (request.url.ends_with(":uploadToFileSearchStore")) {
                const auto body = nlohmann::json::parse(request.body);
                replacement = {{"name", "fileSearchStores/a/documents/new"},
                    {"displayName", "DESCRIPTION.txt"}, {"customMetadata", body["customMetadata"]}};
                return FolderStoreHttpResponse{.status = 200,
                    .headers = {{"X-Goog-Upload-URL", "https://generativelanguage.googleapis.com/upload/session"}}};
            }
            if (request.url.ends_with("/upload/session"))
                return FolderStoreHttpResponse{.status = 200, .body = R"({"name":"fileSearchStores/a/upload/operations/op1"})"};
            if (request.url.ends_with("/upload/operations/op1")) {
                indexed = ++polls == 2;
                return FolderStoreHttpResponse{.status = 200, .body = nlohmann::json{
                    {"name", "fileSearchStores/a/upload/operations/op1"}, {"done", indexed},
                    {"response", {{"documentName", "fileSearchStores/a/documents/new"}}}}.dump()};
            }
            if (request.method == "GET" && request.url.ends_with("/documents/new"))
                return FolderStoreHttpResponse{.status = 200, .body = replacement.dump()};
            if (request.method == "DELETE") {
                EXPECT_TRUE(indexed);
                EXPECT_TRUE(request.url.ends_with("/documents/old?force=true") || request.url.ends_with("/documents/old2?force=true"));
                deleted = true;
                if (request.url.ends_with("/documents/old2?force=true")) extra_exists = false;
                else metadata = replacement;
                return FolderStoreHttpResponse{.status = 200, .body = "{}"};
            }
            if (request.url.find("/documents?") != std::string::npos) {
                auto documents = nlohmann::json::array({metadata,
                    {{"name", "fileSearchStores/a/documents/note"}, {"displayName", "note.md"}}});
                if (extra_exists) documents.push_back(extra_metadata);
                return FolderStoreHttpResponse{.status = 200, .body = nlohmann::json{
                    {"documents", documents}}.dump()};
            }
            const auto store = nlohmann::json{{"name", "fileSearchStores/a"}, {"displayName", "Before"},
                {"createTime", "2026-11-03T09:15:00Z"}};
            if (request.url.ends_with("/fileSearchStores?pageSize=20"))
                return FolderStoreHttpResponse{.status = 200, .body = nlohmann::json{
                    {"fileSearchStores", nlohmann::json::array({store})}}.dump()};
            return FolderStoreHttpResponse{.status = 200, .body = store.dump()};
        });
        std::atomic_bool cancel{false};
        const auto detail = client.update_store(FolderStoreService::google, "fileSearchStores/a", "Renamed", description, "g-key", cancel);
        ASSERT_EQ(detail.result.kind, FolderStoreRemoteKind::ok) << detail.result.message;
        EXPECT_TRUE(deleted);
        EXPECT_FALSE(extra_exists);
        EXPECT_EQ(detail.name, "Renamed");
        EXPECT_EQ(detail.description, description);
        EXPECT_EQ(detail.created_at, "2026-11-03T09:15:00Z");
        const auto listed = client.list_stores(FolderStoreService::google, "g-key", cancel);
        ASSERT_EQ(listed.result.kind, FolderStoreRemoteKind::ok);
        EXPECT_EQ(store_rows(listed), (std::vector<std::string>{"fileSearchStores/a=Renamed"}));
    }
}

TEST(FolderStoreHttp, FailedGeminiMetadataUploadDoesNotDeleteThePreviousDocument) {
    bool deleted = false;
    FolderStoreClient client([&](const auto& request, const auto&) {
        if (request.method == "DELETE") deleted = true;
        if (request.method == "GET") return FolderStoreHttpResponse{.status = 200, .body =
            R"({"documents":[{"name":"fileSearchStores/a/documents/old","displayName":"DESCRIPTION.txt",
                "customMetadata":[{"key":"name","stringListValue":{"values":["Before"]}}]}]})"};
        return FolderStoreHttpResponse{.status = 503, .body = R"({"error":{"status":"UNAVAILABLE"}})"};
    });
    std::atomic_bool cancel{false};
    const auto detail = client.update_store(FolderStoreService::google, "fileSearchStores/a", "Renamed", "New", "g-key", cancel);
    EXPECT_EQ(detail.result.kind, FolderStoreRemoteKind::failed);
    EXPECT_FALSE(deleted);
}

TEST(FolderStoreHttp, GeminiReplacementFailuresKeepVerifiedMetadata) {
    for (const std::string_view mode : {"wrong_values", "delete_rejected", "delete_response_lost"}) {
        SCOPED_TRACE(mode);
        const auto original = nlohmann::json::parse(R"({"name":"fileSearchStores/a/documents/old","displayName":"DESCRIPTION.txt",
            "customMetadata":[{"key":"name","stringListValue":{"values":["Before"]}},
            {"key":"description","stringListValue":{"values":["Original notes"]}}]})");
        auto replacement = original;
        replacement["name"] = "fileSearchStores/a/documents/new";
        replacement["customMetadata"][0]["stringListValue"]["values"] = {mode == "wrong_values" ? "Wrong" : "Renamed"};
        bool replacement_exists = false, original_exists = true, attempted_old_delete = false;
        FolderStoreClient client([&](const auto& request, const auto&) {
            if (request.url.ends_with(":uploadToFileSearchStore"))
                return FolderStoreHttpResponse{.status = 200,
                    .headers = {{"X-Goog-Upload-URL", "https://generativelanguage.googleapis.com/upload/session"}}};
            if (request.url.ends_with("/upload/session")) {
                replacement_exists = true;
                return FolderStoreHttpResponse{.status = 200, .body = R"({"name":"fileSearchStores/a/upload/operations/op1",
                    "done":true,"response":{"documentName":"fileSearchStores/a/documents/new"}})"};
            }
            if (request.method == "DELETE") {
                if (request.url.ends_with("/documents/new?force=true")) {
                    replacement_exists = false;
                    return FolderStoreHttpResponse{.status = 200, .body = "{}"};
                }
                EXPECT_TRUE(request.url.ends_with("/documents/old?force=true"));
                attempted_old_delete = true;
                if (mode == "delete_response_lost") original_exists = false;
                return FolderStoreHttpResponse{.status = 503, .body = R"({"error":{"status":"UNAVAILABLE"}})"};
            }
            if (request.url.ends_with("/documents/new"))
                return FolderStoreHttpResponse{.status = 200, .body = replacement.dump()};
            if (request.url.ends_with("/documents/old")) {
                if (!original_exists) return FolderStoreHttpResponse{.status = 404,
                    .body = R"({"error":{"code":404,"status":"NOT_FOUND"}})"};
                return FolderStoreHttpResponse{.status = 200, .body = original.dump()};
            }
            if (request.url.find("/documents?") != std::string::npos) {
                auto documents = nlohmann::json::array();
                if (original_exists) documents.push_back(original);
                if (replacement_exists) documents.push_back(replacement);
                return FolderStoreHttpResponse{.status = 200, .body = nlohmann::json{{"documents", documents}}.dump()};
            }
            return FolderStoreHttpResponse{.status = 200, .body = R"({"name":"fileSearchStores/a",
                "displayName":"Before","createTime":"2026-11-03T09:15:00Z"})"};
        });
        std::atomic_bool cancel{false};
        const auto result = client.update_store(FolderStoreService::google, "fileSearchStores/a", "Renamed", "Original notes", "key", cancel);
        EXPECT_EQ(result.result.kind, mode == "delete_response_lost" ? FolderStoreRemoteKind::ok : FolderStoreRemoteKind::failed);
        EXPECT_EQ(attempted_old_delete, mode != "wrong_values");
        EXPECT_EQ(replacement_exists, mode == "delete_response_lost");
        const auto saved = client.get_store(FolderStoreService::google, "fileSearchStores/a", "key", cancel);
        ASSERT_EQ(saved.result.kind, FolderStoreRemoteKind::ok);
        EXPECT_EQ(saved.name, mode == "delete_response_lost" ? "Renamed" : "Before");
        EXPECT_EQ(saved.description, "Original notes");
    }
}

TEST(FolderStoreHttp, ReadsGeminiStoreDescriptionFromItsOnlyMetadataDocument) {
    const std::string metadata = R"({"name":"fileSearchStores/a/documents/meta","displayName":"DESCRIPTION.txt","customMetadata":[)"
        R"({"key":"name","stringListValue":{"values":["Research"]}},)"
        R"({"key":"description","stringListValue":{"values":["Part one ","part two"]}}]})";
    std::vector<std::string> documents_pages;
    std::vector<std::string> requests;
    FolderStoreClient client([&](const FolderStoreHttpRequest& request, const std::atomic_bool&) {
        requests.push_back(request.url);
        EXPECT_EQ(request.headers.front().name, "x-goog-api-key");
        if (request.url.ends_with("/v1beta/fileSearchStores/a"))
            return FolderStoreHttpResponse{.status = 200,
                .body = R"({"name":"fileSearchStores/a","displayName":"Research","createTime":"2026-11-03T09:15:00.123Z"})"};
        const auto page = request.url.find("pageToken=") == std::string::npos ? 0 : 1;
        return FolderStoreHttpResponse{.status = 200, .body = documents_pages.at(page)};
    });
    std::atomic_bool cancel{false};
    documents_pages = {
        R"({"documents":[{"name":"fileSearchStores/a/documents/one","displayName":"note.md"}],"nextPageToken":"t"})",
        R"({"documents":[)" + metadata + "]}"};
    auto detail = client.get_store(FolderStoreService::google, "fileSearchStores/a", "g-key", cancel);
    ASSERT_EQ(detail.result.kind, FolderStoreRemoteKind::ok);
    EXPECT_EQ(detail.name, "Research");
    EXPECT_EQ(detail.created_at, "2026-11-03T09:15:00.123Z");
    EXPECT_EQ(detail.description, "Part one part two");
    EXPECT_EQ(requests, (std::vector<std::string>{
        "https://generativelanguage.googleapis.com/v1beta/fileSearchStores/a",
        "https://generativelanguage.googleapis.com/v1beta/fileSearchStores/a/documents?pageSize=20",
        "https://generativelanguage.googleapis.com/v1beta/fileSearchStores/a/documents?pageSize=20&pageToken=t"}));

    // Two candidates, no name entry, or a malformed description give an empty description.
    for (const auto& page : {R"({"documents":[)" + metadata + "," + metadata + "]}",
             std::string(R"({"documents":[{"displayName":"DESCRIPTION.txt","customMetadata":[{"key":"description","stringListValue":{"values":["x"]}}]}]})"),
             std::string(R"({"documents":[{"displayName":"DESCRIPTION.txt","customMetadata":[{"key":"name","stringListValue":{"values":["n"]}},{"key":"description","stringValue":"x"}]}]})")}) {
        SCOPED_TRACE(page);
        documents_pages = {page};
        detail = client.get_store(FolderStoreService::google, "fileSearchStores/a", "g-key", cancel);
        ASSERT_EQ(detail.result.kind, FolderStoreRemoteKind::ok);
        EXPECT_EQ(detail.description, "");
    }
    documents_pages = {R"({"documents":{}})"};
    EXPECT_EQ(client.get_store(FolderStoreService::google, "fileSearchStores/a", "g-key", cancel).result.kind,
        FolderStoreRemoteKind::failed);
}

TEST(FolderStoreHttp, OpenAiStoreDeletionListsAllFilesFirstAndAcceptsAbsentResources) {
    std::vector<std::string> requests;
    FolderStoreClient client([&](const FolderStoreHttpRequest& request, const std::atomic_bool&) {
        requests.push_back(request.method + " " + request.url);
        if (request.method == "GET") {
            return FolderStoreHttpResponse{.status = 200, .body = request.url.ends_with("limit=100")
                ? R"({"data":[{"id":"file_1"},{"id":"file_2"}],"last_id":"file_2","has_more":true})"
                : R"({"data":[{"id":"file_3"}],"last_id":"file_3","has_more":false})"};
        }
        if (request.url.ends_with("/files/file_2"))
            return FolderStoreHttpResponse{.status = 404, .body = R"({"error":{"type":"invalid_request_error","message":"No such file"}})"};
        if (request.url.ends_with("/vector_stores/vs_1"))
            return FolderStoreHttpResponse{.status = 404, .body = R"({"error":{"type":"invalid_request_error","message":"No such store"}})"};
        return FolderStoreHttpResponse{.status = 200, .body = R"({"deleted":true})"};
    });
    std::atomic_bool cancel{false};
    EXPECT_EQ(client.delete_store(FolderStoreService::openai, "vs_1", "sk-key", cancel).kind, FolderStoreRemoteKind::ok);
    EXPECT_EQ(requests, (std::vector<std::string>{
        "GET https://api.openai.com/v1/vector_stores/vs_1/files?limit=100",
        "GET https://api.openai.com/v1/vector_stores/vs_1/files?limit=100&after=file_2",
        "DELETE https://api.openai.com/v1/files/file_1",
        "DELETE https://api.openai.com/v1/files/file_2",
        "DELETE https://api.openai.com/v1/files/file_3",
        "DELETE https://api.openai.com/v1/vector_stores/vs_1"}));
}

TEST(FolderStoreHttp, OpenAiStoreDeletionStopsBeforeChangesWhenFilesCannotBeConfirmed) {
    const auto not_found = FolderStoreHttpResponse{.status = 404,
        .body = R"({"error":{"type":"invalid_request_error","message":"No such vector store"}})"};
    std::atomic_bool cancel{false};
    int deletes = 0, lists = 0;
    std::function<FolderStoreHttpResponse(int)> list_page;
    FolderStoreClient client([&](const FolderStoreHttpRequest& request, const std::atomic_bool&) {
        if (request.method == "DELETE") {
            ++deletes;
            return request.url.find("/files/") != std::string::npos
                ? FolderStoreHttpResponse{.status = 500, .body = "{}"}
                : FolderStoreHttpResponse{.status = 200, .body = R"({"deleted":true})"};
        }
        return list_page(lists++);
    });
    list_page = [&](int) { return not_found; };
    auto result = client.delete_store(FolderStoreService::openai, "vs_1", "sk-key", cancel);
    EXPECT_EQ(result.kind, FolderStoreRemoteKind::not_found);
    EXPECT_EQ(result.message, "Store unavailable. File cleanup could not be verified.");
    EXPECT_EQ(deletes, 0);

    lists = 0;
    list_page = [&](int page) {
        return page == 0 ? FolderStoreHttpResponse{.status = 200, .body = R"({"data":[{"id":"file_1"}],"last_id":"file_1","has_more":true})"}
                         : not_found;
    };
    EXPECT_EQ(client.delete_store(FolderStoreService::openai, "vs_1", "sk-key", cancel).kind, FolderStoreRemoteKind::failed);
    EXPECT_EQ(deletes, 0);

    list_page = [&](int) { return FolderStoreHttpResponse{.status = 200, .body = "{}"}; };
    EXPECT_EQ(client.delete_store(FolderStoreService::openai, "vs_1", "sk-key", cancel).kind, FolderStoreRemoteKind::failed);
    EXPECT_EQ(deletes, 0) << "A file listing without data deletes nothing";

    lists = 0;
    list_page = [&](int) { return FolderStoreHttpResponse{.status = 200, .body = R"({"data":[{"id":"file_1"},{"id":"file_2"}],"has_more":false})"}; };
    EXPECT_EQ(client.delete_store(FolderStoreService::openai, "vs_1", "sk-key", cancel).kind, FolderStoreRemoteKind::failed);
    EXPECT_EQ(deletes, 1) << "A failed file deletion stops before the next file and the store";

    deletes = 0;
    list_page = [&](int) { return FolderStoreHttpResponse{.status = 401, .body = R"({"error":{"code":"invalid_api_key"}})"}; };
    result = client.delete_store(FolderStoreService::openai, "vs_1", "sk-key", cancel);
    EXPECT_EQ(result.kind, FolderStoreRemoteKind::failed);
    EXPECT_EQ(result.message, "The saved API key was rejected.");
    EXPECT_EQ(deletes, 0);
}

TEST(FolderStoreHttp, GeminiSettingsDeletionForcesDocumentsButNotTheStore) {
    std::vector<std::string> requests;
    FolderStoreHttpResponse store_reply{.status = 200, .body = "{}"};
    FolderStoreClient client([&](const FolderStoreHttpRequest& request, const std::atomic_bool&) {
        requests.push_back(request.method + " " + request.url);
        if (request.method == "GET")
            return FolderStoreHttpResponse{.status = 200, .body = R"({"documents":[{"name":"fileSearchStores/a/documents/one"}]})"};
        if (request.url.find("/documents/") != std::string::npos) return FolderStoreHttpResponse{.status = 200, .body = "{}"};
        return store_reply;
    });
    std::atomic_bool cancel{false};
    EXPECT_EQ(client.delete_store(FolderStoreService::google, "fileSearchStores/a", "g-key", cancel).kind, FolderStoreRemoteKind::ok);
    EXPECT_EQ(requests, (std::vector<std::string>{
        "GET https://generativelanguage.googleapis.com/v1beta/fileSearchStores/a/documents?pageSize=20",
        "DELETE https://generativelanguage.googleapis.com/v1beta/fileSearchStores/a/documents/one?force=true",
        "DELETE https://generativelanguage.googleapis.com/v1beta/fileSearchStores/a?force=false"}));

    store_reply = {.status = 400, .body = R"({"error":{"code":400,"status":"FAILED_PRECONDITION","message":"private"}})"};
    auto result = client.delete_store(FolderStoreService::google, "fileSearchStores/a", "g-key", cancel);
    EXPECT_EQ(result.kind, FolderStoreRemoteKind::failed);
    EXPECT_EQ(result.message, "The store still contains documents. Retry deletion.");

    store_reply = {.status = 404, .body = R"({"error":{"code":404,"status":"NOT_FOUND","message":"gone"}})"};
    EXPECT_EQ(client.delete_store(FolderStoreService::google, "fileSearchStores/a", "g-key", cancel).kind, FolderStoreRemoteKind::ok);

    // Upload cleanup still forces the store delete.
    requests.clear();
    store_reply = {.status = 200, .body = "{}"};
    EXPECT_EQ(client.google_delete_store("fileSearchStores/a", "g-key", cancel).kind, FolderStoreRemoteKind::ok);
    EXPECT_EQ(requests.back(), "DELETE https://generativelanguage.googleapis.com/v1beta/fileSearchStores/a?force=true");
}

TEST(FolderStoreHttp, GoogleCleanupRejectsUnsafeOrIncompleteListingsBeforeDeleting) {
    const std::vector<std::string> bodies{
        R"({"documents":{}})", R"({"documents":[{}]})", R"({"documents":[{"name":1}]})",
        R"({"documents":[{"name":"fileSearchStores/other/documents/one"}]})",
        R"({"documents":[{"name":"fileSearchStores/store1/documents/../other"}]})",
        R"({"documents":[{"name":"fileSearchStores/store1/documents/one?key=private"}]})",
        R"({"nextPageToken":12})", R"({"nextPageToken":"repeated"})",
        R"({"error":{"code":13,"message":"private"}})"};
    std::atomic_bool cancel{false};
    for (const auto& body : bodies) {
        SCOPED_TRACE(body);
        int requests = 0;
        FolderStoreClient client([&](const FolderStoreHttpRequest& request, const std::atomic_bool&) {
            ++requests;
            EXPECT_EQ(request.method, "GET");
            EXPECT_LE(requests, 2);
            return FolderStoreHttpResponse{.status = 200, .body = body};
        });
        const auto result = client.google_delete_store("fileSearchStores/store1", "key", cancel);
        EXPECT_EQ(result.kind, FolderStoreRemoteKind::failed);
        EXPECT_EQ(result.message.find("private"), std::string::npos);
    }
    int requests = 0;
    FolderStoreClient client([&](const FolderStoreHttpRequest& request, const std::atomic_bool&) {
        EXPECT_EQ(request.method, "GET");
        if (++requests == 1) return FolderStoreHttpResponse{.status = 200,
            .body = R"({"documents":[{"name":"fileSearchStores/store1/documents/one"}],"nextPageToken":"next"})"};
        return FolderStoreHttpResponse{.status = 503, .body = R"({"error":{"status":"UNAVAILABLE"}})"};
    });
    EXPECT_EQ(client.google_delete_store("fileSearchStores/store1", "key", cancel).kind, FolderStoreRemoteKind::failed);
    EXPECT_EQ(requests, 2);
}

TEST(FolderStoreHttp, GoogleCleanupHandlesAlreadyDeletedDocumentsAndMissingParent) {
    std::vector<std::string> requests;
    FolderStoreClient client([&](const FolderStoreHttpRequest& request, const std::atomic_bool&) {
        requests.push_back(request.method + " " + request.url);
        if (request.method == "GET") return FolderStoreHttpResponse{.status = 200,
            .body = R"({"documents":[{"name":"fileSearchStores/store1/documents/orphan"}]})"};
        return FolderStoreHttpResponse{.status = 404,
            .body = R"({"error":{"code":404,"status":"NOT_FOUND"}})"};
    });
    std::atomic_bool cancel{false};
    EXPECT_EQ(client.google_delete_store("fileSearchStores/store1", "key", cancel).kind, FolderStoreRemoteKind::not_found);
    ASSERT_EQ(requests.size(), 3u);
    EXPECT_NE(requests[1].find("/documents/orphan?force=true"), std::string::npos);
    EXPECT_TRUE(requests[2].ends_with("/store1?force=true"));
}

TEST(FolderStoreHttp, GoogleCleanupHandlesMissingDocumentListing) {
    const FolderStoreHttpResponse missing{.status = 404, .body = R"({"error":{"code":404,"status":"NOT_FOUND"}})"};
    std::atomic_bool cancel{false};
    std::vector<std::string> requests;
    FolderStoreClient first_page([&](const FolderStoreHttpRequest& request, const std::atomic_bool&) {
        requests.push_back(request.method + " " + request.url);
        return request.method == "GET" ? missing : FolderStoreHttpResponse{.status = 200, .body = "{}"};
    });
    EXPECT_EQ(first_page.google_delete_store("fileSearchStores/store1", "key", cancel).kind, FolderStoreRemoteKind::ok);
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests[1], "DELETE https://generativelanguage.googleapis.com/v1beta/fileSearchStores/store1?force=true");

    requests.clear();
    FolderStoreClient later_page([&](const FolderStoreHttpRequest& request, const std::atomic_bool&) {
        requests.push_back(request.method + " " + request.url);
        if (requests.size() > 1) return missing;
        return FolderStoreHttpResponse{.status = 200,
            .body = R"({"documents":[{"name":"fileSearchStores/store1/documents/one"}],"nextPageToken":"next"})"};
    });
    EXPECT_EQ(later_page.google_delete_store("fileSearchStores/store1", "key", cancel).kind, FolderStoreRemoteKind::failed);
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_TRUE(requests[1].starts_with("GET "));
}

TEST(FolderStoreHttp, GoogleCleanupStopsBeforeParentOnDocumentFailureOrCancellation) {
    for (const bool cancelled : {false, true}) {
        std::atomic_bool cancel{false};
        int requests = 0;
        FolderStoreClient client([&](const FolderStoreHttpRequest& request, const std::atomic_bool&) {
            ++requests;
            if (request.method == "GET") return FolderStoreHttpResponse{.status = 200,
                .body = R"({"documents":[{"name":"fileSearchStores/store1/documents/one"},{"name":"fileSearchStores/store1/documents/two"}]})"};
            EXPECT_TRUE(request.url.ends_with("/documents/one?force=true"));
            if (cancelled) {
                cancel = true;
                return FolderStoreHttpResponse{.status = 200, .body = "{}"};
            }
            return FolderStoreHttpResponse{.status = 403, .body = R"({"error":{"status":"PERMISSION_DENIED"}})"};
        });
        const auto result = client.google_delete_store("fileSearchStores/store1", "key", cancel);
        EXPECT_EQ(result.kind, cancelled ? FolderStoreRemoteKind::cancelled : FolderStoreRemoteKind::failed);
        EXPECT_EQ(requests, 2);
    }
}

TEST(FolderStoreHttp, RejectsForeignUploadUrlAndMalformedCounts) {
    FolderStoreClient client([](const FolderStoreHttpRequest& request, const std::atomic_bool&) {
        FolderStoreHttpResponse response;
        response.status = 200;
        if (request.url.find(":uploadToFileSearchStore") != std::string::npos) {
            response.headers.push_back({"X-Goog-Upload-URL", "https://evil.example/upload"});
        } else {
            response.body = R"({"activeDocumentsCount":"nope"})";
        }
        return response;
    });
    std::atomic_bool cancel{false};
    EXPECT_EQ(
        client.google_upload_document("fileSearchStores/store1", "note.md", "body", "key", cancel, cancel).kind,
        FolderStoreRemoteKind::failed);
    EXPECT_EQ(
        client.google_inspect_store("fileSearchStores/store1", "key", cancel).kind,
        FolderStoreRemoteKind::failed);
}

TEST(FolderStoreHttp, AuthFailuresRetainCleanupDuty) {
    FolderStoreClient client([](const FolderStoreHttpRequest&, const std::atomic_bool&) {
        FolderStoreHttpResponse response;
        response.status = 401;
        response.body = R"({"error":{"message":"unauthorized"}})";
        return response;
    });
    std::atomic_bool cancel{false};
    const auto result = client.openai_delete_store("vs_1", "sk", cancel);
    EXPECT_EQ(result.kind, FolderStoreRemoteKind::failed);
    EXPECT_FALSE(result.unknown_allocation);
}

TEST(FolderStoreHttp, RejectsUnsafeUploadOriginsAndOperationPathsBeforeSendingCredentials) {
    std::atomic_bool cancel{false};
    for (const auto& url : {"https://evil.example/upload", "http://generativelanguage.googleapis.com/upload",
            "https://generativelanguage.googleapis.com:444/upload", "https://user@generativelanguage.googleapis.com/upload",
            "https://generativelanguage.googleapis.com/upload#fragment", "https://generativelanguage.googleapis.com.evil/upload"}) {
        int calls = 0;
        FolderStoreClient client([&](const auto&, const auto&) {
            ++calls;
            return FolderStoreHttpResponse{.status = 200, .headers = {{"X-Goog-Upload-URL", url}}};
        });
        EXPECT_EQ(client.google_upload_document("fileSearchStores/store1", "note.md", "body", "secret", cancel, cancel).kind, FolderStoreRemoteKind::failed);
        EXPECT_EQ(calls, 1);
    }
    for (const auto& name : {"fileSearchStores/other/upload/operations/op1", "fileSearchStores/store1/upload/operations/",
            "fileSearchStores/store1/upload/operations/../op", "fileSearchStores/store1/upload/operations/op?key=secret"}) {
        FolderStoreClient client([&](const auto& request, const auto&) {
            if (request.capture_headers) return FolderStoreHttpResponse{.status = 200,
                .headers = {{"X-Goog-Upload-URL", "https://generativelanguage.googleapis.com/upload/session"}}};
            return FolderStoreHttpResponse{.status = 200, .body = nlohmann::json{{"name", name}}.dump()};
        });
        const auto result = client.google_upload_document("fileSearchStores/store1", "note.md", "body", "key", cancel, cancel);
        EXPECT_EQ(result.kind, FolderStoreRemoteKind::failed);
        EXPECT_TRUE(result.id.empty());
    }
}

TEST(FolderStoreHttp, MalformedSuccessAndErrorBodiesDoNotClaimReadinessOrDeletion) {
    std::atomic_bool cancel{false};
    for (const auto& body : {"<html>proxy</html>", "null", "[]", "{}", R"({"status":"mystery"})"}) {
        FolderStoreClient client([&](const auto&, const auto&) { return FolderStoreHttpResponse{.status = 200, .body = body}; });
        EXPECT_EQ(client.openai_create_store("Folder", "", "key", cancel).kind, FolderStoreRemoteKind::failed);
        EXPECT_EQ(client.openai_inspect_file("vs_1", "file_1", "key", cancel).kind, FolderStoreRemoteKind::failed);
        EXPECT_EQ(client.openai_delete_store("vs_1", "key", cancel).kind, FolderStoreRemoteKind::failed);
    }
    for (const auto& body : {"<html>proxy</html>", R"({"error":{}})", R"({"error":{"message":"secret echoed"}})"}) {
        FolderStoreClient client([&](const auto&, const auto&) { return FolderStoreHttpResponse{.status = 404, .body = body}; });
        const auto result = client.openai_delete_file("file_1", "key", cancel);
        EXPECT_EQ(result.kind, FolderStoreRemoteKind::failed);
        EXPECT_EQ(result.message.find("secret echoed"), std::string::npos);
    }
}

TEST(FolderStoreHttp, GoogleTerminalErrorsAndCountsAreStrict) {
    std::atomic_bool cancel{false};
    for (const auto& count : {"-1", "+1", " 1", "1.0", "9223372036854775808"}) {
        FolderStoreClient client([&](const auto&, const auto&) {
            return FolderStoreHttpResponse{.status = 200, .body = nlohmann::json{{"activeDocumentsCount", count}}.dump()};
        });
        EXPECT_EQ(client.google_inspect_store("fileSearchStores/store1", "key", cancel).kind, FolderStoreRemoteKind::failed);
    }
    FolderStoreClient client([](const auto&, const auto&) {
        return FolderStoreHttpResponse{.status = 200,
            .body = R"({"name":"fileSearchStores/store1/upload/operations/op1","done":true,"error":{"message":"private note text"}})"};
    });
    const auto failed = client.google_inspect_operation("fileSearchStores/store1/upload/operations/op1", "key", cancel);
    EXPECT_EQ(failed.kind, FolderStoreRemoteKind::failed);
    EXPECT_EQ(failed.message.find("private note text"), std::string::npos);
    FolderStoreClient not_found([](const auto&, const auto&) {
        return FolderStoreHttpResponse{.status = 404, .body = R"({"error":{"code":404,"status":"NOT_FOUND","message":"missing"}})"};
    });
    EXPECT_EQ(not_found.google_delete_store("fileSearchStores/store1", "key", cancel).kind, FolderStoreRemoteKind::not_found);
}

TEST(FolderStoreHttp, GoogleCancelAfterResumableStartDoesNotUploadNoteContent) {
    std::atomic_bool interrupted{false};
    std::atomic_bool cancelled{false};
    int requests = 0;
    FolderStoreClient client([&](const auto& request, const auto& transfer_cancel) {
        ++requests;
        EXPECT_TRUE(request.capture_headers);
        EXPECT_EQ(&transfer_cancel, &interrupted);
        cancelled = true;
        return FolderStoreHttpResponse{.status = 200,
            .headers = {{"X-Goog-Upload-URL", "https://generativelanguage.googleapis.com/upload/session"}}};
    });
    const auto result = client.google_upload_document(
        "fileSearchStores/store1", "note.md", "private note content", "key", interrupted, cancelled);
    EXPECT_EQ(result.kind, FolderStoreRemoteKind::cancelled);
    EXPECT_EQ(requests, 1);
    EXPECT_TRUE(result.id.empty());
    EXPECT_FALSE(interrupted.load());
}

TEST(FolderStoreHttp, GoogleCancelDuringFinalizeStillReturnsItsOperationId) {
    std::atomic_bool interrupted{false};
    std::atomic_bool cancelled{false};
    int requests = 0;
    FolderStoreClient client([&](const auto& request, const auto& transfer_cancel) {
        ++requests;
        EXPECT_EQ(&transfer_cancel, &interrupted);
        if (request.capture_headers) return FolderStoreHttpResponse{.status = 200,
            .headers = {{"X-Goog-Upload-URL", "https://generativelanguage.googleapis.com/upload/session"}}};
        EXPECT_EQ(request.body, "note content");
        cancelled = true;
        EXPECT_FALSE(transfer_cancel.load());
        return FolderStoreHttpResponse{.status = 200,
            .body = R"({"name":"fileSearchStores/store1/upload/operations/op1"})"};
    });
    const auto result = client.google_upload_document(
        "fileSearchStores/store1", "note.md", "note content", "key", interrupted, cancelled);
    EXPECT_EQ(result.kind, FolderStoreRemoteKind::ok);
    EXPECT_EQ(result.id, "fileSearchStores/store1/upload/operations/op1");
    EXPECT_EQ(requests, 2);
}

TEST(FolderStoreHttp, OpenAiIndexErrorsKeepSafeReasonsWithoutProviderText) {
    const std::vector<std::pair<std::string, std::string>> cases{
        {"invalid_file", "The provider could not read this file. Check the note content before uploading again."},
        {"unsupported_file", "The provider does not support this file."},
        {"server_error", "The provider could not process the request. Try again later."},
        {"private unknown code", "Indexing did not complete."},
    };
    std::atomic_bool cancel{false};
    for (const auto& [code, expected] : cases) {
        SCOPED_TRACE(code);
        FolderStoreClient client([&](const auto&, const auto&) {
            return FolderStoreHttpResponse{.status = 200, .body = nlohmann::json{
                {"id", "file_1"}, {"status", "failed"},
                {"last_error", {{"code", code}, {"message", "private note content and secret key"}}}}.dump()};
        });
        for (const auto& result : {
                 client.openai_attach_file("vs_1", "file_1", "key", cancel),
                 client.openai_inspect_file("vs_1", "file_1", "key", cancel)}) {
            EXPECT_EQ(result.kind, FolderStoreRemoteKind::failed);
            EXPECT_EQ(result.message, expected);
        }
    }
}

TEST(FolderStoreHttp, GoogleOperationErrorsKeepSafeReasonsWithoutProviderText) {
    const std::vector<std::pair<int, std::string>> cases{
        {3, "The provider rejected the file or request parameters. Check the note content and configuration."},
        {7, "The saved API key cannot access this resource. Check its project and permissions."},
        {8, "The provider quota or rate limit was reached. Check its quota settings or try again later."},
        {13, "The provider could not process the request. Try again later."},
        {999, "Indexing did not complete."},
    };
    std::atomic_bool cancel{false};
    for (const auto& [code, expected] : cases) {
        SCOPED_TRACE(code);
        FolderStoreClient client([&](const auto& request, const auto&) {
            if (request.capture_headers) return FolderStoreHttpResponse{.status = 200,
                .headers = {{"X-Goog-Upload-URL", "https://generativelanguage.googleapis.com/upload/session"}}};
            return FolderStoreHttpResponse{.status = 200, .body = nlohmann::json{
                {"name", "fileSearchStores/store1/upload/operations/op1"}, {"done", true},
                {"error", {{"code", code}, {"message", "private note content and secret key"}}}}.dump()};
        });
        for (const auto& result : {
                 client.google_upload_document("fileSearchStores/store1", "note.md", "body", "key", cancel, cancel),
                 client.google_inspect_operation("fileSearchStores/store1/upload/operations/op1", "key", cancel)}) {
            EXPECT_EQ(result.kind, FolderStoreRemoteKind::failed);
            EXPECT_EQ(result.message, expected);
            EXPECT_EQ(result.id, "fileSearchStores/store1/upload/operations/op1");
        }
    }
}

TEST(FolderStoreHttp, HttpErrorsKeepSafeReasonsWithoutChangingCleanupUncertainty) {
    struct ErrorCase {
        long status;
        nlohmann::json error;
        std::string expected;
        bool unknown;
    };
    const std::vector<ErrorCase> cases{
        {429, {{"code", "insufficient_quota"}}, "The provider account has insufficient quota. Check its billing and quota settings.", true},
        {429, {{"code", "rate_limit_exceeded"}}, "The provider rate limit was reached. Try again later.", true},
        {400, {{"code", 400}, {"status", "INVALID_ARGUMENT"}}, "The provider rejected the file or request parameters. Check the note content and configuration.", false},
        {400, {{"code", 400}, {"status", "INVALID_ARGUMENT"}, {"details", nlohmann::json::array({{{"@type", "type.googleapis.com/google.rpc.ErrorInfo"}, {"reason", "API_KEY_INVALID"}}})}},
            "The saved API key was rejected.", false},
        {403, {{"code", 403}, {"status", "PERMISSION_DENIED"}}, "The saved API key cannot access this resource. Check its project and permissions.", false},
        {429, {{"code", 429}, {"status", "RESOURCE_EXHAUSTED"}}, "The provider quota or rate limit was reached. Check its quota settings or try again later.", true},
        {413, nlohmann::json::object(), "The provider's file-size limit was exceeded. Reduce the note size before uploading again.", true},
        {503, nlohmann::json::object(), "The provider could not process the request. Try again later.", true},
        {400, {{"code", "private unknown code"}}, "The provider rejected the request.", false},
    };
    std::atomic_bool cancel{false};
    for (const auto& item : cases) {
        SCOPED_TRACE(item.status);
        FolderStoreClient client([&](const auto&, const auto&) {
            auto error = item.error;
            error["message"] = "private note content and secret key";
            return FolderStoreHttpResponse{.status = item.status,
                .body = nlohmann::json{{"error", error}}.dump()};
        });
        for (const auto& result : {client.openai_create_store("Folder", "", "key", cancel),
                 client.google_create_store("Folder", "key", cancel)}) {
            EXPECT_NE(result.kind, FolderStoreRemoteKind::ok);
            EXPECT_EQ(result.message, item.expected);
            EXPECT_EQ(result.unknown_allocation, item.unknown);
        }
    }
}

TEST(FolderStoreHttp, UsesOpenAiIdAndGoogleNameWhenBothFieldsExist) {
    std::atomic_bool cancel{false};
    FolderStoreClient client([](const auto& request, const auto&) {
        return FolderStoreHttpResponse{.status = 200,
            .body = request.url.find("api.openai.com") != std::string::npos
                ? R"({"id":"vs_1","name":"Research ##00000B","object":"vector_store","status":"completed"})"
                : R"({"id":"ignored","name":"fileSearchStores/store1","displayName":"Research"})"};
    });
    EXPECT_EQ(client.openai_create_store("Research", "", "key", cancel).id, "vs_1");
    EXPECT_EQ(client.google_create_store("Research", "key", cancel).id, "fileSearchStores/store1");
    FolderStoreClient missing([](const auto&, const auto&) {
        return FolderStoreHttpResponse{.status = 200, .body = R"({"name":"display-name-without-id"})"};
    });
    const auto failed = missing.openai_create_store("Research", "", "key", cancel);
    EXPECT_EQ(failed.kind, FolderStoreRemoteKind::failed);
    EXPECT_TRUE(failed.id.empty());
    EXPECT_TRUE(failed.unknown_allocation);
}


TEST_F(FolderStoreClientTest, AmbiguousLegacyNamesNeverChooseAnArbitrarySavedKey) {
    const auto duplicate = keys_->create("OpenAI", "same-secret");
    auto config = openai_config(""); config.api_key_env = "OpenAI";
    const auto saved = keys_->list();
    const auto matched = folder_store_match(config, saved);
    EXPECT_FALSE(matched.service);
    EXPECT_TRUE(matched.api_key_id.empty());
    config.api_key_id = duplicate.id;
    EXPECT_EQ(folder_store_match(config, keys_->list()).api_key_id, duplicate.id);
    config.api_key_id = openai_key_;
    EXPECT_EQ(folder_store_match(config, saved).api_key_id, openai_key_);
}


TEST(FolderStoreHttp, OpenAiNativeDescriptionAndEmptyOmission) {
    std::atomic_bool cancel{false};
    const std::string description = "  Первый paragraph 😀\n\n第二段\n ";
    int calls = 0;
    FolderStoreClient client([&](const auto& request, const auto&) {
        const auto body = nlohmann::json::parse(request.body);
        EXPECT_EQ(body.at("name"), "Chosen name");
        if (calls++ == 0) EXPECT_EQ(body.at("description"), description);
        else EXPECT_FALSE(body.contains("description"));
        EXPECT_FALSE(body.contains("metadata"));
        return FolderStoreHttpResponse{.status = 200, .body = R"({"id":"vs_1"})"};
    });
    EXPECT_EQ(client.openai_create_store("Chosen name", description, "key", cancel).kind, FolderStoreRemoteKind::ok);
    EXPECT_EQ(client.openai_create_store("Chosen name", "", "key", cancel).kind, FolderStoreRemoteKind::ok);
    EXPECT_EQ(calls, 2);
}

TEST(FolderStoreHttp, GoogleMetadataUsesByteSafeOrderedListsAndPlainText) {
    std::atomic_bool cancel{false};
    for (const std::string& description : {std::string{}, std::string("short"), std::string(199, 'a'),
            std::string(200, 'a'), std::string(201, 'a'), std::string(199, 'a') + "😀é中\n\n" + std::string(700, 'z')}) {
        SCOPED_TRACE(description.size());
        const std::string name = std::string(199, 'n') + "😀é中";
        int calls = 0;
        FolderStoreClient client([&](const auto& request, const auto&) {
            const auto header = [&](std::string_view key) {
                for (const auto& h : request.headers) if (h.name == key) return h.value;
                return std::string{};
            };
            if (calls++ == 0) {
                const auto body = nlohmann::json::parse(request.body);
                EXPECT_EQ(body.at("displayName"), "DESCRIPTION.txt");
                EXPECT_EQ(body.at("mimeType"), "text/plain");
                EXPECT_EQ(header("X-Goog-Upload-Header-Content-Type"), "text/plain");
                EXPECT_EQ(header("X-Goog-Upload-Header-Content-Length"), "1");
                const auto& metadata = body.at("customMetadata");
                EXPECT_EQ(metadata.size(), description.empty() ? 1U : 2U);
                for (const auto& entry : metadata) {
                    std::string reconstructed;
                    for (const auto& piece : entry.at("stringListValue").at("values")) {
                        const auto text = piece.template get<std::string>();
                        EXPECT_LE(text.size(), 200U);
                        EXPECT_FALSE(text.empty());
                        // Strict JSON decoding/encoding also checks every piece is valid UTF-8.
                        EXPECT_NO_THROW((void)nlohmann::json(text).dump());
                        reconstructed += text;
                    }
                    EXPECT_EQ(reconstructed, entry.at("key") == "name" ? name : description);
                }
                return FolderStoreHttpResponse{.status = 200, .headers = {{"X-Goog-Upload-URL", "https://generativelanguage.googleapis.com/upload/session"}}};
            }
            EXPECT_EQ(request.body, "x");
            EXPECT_EQ(header("Content-Type"), "text/plain");
            EXPECT_EQ(header("Content-Length"), "1");
            return FolderStoreHttpResponse{.status = 200, .body = R"({"name":"fileSearchStores/test/upload/operations/op1","done":true})"};
        });
        EXPECT_EQ(client.google_upload_store_metadata("fileSearchStores/test", name, description, "key", cancel, cancel).kind,
            FolderStoreRemoteKind::ok);
        EXPECT_EQ(calls, 2);
    }
}

} // namespace

TEST(LibrarySearch, OpenAiUsesCapturedScopeAndKeepsPartialResults) {
    LibrarySearchScope scope{"openai", {"vs_one", "vs_two"}, "library-secret", 37};
    std::vector<FolderStoreHttpRequest> requests;
    FolderStoreClient client([&](const auto& request, const auto&) {
        requests.push_back(request);
        EXPECT_EQ(request.method, "POST");
        EXPECT_EQ(request.timeout_s, 37);
        EXPECT_EQ(request.headers[0].name, "Authorization");
        EXPECT_EQ(request.headers[0].value, "Bearer library-secret");
        EXPECT_EQ(nlohmann::json::parse(request.body), (nlohmann::json{{"query", "standalone query"}, {"max_num_results", 5}}));
        if (requests.size() == 2) return FolderStoreHttpResponse{403, R"({"error":{"message":"library-secret private query"}})"};
        return FolderStoreHttpResponse{200, R"({"has_more":true,"data":[{"file_id":"file_1","filename":"Book","content":[{"type":"text","text":"First"},{"type":"image"},{"type":"text","text":"Second"}]}]})"};
    });
    test::TestWorkspace workspace;
    const auto log_file = workspace.root() / "retrieval.log";
    initialize_diagnostic_logging(log_file, "info");
    const auto encoded = client.search_library(scope, "standalone query", std::atomic_bool{false});
    shutdown_diagnostic_logging();
    std::ifstream log_input(log_file);
    const std::string diagnostic{std::istreambuf_iterator<char>(log_input), std::istreambuf_iterator<char>()};
    EXPECT_NE(diagnostic.find("Library search: service=openai stores=2 matches=1 partial=true truncated=true"), std::string::npos);
    for (const auto* private_text : {"library-secret", "standalone query", "private query", "First", "Second"})
        EXPECT_EQ(diagnostic.find(private_text), std::string::npos);
    const auto result = nlohmann::json::parse(encoded);
    ASSERT_EQ(requests.size(), 2u);
    EXPECT_EQ(requests[0].url, "https://api.openai.com/v1/vector_stores/vs_one/search");
    EXPECT_EQ(requests[1].url, "https://api.openai.com/v1/vector_stores/vs_two/search");
    ASSERT_EQ(result["matches"].size(), 1u);
    EXPECT_EQ(result["matches"][0], (nlohmann::json{{"store_id", "vs_one"}, {"file_id", "file_1"},
        {"title", "Book"}, {"uri", nullptr}, {"text", "First\nSecond"}}));
    EXPECT_EQ(result["errors"][0]["store_id"], "vs_two");
    EXPECT_TRUE(result["truncated"]);
    EXPECT_FALSE(result.contains("error"));
    EXPECT_EQ(encoded.find("library-secret"), std::string::npos);
    EXPECT_EQ(encoded.find("private query"), std::string::npos);
}

TEST(LibrarySearch, EmptyOpenAiSearchIsSuccessfulButMalformedSourcesAreErrors) {
    for (const auto* body : {R"({"data":[]})", R"({"data":[{"file_id":4}]})", "not json",
            R"({"data":[{"file_id":"f","filename":"t","content":[{"type":"text","text":7}]}]})"}) {
        FolderStoreClient client([&](const auto&, const auto&) { return FolderStoreHttpResponse{200, body}; });
        const auto result = nlohmann::json::parse(client.search_library(
            {"openai", {"vs_a"}, "secret", 600}, "q", std::atomic_bool{false}));
        EXPECT_TRUE(result["matches"].empty());
        EXPECT_EQ(result.contains("error"), std::string_view(body) != R"({"data":[]})");
    }
}

TEST(LibrarySearch, GeminiUsesOneFixedHelperAndReturnsOnlySourcePassages) {
    using Json = nlohmann::json;
    int calls = 0;
    FolderStoreClient client([&](const auto& request, const auto&) {
        ++calls;
        EXPECT_EQ(request.url, "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.8-flash:generateContent");
        EXPECT_EQ(request.headers[0].name, "x-goog-api-key");
        EXPECT_EQ(request.headers[0].value, "library-key");
        EXPECT_EQ(request.timeout_s, 29);
        const auto body = Json::parse(request.body);
        EXPECT_EQ(body["tools"], (Json::array({{{"fileSearch", {
            {"fileSearchStoreNames", {"fileSearchStores/a", "fileSearchStores/b"}}, {"topK", 10}}}}})));
        EXPECT_EQ(body["generationConfig"], (Json{{"candidateCount", 1}, {"maxOutputTokens", 2048}}));
        EXPECT_EQ(body["contents"].size(), 1u);
        return FolderStoreHttpResponse{200, R"({"candidates":[{"finishReason":"STOP","content":{"parts":[{"text":"Invented helper answer"}]},"groundingMetadata":{"groundingChunks":[{"retrievedContext":{"text":"Real first passage","title":"First","fileSearchStore":"fileSearchStores/a"}},{"retrievedContext":{"text":"Real second passage","uri":"private://source"}}]}}]})"};
    });
    const auto encoded = client.search_library({"google", {"fileSearchStores/a", "fileSearchStores/b"}, "library-key", 29},
        "query", std::atomic_bool{false});
    const auto result = Json::parse(encoded);
    EXPECT_EQ(calls, 1);
    ASSERT_EQ(result["matches"].size(), 2u);
    EXPECT_EQ(result["matches"][0]["text"], "Real first passage");
    EXPECT_EQ(result["matches"][0]["store_id"], "fileSearchStores/a");
    EXPECT_TRUE(result["matches"][0]["file_id"].is_null());
    EXPECT_TRUE(result["matches"][0]["uri"].is_null());
    EXPECT_TRUE(result["matches"][1]["store_id"].is_null());
    EXPECT_TRUE(result["matches"][1]["title"].is_null());
    EXPECT_EQ(result["matches"][1]["uri"], "private://source");
    EXPECT_EQ(encoded.find("Invented helper answer"), std::string::npos);
    EXPECT_EQ(encoded.find("library-key"), std::string::npos);
}

TEST(LibrarySearch, GeminiRejectsForeignAndUngroundedResponsesAndAcceptsGroundedTokenLimit) {
    using Json = nlohmann::json;
    const auto context = [](Json source, std::string finish = "STOP") {
        return Json{{"candidates", Json::array({{{"finishReason", finish},
            {"groundingMetadata", {{"groundingChunks", Json::array({{{"retrievedContext", source}}})}}}}})}};
    };
    for (const auto& body : {
            context({{"text", "source"}, {"fileSearchStore", "fileSearchStores/foreign"}}),
            context({{"text", "source"}}, "SAFETY"), Json{{"candidates", Json::array()}},
            Json::parse(R"({"candidates":[{"finishReason":"STOP","content":{"parts":[{"text":"answer"}]}}]})"),
            context({{"text", 7}}), context(Json::object())}) {
        FolderStoreClient client([&](const auto&, const auto&) { return FolderStoreHttpResponse{200, body.dump()}; });
        const auto result = Json::parse(client.search_library({"google", {"fileSearchStores/a"}, "key", 10},
            "q", std::atomic_bool{false}));
        EXPECT_EQ(result["error"], "unavailable");
        EXPECT_TRUE(result["matches"].empty());
        EXPECT_TRUE(result["errors"][0]["store_id"].is_null());
    }
    FolderStoreClient client([&](const auto&, const auto&) {
        return FolderStoreHttpResponse{200, context({{"text", "passage"}}, "MAX_TOKENS").dump()};
    });
    const auto result = Json::parse(client.search_library({"google", {"fileSearchStores/a"}, "key", 10},
        "q", std::atomic_bool{false}));
    EXPECT_FALSE(result.contains("error"));
    EXPECT_TRUE(result["truncated"]);
    EXPECT_EQ(result["matches"][0]["store_id"], "fileSearchStores/a");
}

TEST(LibrarySearch, Utf8MetadataAndSerializedEscapingStayWithinBounds) {
    using Json = nlohmann::json;
    for (const bool escaped : {false, true}) {
        std::string text;
        for (int i = 0; i < 2000; ++i) text += escaped ? "\x01\x02\x03" : "📚x";
        Json data = Json::array();
        for (int i = 0; i < 7; ++i) data.push_back({{"file_id", "file"}, {"filename", text},
            {"content", Json::array({{{"type", "text"}, {"text", text}}})}});
        int calls = 0;
        FolderStoreClient client([&](const auto&, const auto&) { ++calls; return FolderStoreHttpResponse{200, Json{{"data", data}}.dump()}; });
        const auto encoded = client.search_library({"openai", {"vs_a", "vs_b"}, "key", 600}, "q", std::atomic_bool{false});
        const auto result = Json::parse(encoded);
        EXPECT_EQ(calls, 2);
        EXPECT_LE(encoded.size(), 64u * 1024);
        EXPECT_TRUE(result["truncated"]);
        ASSERT_FALSE(result["matches"].empty());
        EXPECT_LE(result["matches"].size(), 10u);
        if (escaped) EXPECT_LT(result["matches"].size(), 10u);
        for (const auto& match : result["matches"]) {
            EXPECT_LE(match["text"].get<std::string>().size(), 4096u);
            EXPECT_LE(match["title"].get<std::string>().size(), 512u);
        }
    }
}

TEST(LibrarySearch, GeminiBoundsTextChunksAndReportsOmittedImages) {
    using Json = nlohmann::json;
    Json chunks = Json::array({{{"image", {{"uri", "unused://image"}}}}});
    for (int i = 0; i < 12; ++i) chunks.push_back({{"retrievedContext", {{"text", "Passage " + std::to_string(i)}}}});
    FolderStoreClient client([&](const auto&, const auto&) {
        return FolderStoreHttpResponse{200, Json{{"candidates", Json::array({{{"finishReason", "STOP"},
            {"groundingMetadata", {{"groundingChunks", chunks}}}}})}}.dump()};
    });
    const auto result = Json::parse(client.search_library({"google", {"fileSearchStores/a"}, "key", 600},
        "q", std::atomic_bool{false}));
    ASSERT_EQ(result["matches"].size(), 10u);
    EXPECT_EQ(result["matches"][0]["text"], "Passage 0");
    EXPECT_EQ(result["matches"][9]["text"], "Passage 9");
    EXPECT_TRUE(result["truncated"]);
    EXPECT_TRUE(result["errors"].empty());
}

TEST(LibrarySearch, CancellationStopsBeforeBetweenAndAfterRequests) {
    for (bool before : {false, true}) {
        std::atomic_bool cancel{before};
        int calls = 0;
        FolderStoreClient client([&](const auto&, const auto&) {
            ++calls;
            cancel = true;
            return FolderStoreHttpResponse{200, R"({"data":[]})"};
        });
        const auto result = nlohmann::json::parse(client.search_library({"openai", {"vs_a", "vs_b"}, "key", 600}, "q", cancel));
        EXPECT_EQ(calls, before ? 0 : 1);
        EXPECT_EQ(result["error"], "cancelled");
    }
    EXPECT_EQ(FolderStoreHttpRequest{}.timeout_s, 600);
}

TEST(LibrarySearch, TransportTimeoutReturnsSafeScopedErrors) {
    for (const auto* service : {"openai", "google"}) {
        const std::string store = std::string_view(service) == "openai"
            ? "vs_one" : "fileSearchStores/one";
        FolderStoreClient client([](const auto& request, const auto&) {
            EXPECT_EQ(request.timeout_s, 17);
            return FolderStoreHttpResponse{
                .error = "Operation timed out: private-query library-secret"};
        });
        const auto encoded = client.search_library(
            {service, {store}, "library-secret", 17}, "private-query",
            std::atomic_bool{false});
        const auto result = nlohmann::json::parse(encoded);
        EXPECT_EQ(result["error"], "unavailable");
        EXPECT_TRUE(result["matches"].empty());
        ASSERT_EQ(result["errors"].size(), 1u);
        EXPECT_EQ(result["errors"][0]["store_id"],
            std::string_view(service) == "openai" ? nlohmann::json(store) : nlohmann::json(nullptr));
        EXPECT_EQ(encoded.find("private-query"), std::string::npos);
        EXPECT_EQ(encoded.find("library-secret"), std::string::npos);
    }
}

TEST(FolderStoreHttp, CurlHonorsTheCapturedRequestTimeout) {
    // This HTTP peer accepts the connection but cannot complete TLS. The
    // request timeout must bound the handshake without relaxing HTTPS-only use.
    MockHttpServer server({http_response("application/json", "{}")});
    server.start();
    FolderStoreHttpRequest request;
    request.method = "GET";
    request.url = "https://127.0.0.1:" + std::to_string(server.port()) + "/slow";
    request.timeout_s = 1;
    const auto started = std::chrono::steady_clock::now();
    const auto response = folder_store_curl_transport(request, std::atomic_bool{false});
    const auto elapsed = std::chrono::steady_clock::now() - started;
    EXPECT_GE(elapsed, std::chrono::milliseconds(500));
    EXPECT_LT(elapsed, std::chrono::seconds(4));
    EXPECT_FALSE(response.cancelled);
    EXPECT_FALSE(response.error.empty());
}

} // namespace cha
