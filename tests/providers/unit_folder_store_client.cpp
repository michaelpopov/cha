#include "providers/folder_store_client.h"
#include "support/test_workspace.h"
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
    auto matched = folder_store_match(openai, *keys_);
    ASSERT_TRUE(matched.service);
    EXPECT_EQ(*matched.service, FolderStoreService::openai);
    EXPECT_EQ(matched.api_key_id, openai_key_);

    openai.host = "API.OpenAI.COM";
    EXPECT_TRUE(folder_store_match(openai, *keys_).service);

    openai.host = "proxy.api.openai.com";
    EXPECT_FALSE(folder_store_match(openai, *keys_).service);

    openai = openai_config(openai_key_);
    openai.port = 8443;
    EXPECT_FALSE(folder_store_match(openai, *keys_).service);
    openai = openai_config(openai_key_);
    openai.https = false;
    EXPECT_FALSE(folder_store_match(openai, *keys_).service);
    openai = openai_config(openai_key_);
    openai.mode = Mode::test;
    EXPECT_FALSE(folder_store_match(openai, *keys_).service);
    openai = openai_config(openai_key_);
    openai.api = ProviderApi::chat_completions;
    EXPECT_FALSE(folder_store_match(openai, *keys_).service);
    openai = openai_config(openai_key_);
    openai.auth = ProviderAuth::openai_subscription;
    EXPECT_FALSE(folder_store_match(openai, *keys_).service);
    openai = openai_config(openai_key_);
    openai.base_path = "/v1";
    EXPECT_FALSE(folder_store_match(openai, *keys_).service);

    auto google = google_config(google_key_);
    matched = folder_store_match(google, *keys_);
    ASSERT_TRUE(matched.service);
    EXPECT_EQ(*matched.service, FolderStoreService::google);

    google.base_path = "/v1";
    EXPECT_FALSE(folder_store_match(google, *keys_).service);
}

TEST_F(FolderStoreClientTest, ResolvesLegacyNamesAndDeduplicatesById) {
    auto config = openai_config();
    config.api_key_id.clear();
    config.api_key_env = "OpenAI";
    auto matched = folder_store_match(config, *keys_);
    ASSERT_TRUE(matched.service);
    EXPECT_EQ(matched.api_key_id, openai_key_);

    config = openai_config({});
    EXPECT_FALSE(folder_store_match(config, *keys_).service);

    config = openai_config("missing");
    EXPECT_FALSE(folder_store_match(config, *keys_).service);

    const std::vector<ModelBackendConfig> configs{
        openai_config(openai_key_),
        openai_config(openai_key_),
        google_config(google_key_),
        google_config(openai_key_),
    };
    const auto choices = folder_store_choices_from_providers(configs, *keys_);
    ASSERT_EQ(choices.size(), 3U);
}

TEST_F(FolderStoreClientTest, ClassifiesEndpointsBeforeAuthenticationAndKeyValidation) {
    for (auto config : {openai_config("missing"), google_config("missing")}) {
        const auto expected = config.api == ProviderApi::responses
            ? FolderStoreService::openai : FolderStoreService::google;
        EXPECT_EQ(folder_store_endpoint_service(config), expected);
        EXPECT_FALSE(folder_store_match(config, *keys_).service);
        config.auth = ProviderAuth::openai_subscription;
        EXPECT_EQ(folder_store_endpoint_service(config), expected);
        EXPECT_FALSE(folder_store_match(config, *keys_).service);
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

TEST_F(FolderStoreClientTest, DistinctIdsDoNotMatchOnEqualValues) {
    const auto other = keys_->create("OpenAI 2", "sk-test").id;
    const std::vector<ModelBackendConfig> configs{
        openai_config(openai_key_),
        openai_config(other),
    };
    EXPECT_EQ(folder_store_choices_from_providers(configs, *keys_).size(), 2U);
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
    EXPECT_EQ(client.openai_create_store("Research ##00000B", "sk", cancel).id, "vs_1");
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
        EXPECT_EQ(client.openai_create_store("Folder", "key", cancel).kind, FolderStoreRemoteKind::failed);
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
        for (const auto& result : {client.openai_create_store("Folder", "key", cancel),
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
    EXPECT_EQ(client.openai_create_store("Research", "key", cancel).id, "vs_1");
    EXPECT_EQ(client.google_create_store("Research", "key", cancel).id, "fileSearchStores/store1");
    FolderStoreClient missing([](const auto&, const auto&) {
        return FolderStoreHttpResponse{.status = 200, .body = R"({"name":"display-name-without-id"})"};
    });
    const auto failed = missing.openai_create_store("Research", "key", cancel);
    EXPECT_EQ(failed.kind, FolderStoreRemoteKind::failed);
    EXPECT_TRUE(failed.id.empty());
    EXPECT_TRUE(failed.unknown_allocation);
}


TEST_F(FolderStoreClientTest, AmbiguousLegacyNamesNeverChooseAnArbitrarySavedKey) {
    const auto duplicate = keys_->create("OpenAI", "same-secret");
    auto config = openai_config(""); config.api_key_env = "OpenAI";
    const auto saved = keys_->list();
    for (const auto& matched : {folder_store_match(config, *keys_), folder_store_match(config, saved)}) {
        EXPECT_FALSE(matched.service);
        EXPECT_TRUE(matched.api_key_id.empty());
        EXPECT_NE(matched.reason.find("ambiguous"), std::string::npos);
    }
    config.api_key_id = duplicate.id;
    EXPECT_EQ(folder_store_match(config, *keys_).api_key_id, duplicate.id);
    config.api_key_id = openai_key_;
    EXPECT_EQ(folder_store_match(config, saved).api_key_id, openai_key_);
}

} // namespace
} // namespace cha
