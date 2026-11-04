#include "providers/folder_store_client.h"
#include "providers/api_key_store.h"
#include "support/test_workspace.h"
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

TEST(FolderStoreHttp, SearchesEachOpenAiStoreWithItsOwnKeyAndKeepsPartialFailures) {
    std::vector<std::string> searched;
    FolderStoreClient client([&](const auto& request, const auto&) {
        searched.push_back(request.url);
        EXPECT_EQ(request.method, "POST");
        EXPECT_EQ(nlohmann::json::parse(request.body),
            (nlohmann::json{{"query", "backup policy"}, {"max_num_results", 5}}));
        if (request.url.ends_with("vs_bad/search")) {
            EXPECT_EQ(request.headers[0].value, "Bearer secret-a");
            return FolderStoreHttpResponse{.status = 401,
                .body = R"({"error":{"message":"secret-a"}})"};
        }
        EXPECT_EQ(request.url, "https://api.openai.com/v1/vector_stores/vs_good/search");
        EXPECT_EQ(request.headers[0].value, "Bearer secret-b");
        return FolderStoreHttpResponse{.status = 200, .body = R"({"data":[{
            "file_id":"file_1","filename":"Backups.md",
            "content":[{"type":"text","text":"Back up daily."}]}],"has_more":false})"};
    });
    const std::vector<FolderStoreSearchTarget> targets{
        {"Missing key", "vs_missing", ""}, {"Rejected", "vs_bad", "secret-a"},
        {"Research", "vs_good", "secret-b"}};
    const auto text = client.search_stores(targets, "backup policy", std::atomic_bool{false});
    const auto output = nlohmann::json::parse(text);
    ASSERT_EQ(output["stores"].size(), 3u);
    EXPECT_TRUE(output["stores"][0].contains("error"));
    EXPECT_TRUE(output["stores"][1].contains("error"));
    EXPECT_EQ(output["stores"][2]["folder"], "Research");
    EXPECT_EQ(output["stores"][2]["matches"][0]["filename"], "Backups.md");
    EXPECT_EQ(output["stores"][2]["matches"][0]["text"], "Back up daily.");
    EXPECT_EQ(text.find("secret-"), std::string::npos);
    EXPECT_EQ(searched.size(), 2u);
}

TEST(FolderStoreHttp, AttachedSearchBoundsResultsAndRejectsMalformedResponsesAndUnsafeIds) {
    int calls = 0;
    FolderStoreClient client([&](const auto&, const auto&) {
        ++calls;
        if (calls == 1) return FolderStoreHttpResponse{.status = 200, .body = R"({"data":[{}]})"};
        nlohmann::json data = nlohmann::json::array();
        for (int i = 0; i < 5; ++i) data.push_back({{"file_id", "file_1"}, {"filename", "Note"},
            {"content", nlohmann::json::array({{{"type", "text"}, {"text", std::string(10000, '\n')}}})}});
        return FolderStoreHttpResponse{.status = 200, .body = nlohmann::json{{"data", data}}.dump()};
    });
    std::vector<FolderStoreSearchTarget> targets{{"Unsafe", "../files", "key"}, {"Bad", "vs_bad", "key"}};
    for (int i = 0; i < 10; ++i) targets.push_back({"Long", "vs_long", "key"});
    const auto text = client.search_stores(targets, "query", std::atomic_bool{false});
    const auto output = nlohmann::json::parse(text);
    EXPECT_TRUE(output["stores"][0].contains("error"));
    EXPECT_TRUE(output["stores"][1].contains("error"));
    EXPECT_TRUE(output["truncated"]);
    EXPECT_LE(text.size(), 65536u);
    EXPECT_LT(calls, 11);
    EXPECT_EQ(client.search_stores(targets, " ", std::atomic_bool{false}).find("\"error\""), 1u);
}

TEST(FolderStoreHttp, AttachedSearchCancellationStopsBeforeNextStore) {
    std::atomic_bool cancel{false};
    int calls = 0;
    FolderStoreClient client([&](const auto&, const auto&) {
        ++calls;
        cancel = true;
        return FolderStoreHttpResponse{.status = 200, .body = R"({"data":[]})"};
    });
    const std::vector<FolderStoreSearchTarget> targets{{"One", "vs_one", "key"}, {"Two", "vs_two", "key"}};
    EXPECT_NE(client.search_stores(targets, "query", cancel).find("cancelled"), std::string::npos);
    EXPECT_EQ(calls, 1);
    (void)client.search_stores(targets, "query", cancel);
    EXPECT_EQ(calls, 1);
}

TEST(FolderStoreHttp, MixedStoreSearchUsesEachSavedKeyAndOnlyReturnsGeminiGroundingText) {
    int calls = 0;
    FolderStoreClient client([&](const auto& request, const auto&) {
        ++calls;
        const auto body = nlohmann::json::parse(request.body);
        if (request.url.ends_with("vs_one/search")) {
            EXPECT_EQ(request.headers[0].value, "Bearer openai-secret");
            return FolderStoreHttpResponse{.status = 200, .body = R"({"data":[]})"};
        }
        EXPECT_EQ(request.url, "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.8-flash:generateContent");
        EXPECT_EQ(request.headers[0].name, "x-goog-api-key");
        EXPECT_EQ(body["generationConfig"]["maxOutputTokens"], 2048);
        EXPECT_EQ(body["contents"][0]["parts"][0]["text"], "backup policy");
        if (calls == 1) {
            EXPECT_EQ(request.headers[0].value, "gemini-secret-a");
            EXPECT_EQ(body["tools"][0]["fileSearch"]["fileSearchStoreNames"], nlohmann::json::array({"fileSearchStores/one"}));
            return FolderStoreHttpResponse{.status = 403, .body = R"({"error":{"message":"gemini-secret-a"}})"};
        }
        EXPECT_EQ(request.headers[0].value, "gemini-secret-b");
        EXPECT_EQ(body["tools"][0]["fileSearch"]["fileSearchStoreNames"], nlohmann::json::array({"fileSearchStores/two"}));
        return FolderStoreHttpResponse{.status = 200, .body = R"({"candidates":[{
            "finishReason":"STOP","content":{"parts":[{"text":"SYNTHESIZED ANSWER"}]},
            "groundingMetadata":{"groundingChunks":[{"retrievedContext":{
                "title":"Backups.md","text":"Keep daily backups for 19 days."}}]}}]})"};
    });
    const std::vector<FolderStoreSearchTarget> stores{
        {"Rejected", "fileSearchStores/one", "gemini-secret-a", FolderStoreService::google},
        {"Research", "fileSearchStores/two", "gemini-secret-b", FolderStoreService::google},
        {"OpenAI", "vs_one", "openai-secret"}};
    const auto text = client.search_stores(stores, "backup policy", std::atomic_bool{false});
    const auto result = nlohmann::json::parse(text);
    ASSERT_EQ(result["stores"].size(), 3u);
    EXPECT_TRUE(result["stores"][0].contains("error"));
    EXPECT_EQ(result["stores"][1]["matches"][0]["filename"], "Backups.md");
    EXPECT_EQ(result["stores"][1]["matches"][0]["text"], "Keep daily backups for 19 days.");
    EXPECT_FALSE(result["stores"][2].contains("error"));
    EXPECT_EQ(text.find("SYNTHESIZED"), std::string::npos);
    EXPECT_EQ(text.find("secret"), std::string::npos);
    EXPECT_EQ(calls, 3);
}

TEST(FolderStoreHttp, GeminiSearchReportsMissingOrInvalidGroundingInsteadOfUsingGeneratedAnswers) {
    for (const auto response : {
        R"({"candidates":[{"finishReason":"MAX_TOKENS","content":{"parts":[{"text":"SYNTHESIZED"}]}}]})",
        R"({"candidates":[{"finishReason":"SAFETY"}]})",
        R"({"promptFeedback":{"blockReason":"SAFETY"}})",
        R"({"candidates":[{"groundingMetadata":{"groundingChunks":{}}}]})",
        R"({"candidates":[{"groundingMetadata":{"groundingChunks":[{"retrievedContext":{"text":42}}]}}]})",
        R"({"candidates":[{"groundingMetadata":{"groundingChunks":[{"retrievedContext":{"mediaId":"image"}}]}}]})",
    }) {
        FolderStoreClient client([&](const auto&, const auto&) {
            return FolderStoreHttpResponse{.status = 200, .body = response};
        });
        const std::vector<FolderStoreSearchTarget> stores{{"Gemini", "fileSearchStores/one", "key", FolderStoreService::google}};
        const auto text = client.search_stores(stores, "query", std::atomic_bool{false});
        const auto result = nlohmann::json::parse(text);
        EXPECT_TRUE(result["stores"][0].contains("error")) << response;
        EXPECT_TRUE(result["stores"][0]["matches"].empty());
        EXPECT_EQ(text.find("SYNTHESIZED"), std::string::npos);
    }
}

TEST(FolderStoreHttp, GeminiNormalEmptyResultsAreNotErrorsAndGenerationLimitDoesNotTruncatePassages) {
    for (const auto reason : {"STOP", "MAX_TOKENS"}) {
        for (const bool passages : {false, true}) {
            for (const bool metadata : {false, true}) {
                if (passages && !metadata) continue;
                SCOPED_TRACE(std::string(reason) + (passages ? " grounded" : " empty")
                    + (metadata ? " with metadata" : " without metadata"));
                int searches = 0, inspections = 0;
                FolderStoreClient client([&](const auto& request, const auto&) {
                    EXPECT_EQ(request.headers[0].name, "x-goog-api-key");
                    EXPECT_EQ(request.headers[0].value, "store-key");
                    if (request.method == "GET") {
                        ++inspections;
                        EXPECT_EQ(request.url, "https://generativelanguage.googleapis.com/v1beta/fileSearchStores/one");
                        EXPECT_TRUE(request.body.empty());
                        return FolderStoreHttpResponse{.status = 200,
                            .body = R"({"name":"fileSearchStores/one","activeDocumentsCount":"1"})"};
                    }
                    ++searches;
                    EXPECT_EQ(request.method, "POST");
                    EXPECT_EQ(request.url, "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.8-flash:generateContent");
                    EXPECT_EQ(nlohmann::json::parse(request.body)["tools"][0]["fileSearch"]["fileSearchStoreNames"],
                        nlohmann::json::array({"fileSearchStores/one"}));
                    nlohmann::json candidate{{"finishReason", reason},
                        {"content", {{"parts", nlohmann::json::array({{{"text", "SYNTHESIZED"}}})}}}};
                    if (metadata) candidate["groundingMetadata"]["groundingChunks"] = nlohmann::json::array();
                    if (passages) candidate["groundingMetadata"]["groundingChunks"] = nlohmann::json::array({
                        {{"retrievedContext", {{"title", "Note"}, {"text", "Source passage"}}}}});
                    return FolderStoreHttpResponse{.status = 200,
                        .body = nlohmann::json{{"candidates", nlohmann::json::array({candidate})}}.dump()};
                });
                const std::vector<FolderStoreSearchTarget> targets{{"Gemini", "fileSearchStores/one", "store-key", FolderStoreService::google}};
                const auto text = client.search_stores(targets, "query", std::atomic_bool{false});
                const auto result = nlohmann::json::parse(text);
                EXPECT_EQ(result["stores"][0].contains("error"), !passages && std::string_view(reason) == "MAX_TOKENS");
                EXPECT_FALSE(result["truncated"].get<bool>());
                EXPECT_EQ(result["stores"][0]["matches"].size(), passages ? 1u : 0u);
                if (passages) EXPECT_EQ(result["stores"][0]["matches"][0]["text"], "Source passage");
                EXPECT_EQ(text.find("SYNTHESIZED"), std::string::npos);
                EXPECT_EQ(searches, 1);
                EXPECT_EQ(inspections, !passages && std::string_view(reason) == "STOP" ? 1 : 0);
            }
        }
    }
}

TEST(FolderStoreHttp, GeminiEmptySearchInspectionReportsSafeFailuresAndKeepsHealthyPeer) {
    for (const long status : {404, 403}) {
        SCOPED_TRACE(status);
        test::TestWorkspace workspace;
        const auto path = workspace.root() / "search.log";
        initialize_diagnostic_logging(path, "info");
        std::vector<std::string> requests;
        FolderStoreClient client([&](const auto& request, const auto&) {
            requests.push_back(request.method + " " + request.url);
            EXPECT_EQ(request.headers[0].name, "x-goog-api-key");
            if (request.method == "GET") {
                EXPECT_EQ(request.url, "https://generativelanguage.googleapis.com/v1beta/fileSearchStores/one");
                EXPECT_EQ(request.headers[0].value, "secret-one");
                return FolderStoreHttpResponse{.status = status,
                    .body = status == 404
                        ? R"({"error":{"code":404,"status":"NOT_FOUND","message":"secret-one"}})"
                        : R"({"error":{"status":"PERMISSION_DENIED","message":"secret-one"}})"};
            }
            EXPECT_EQ(request.method, "POST");
            EXPECT_EQ(request.url, "https://generativelanguage.googleapis.com/v1beta/models/gemini-3.8-flash:generateContent");
            const auto store = nlohmann::json::parse(request.body)["tools"][0]["fileSearch"]["fileSearchStoreNames"][0];
            if (store == "fileSearchStores/one") {
                EXPECT_EQ(request.headers[0].value, "secret-one");
                return FolderStoreHttpResponse{.status = 200, .body = R"({"candidates":[{"finishReason":"STOP"}]})"};
            }
            EXPECT_EQ(store, "fileSearchStores/two");
            EXPECT_EQ(request.headers[0].value, "secret-two");
            return FolderStoreHttpResponse{.status = 200, .body = R"({"candidates":[{"finishReason":"STOP",
                "groundingMetadata":{"groundingChunks":[{"retrievedContext":{"title":"Peer.md","text":"Healthy source"}}]}}]})"};
        });
        const std::vector<FolderStoreSearchTarget> targets{
            {"Unavailable", "fileSearchStores/one", "secret-one", FolderStoreService::google},
            {"Healthy", "fileSearchStores/two", "secret-two", FolderStoreService::google}};
        const auto text = client.search_stores(targets, "query", std::atomic_bool{false});
        shutdown_diagnostic_logging();
        const auto result = nlohmann::json::parse(text);
        ASSERT_EQ(result["stores"].size(), 2u);
        const std::string message = status == 404 ? "The remote resource was not found."
            : "The saved API key cannot access this resource. Check its project and permissions.";
        EXPECT_EQ(result["stores"][0]["error"], message);
        EXPECT_TRUE(result["stores"][0]["matches"].empty());
        EXPECT_FALSE(result["stores"][1].contains("error"));
        EXPECT_EQ(result["stores"][1]["matches"][0]["text"], "Healthy source");
        EXPECT_EQ(requests.size(), 3u);
        std::ifstream file(path);
        const std::string log{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        EXPECT_NE(log.find("Store search failed for Unavailable: " + message), std::string::npos);
        EXPECT_EQ(log.find("secret-"), std::string::npos);
        EXPECT_EQ(text.find("secret-"), std::string::npos);
    }
}

TEST(FolderStoreHttp, GeminiInspectionCancellationStopsBeforeNextStore) {
    // Both the cancellation flag and a transport cancellation must stop the search.
    for (const bool flag : {false, true}) {
        std::atomic_bool cancel{false};
        int searches = 0, inspections = 0;
        FolderStoreClient client([&](const auto& request, const auto& cancellation) {
            EXPECT_EQ(&cancellation, &cancel);
            if (request.method == "GET") {
                ++inspections;
                if (flag) {
                    cancel = true;
                    return FolderStoreHttpResponse{.status = 200, .body = R"({"name":"fileSearchStores/one"})"};
                }
                return FolderStoreHttpResponse{.cancelled = true};
            }
            ++searches;
            return FolderStoreHttpResponse{.status = 200, .body = R"({"candidates":[{"finishReason":"STOP"}]})"};
        });
        const std::vector<FolderStoreSearchTarget> targets{
            {"One", "fileSearchStores/one", "key-one", FolderStoreService::google},
            {"Two", "fileSearchStores/two", "key-two", FolderStoreService::google}};
        EXPECT_EQ(client.search_stores(targets, "query", cancel), R"({"error":"Search cancelled."})");
        EXPECT_EQ(searches, 1);
        EXPECT_EQ(inspections, 1);
    }
}

TEST(FolderStoreHttp, SearchFailuresLogFolderAndSafeReasonWithoutCredentials) {
    test::TestWorkspace workspace;
    const auto path = workspace.root() / "search.log";
    initialize_diagnostic_logging(path, "info");
    FolderStoreClient client([](const auto&, const auto&) {
        return FolderStoreHttpResponse{.status = 401, .body = R"({"error":{"message":"secret-value"}})"};
    });
    const std::vector<FolderStoreSearchTarget> targets{{"Research", "vs_one", "secret-value"}};
    (void)client.search_stores(targets, "query", std::atomic_bool{false});
    shutdown_diagnostic_logging();
    std::ifstream file(path);
    const std::string log{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    EXPECT_NE(log.find("Store search failed for Research: The saved API key was rejected."), std::string::npos);
    EXPECT_EQ(log.find("secret-value"), std::string::npos);
}

TEST(FolderStoreHttp, GeminiSearchBoundsGroundingAndHandlesCancellationAndUnsafeNames) {
    int calls = 0;
    FolderStoreClient client([&](const auto&, const auto&) {
        ++calls;
        nlohmann::json chunks = nlohmann::json::array();
        for (int i = 0; i < 7; ++i)
            chunks.push_back({{"retrievedContext", {{"title", "Note"}, {"text", std::string(10000, 'x')}}}});
        return FolderStoreHttpResponse{.status = 200, .body = nlohmann::json{{"candidates", nlohmann::json::array({
            {{"finishReason", "STOP"}, {"groundingMetadata", {{"groundingChunks", chunks}}}}})}}.dump()};
    });
    std::vector<FolderStoreSearchTarget> stores{{"Unsafe", "fileSearchStores/../files", "key", FolderStoreService::google}};
    for (int i = 0; i < 5; ++i) stores.push_back({"Gemini", "fileSearchStores/one", "key", FolderStoreService::google});
    const auto text = client.search_stores(stores, "query", std::atomic_bool{false});
    const auto result = nlohmann::json::parse(text);
    EXPECT_TRUE(result["stores"][0].contains("error"));
    EXPECT_EQ(result["stores"][1]["matches"].size(), 5u);
    EXPECT_EQ(result["stores"][1]["matches"][0]["text"].get<std::string>().size(), 4096u);
    EXPECT_TRUE(result["truncated"]);
    EXPECT_LE(text.size(), 65536u);
    EXPECT_LT(calls, 5);
    std::atomic_bool cancel{false};
    calls = 0;
    FolderStoreClient cancelled([&](const auto&, const auto&) {
        ++calls;
        cancel = true;
        return FolderStoreHttpResponse{.status = 200, .body = "{}"};
    });
    (void)cancelled.search_stores(std::span(stores).subspan(1), "query", cancel);
    EXPECT_EQ(calls, 1);
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
} // namespace cha
