#include "providers/web_search.h"
#include "support/mock_http_server.h"
#include "support/test_workspace.h"
#include "util/logging.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <future>
#include <fstream>
#include <thread>
#include <vector>

namespace cha {
namespace {
using namespace std::chrono_literals;

TEST(BraveSearch, RequestsExtraPlainTextSnippetsAndPreservesTheResponse) {
    const std::string body = R"json({"web":{"results":[
        {"title":"A source","url":"https://example.org/a","description":"A summary",
         "extra_snippets":["", " \t\n", "A summary", "More context",123,null,"More context"],"unused":"not needed"},
        {"url":"javascript:alert(1)"}, {"title":"No URL"},
        {"title":"Another source","url":"https://example.org/b"}]}})json";
    MockHttpServer server({http_response("application/json", body)});
    server.start();
    std::atomic_bool cancelled{};
    const auto result = nlohmann::json::parse(search_brave("C++ & café?", "test-secret",
        cancelled, "http://127.0.0.1:" + std::to_string(server.port()) + "/res/v1/web/search"));
    server.join();
    ASSERT_EQ(server.requests().size(), 1u);
    const auto& request = server.requests().front();
    EXPECT_TRUE(request.starts_with(
        "GET /res/v1/web/search?q=C%2B%2B%20%26%20caf%C3%A9%3F&count=10&extra_snippets=true&text_decorations=false&result_filter=web,news,discussions,faq,infobox,query HTTP/1.1\r\n"));
    EXPECT_NE(request.find("X-Subscription-Token: test-secret\r\n"), std::string::npos);
    EXPECT_EQ(result, nlohmann::json::parse(body));
}

TEST(BraveSearch, KeepsAllResultsInProviderOrderWithMetadataAndOtherCategories) {
    auto rows = nlohmann::json::array();
    for (int i = 0; i < 20; ++i) {
        rows.push_back({{"url", "https://cnn.com/article" + std::to_string(i)},
            {"title", "Headline " + std::to_string(i)}, {"age", "2 hours ago"},
            {"page_age", "2026-09-26"}, {"description", "Original excerpt"},
            {"extra_snippets", {"One", "Two", "Three", "Four", "Five", "Six"}}});
    }
    const nlohmann::json body{{"web", {{"results", rows}}},
        {"news", {{"results", {{{"title", "News result"}, {"url", "https://cnn.com/news"}}}}}}};
    MockHttpServer server({http_response("application/json", body.dump())});
    server.start();
    const auto result = nlohmann::json::parse(search_brave("CNN today", "key", std::atomic_bool{false},
        "http://127.0.0.1:" + std::to_string(server.port())));
    server.join();
    EXPECT_EQ(result, body);
}

TEST(BraveSearch, HandlesEmptyResultsAndRejectsMalformedAndHttpFailures) {
    std::atomic_bool cancelled{};
    for (const auto* body : {R"({"query":{}})", R"({"web":{"results":[]}})"}) {
        MockHttpServer server({http_response("application/json", body)});
        server.start();
        const auto result = nlohmann::json::parse(search_brave("query", "key", cancelled,
            "http://127.0.0.1:" + std::to_string(server.port())));
        server.join();
        EXPECT_EQ(result, nlohmann::json::parse(body));
    }
    for (const auto& response : {
        http_response("application/json", "broken"),
        http_response("application/json", R"({"web":{"results":{}}})"),
        http_response("application/json", R"({"error":"secret"})"),
        std::string("HTTP/1.1 429 Too Many Requests\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"),
        std::string("HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:1/\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")}) {
        MockHttpServer server({response});
        server.start();
        EXPECT_THROW(search_brave("query", "key", cancelled,
            "http://127.0.0.1:" + std::to_string(server.port())), std::runtime_error);
        server.join();
        EXPECT_EQ(server.requests().size(), 1u);
    }
}

TEST(BraveSearch, LimitsLongQueriesToWholeWordsWithinBothLimits) {
    const std::string utf8_word = std::string(198, 'x') + "é";
    for (const auto& word : {std::string("word"), utf8_word}) {
        std::string query;
        const int retained_words = word == "word" ? 75 : 2;
        for (int i = 0; i < 100; ++i) {
            if (i > 0) query += ' ';
            query += word;
        }
        MockHttpServer server({http_response("application/json", R"({"web":{"results":[]}})")});
        server.start();
        std::atomic_bool cancelled{};
        const auto result = nlohmann::json::parse(search_brave(query, "key", cancelled,
            "http://127.0.0.1:" + std::to_string(server.port())));
        server.join();
        EXPECT_TRUE(result["web"]["results"].empty());
        std::string encoded;
        for (int i = 0; i < retained_words; ++i) {
            if (i > 0) encoded += "%20";
            encoded += word == "word" ? "word" : std::string(198, 'x') + "%C3%A9";
        }
        ASSERT_EQ(server.requests().size(), 1u);
        EXPECT_TRUE(server.requests().front().starts_with("GET /?q=" + encoded
            + "&count=10&extra_snippets=true&text_decorations=false&result_filter=web,news,discussions,faq,infobox,query HTTP/1.1\r\n"));
    }
    std::atomic_bool cancelled{};
    EXPECT_THROW(search_brave(std::string(601, 'x'), "key", cancelled,
        "http://127.0.0.1:1"), std::runtime_error);
}

TEST(BraveSearch, DebugLogsRawResultsOnceAndSkipsEmptyPayloadsForBothProviders) {
    for (bool brave : {true, false}) {
        for (const auto* level : {"debug", "info"}) {
            test::TestWorkspace fixture;
            const auto path = fixture.root() / "search-debug.log";
            initialize_diagnostic_logging(path, level);
            nlohmann::json rows = nlohmann::json::array();
            for (int i = 0; i < 6; ++i)
                rows.push_back({{"url", "https://cnn.com/article" + std::to_string(i)},
                    {"title", "headline" + std::to_string(i)}, {"age", "old-date"},
                    {"description", "snippet"}, {"content", "snippet"}});
            nlohmann::json body{{"results", rows}, {"echo", "secret-search-key"}};
            if (brave) body = nlohmann::json{{"web", body}};
            MockHttpServer server({http_response("application/json", body.dump())});
            server.start();
            const auto endpoint = "http://127.0.0.1:" + std::to_string(server.port());
            const auto results = brave
                ? search_brave("CNN today", "secret-search-key", std::atomic_bool{false}, endpoint)
                : search_tavily("CNN today", "secret-search-key", std::atomic_bool{false}, endpoint);
            server.join();
            shutdown_diagnostic_logging();
            std::ifstream file(path);
            const std::string output{std::istreambuf_iterator<char>(file), {}};
            EXPECT_EQ(output.find("secret-search-key"), std::string::npos);
            EXPECT_EQ(output.find("Authorization:"), std::string::npos);
            EXPECT_EQ(output.find("search results passed to model"), std::string::npos);
            EXPECT_EQ(output.find("data=\"\""), std::string::npos);
            if (std::string_view(level) == "debug") {
                EXPECT_NE(output.find("CNN today"), std::string::npos);
                EXPECT_NE(output.find("old-date"), std::string::npos);
                EXPECT_NE(output.find("headline5"), std::string::npos);
                EXPECT_NE(output.find("[REDACTED]"), std::string::npos);
                const auto raw_response = output.find("search raw response");
                ASSERT_NE(raw_response, std::string::npos);
                EXPECT_EQ(output.find("search raw response", raw_response + 1), std::string::npos);
                EXPECT_EQ(output.find("headline5", output.find("headline5") + 1), std::string::npos);
                EXPECT_EQ(output.find("search request body") != std::string::npos, !brave);
            } else {
                EXPECT_EQ(output.find("CNN today"), std::string::npos);
                EXPECT_EQ(output.find("headline5"), std::string::npos);
            }
            EXPECT_NE(results.find("headline5"), std::string::npos);
        }
    }
}

TEST(BraveSearch, LogsSafeFailureReasonsWithoutSecrets) {
    test::TestWorkspace fixture;
    const auto path = fixture.root() / "brave-warnings.log";
    initialize_diagnostic_logging(path, "warn");
    std::atomic_bool cancelled{};
    for (const int status : {401, 422, 429}) {
        const std::string body = "private-response-body";
        MockHttpServer server({"HTTP/1.1 " + std::to_string(status) + " Error\r\nContent-Length: "
            + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body});
        server.start();
        EXPECT_THROW(search_brave("private-query", "private-key", cancelled,
            "http://127.0.0.1:" + std::to_string(server.port())), std::runtime_error);
        server.join();
    }
    for (const auto* body : {"private-response-body", R"({"web":{}})"}) {
        MockHttpServer server({http_response("application/json", body)});
        server.start();
        EXPECT_THROW(search_brave("private-query", "private-key", cancelled,
            "http://127.0.0.1:" + std::to_string(server.port())), std::runtime_error);
        server.join();
    }
    WebSearchContext context;
    EXPECT_TRUE(context.get({}, [](const auto&, auto, const auto&) -> std::string {
        throw std::runtime_error("private-provider-error");
    }, cancelled).empty());
    shutdown_diagnostic_logging();
    std::ifstream log(path);
    const std::string warnings{std::istreambuf_iterator<char>(log), {}};
    for (const auto* reason : {"Web search HTTP 401", "Web search HTTP 422", "Web search HTTP 429",
        "Invalid web search response", "Invalid web search results"}) {
        EXPECT_NE(warnings.find(std::string("Brave search failed: ") + reason), std::string::npos);
    }
    EXPECT_NE(warnings.find("Web search failed; continuing without search results"), std::string::npos);
    for (const auto* secret : {"private-query", "private-key", "private-response-body", "private-provider-error"})
        EXPECT_EQ(warnings.find(secret), std::string::npos);
}

TEST(BraveSearch, CancelsAnInFlightTransfer) {
    MockHttpServer server({"HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n"}, true);
    server.start();
    std::atomic_bool cancelled{};
    auto result = std::async(std::launch::async, [&] {
        return search_brave("query", "key", cancelled,
            "http://127.0.0.1:" + std::to_string(server.port()));
    });
    const bool received = server.wait_for_requests(1, 2s);
    cancelled.store(true);
    EXPECT_TRUE(received);
    EXPECT_EQ(result.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(result.get().empty());
    server.join();
}

TEST(TavilySearch, RequestsTenResultsAndPreservesTheResponse) {
    const std::string body = R"json({"query":"provider query","answer":"Provider answer",
        "response_time":0.42,"request_id":"search-123","images":["https://example.org/image.png"],"results":[
        {"title":"A source","url":"https://example.org/a","content":"A summary",
         "score":0.9,"raw_content":"not needed"},
        {"url":"javascript:alert(1)"}, {"title":"No URL"}, null,
        {"url":123}, {"url":"http://example.org/b","title":12,"content":null},
        {"url":"https://example.org/c"}, {"url":"https://example.org/d"},
        {"url":"https://example.org/e"}, {"url":"https://example.org/f"}]})json";
    MockHttpServer server({http_response("application/json", body)});
    server.start();
    std::atomic_bool cancelled{};
    const std::string query = "C++ & \"café\"?";
    const auto result = nlohmann::json::parse(search_tavily(query, "test-secret",
        cancelled, "http://127.0.0.1:" + std::to_string(server.port()) + "/search"));
    server.join();
    ASSERT_EQ(server.requests().size(), 1u);
    const auto& request = server.requests().front();
    EXPECT_TRUE(request.starts_with("POST /search HTTP/1.1\r\n"));
    EXPECT_NE(request.find("Authorization: Bearer test-secret\r\n"), std::string::npos);
    EXPECT_NE(request.find("Content-Type: application/json\r\n"), std::string::npos);
    const auto sent = nlohmann::json::parse(request.substr(request.find("\r\n\r\n") + 4));
    EXPECT_EQ(sent, (nlohmann::json{{"query", query}, {"max_results", 10},
        {"include_images", false}, {"include_image_descriptions", false},
        {"include_favicon", false}}));
    auto expected = nlohmann::json::parse(body);
    expected.erase("images");
    EXPECT_EQ(result, expected);
}

TEST(WebSearchResults, PreservesAllResultsAndProviderRankingForBothProviders) {
    for (const bool brave : {true, false}) {
        SCOPED_TRACE(brave ? "Brave" : "Tavily");
        auto rows = nlohmann::json::array();
        for (int i = 0; i < 20; ++i)
            rows.push_back({{"url", "https://cnn.com/article" + std::to_string(i)},
                {"title", "Headline " + std::to_string(i)}, {"score", 1.0 - i * 0.01},
                {"content", "Original excerpt"}, {"raw_content", "Full article"},
                {"published_date", "2026-09-26"}});
        nlohmann::json body{{"results", rows}};
        if (brave) body = nlohmann::json{{"web", body}};
        MockHttpServer server({http_response("application/json", body.dump())});
        server.start();
        std::atomic_bool cancelled{};
        const auto endpoint = "http://127.0.0.1:" + std::to_string(server.port());
        const auto result = nlohmann::json::parse(brave
            ? search_brave("query", "key", cancelled, endpoint)
            : search_tavily("query", "key", cancelled, endpoint));
        server.join();
        EXPECT_EQ(result, body);
    }
}

TEST(WebSearchResults, RemovesMediaBeforeApplyingTheSizeLimit) {
    for (bool brave : {true, false}) {
        SCOPED_TRACE(brave);
        const nlohmann::ordered_json article{{"title", "Text article"},
            {"url", "https://cnn.com/article"}, {"description", "Article excerpt"},
            {"extra_snippets", {"More source text"}},
            {"profile", {{"name", "CNN"}}}, {"meta_url", {{"hostname", "cnn.com"}}},
            {"location", {{"name", "Newsroom"}}}};
        auto source = article;
        source["thumbnail"] = {{"src", std::string(40000, 'x')}};
        source["images"] = {"https://cnn.com/photo.jpg"};
        source["favicon"] = "https://cnn.com/favicon.ico";
        source["profile"]["img"] = "https://cnn.com/profile.png";
        source["meta_url"]["favicon"] = "https://cnn.com/favicon.ico";
        source["icons"] = {"https://cnn.com/icon.png"};
        source["location"]["pictures"] = {{{"url", "https://cnn.com/newsroom.jpg"}}};
        source["schemas"] = {{{"@type", "VideoObject"},
            {"thumbnailUrl", "https://cnn.com/thumbnail.jpg"},
            {"contentUrl", "https://cnn.com/movie.mp4"},
            {"embedUrl", "https://cnn.com/player"},
            {"description", std::string(40000, 'x')}}};
        nlohmann::ordered_json body{{"results", {source}}};
        nlohmann::ordered_json expected{{"results", {article}}};
        if (brave) {
            body = {{"web", body}, {"news", body}};
            expected = {{"web", expected}, {"news", expected}};
        }
        body["images"] = {"https://example.org/image.png"};
        body["videos"] = {{"results", {{{"url", "https://example.org/movie.mp4"}}}}};
        body["audio"] = {{"url", "https://example.org/podcast.mp3"}};
        MockHttpServer server({http_response("application/json", body.dump())});
        server.start();
        const auto endpoint = "http://127.0.0.1:" + std::to_string(server.port());
        const auto output = brave
            ? search_brave("query", "key", std::atomic_bool{false}, endpoint)
            : search_tavily("query", "key", std::atomic_bool{false}, endpoint);
        server.join();
        EXPECT_EQ(output, expected.dump());
    }
}

TEST(WebSearchResults, PreservesSerializedKeyOrderBeforeAndAfterTruncation) {
    for (bool brave : {true, false}) {
        for (bool oversized : {false, true}) {
            SCOPED_TRACE(brave);
            SCOPED_TRACE(oversized);
            auto rows = nlohmann::ordered_json::array();
            for (int i = 0; i < (oversized ? 20 : 1); ++i)
                rows.push_back({{"title", "Headline " + std::to_string(i)},
                    {"url", "https://cnn.com/article" + std::to_string(i)},
                    {"description", std::string(2000, 'x')}});
            nlohmann::ordered_json body{{"results", rows}, {"query", "CNN today"}};
            if (brave) body = {{"web", {{"results", rows}}},
                {"news", {{"results", rows}}}, {"query", "CNN today"}};
            MockHttpServer server({http_response("application/json", body.dump())});
            server.start();
            const auto endpoint = "http://127.0.0.1:" + std::to_string(server.port());
            const auto output = brave
                ? search_brave("CNN today", "key", std::atomic_bool{false}, endpoint)
                : search_tavily("CNN today", "key", std::atomic_bool{false}, endpoint);
            server.join();
            if (!oversized) EXPECT_EQ(output, body.dump());
            EXPECT_TRUE(output.starts_with(brave ? "{\"web\":" : "{\"results\":"));
            EXPECT_LT(output.find("\"title\":"), output.find("\"url\":"));
            EXPECT_LT(output.find("\"url\":"), output.find("\"description\":"));
            if (brave) EXPECT_LT(output.find("\"news\":"), output.find("\"query\":"));
            if (oversized) {
                EXPECT_LE(output.size(), 32u * 1024);
                EXPECT_TRUE(nlohmann::json::parse(output).at("truncated").get<bool>());
            }
        }
    }
}

TEST(WebSearchResults, BoundsLargeOutputsAndKeepsWholeRankedResults) {
    for (bool brave : {true, false}) {
        SCOPED_TRACE(brave);
        test::TestWorkspace fixture;
        const auto path = fixture.root() / "search-limit.log";
        initialize_diagnostic_logging(path, "debug");
        auto rows = nlohmann::json::array();
        for (int i = 0; i < 20; ++i)
            rows.push_back({{"url", "https://cnn.com/article" + std::to_string(i)},
                {"title", "Café \"headline\" " + std::to_string(i)},
                {"content", std::string(1500, '\n') + "é — 完整"}, {"score", 0.9}});
        nlohmann::json body{{"results", rows}, {"query", "CNN today"}};
        if (brave) body = {{"web", {{"results", rows}}},
            {"news", {{"results", rows}}}, {"query", {{"original", "CNN today"}}}};
        MockHttpServer server({http_response("application/json", body.dump())});
        server.start();
        const auto endpoint = "http://127.0.0.1:" + std::to_string(server.port());
        const auto output = brave
            ? search_brave("CNN today", "key", std::atomic_bool{false}, endpoint)
            : search_tavily("CNN today", "key", std::atomic_bool{false}, endpoint);
        server.join();
        shutdown_diagnostic_logging();
        EXPECT_LE(output.size(), 32u * 1024);
        const auto result = nlohmann::json::parse(output);
        EXPECT_EQ(result["truncated"], true);
        EXPECT_TRUE(result.contains("truncation_reason"));
        EXPECT_EQ(result["query"], body["query"]);
        const auto check_prefix = [&](const nlohmann::json& kept) {
            EXPECT_FALSE(kept.empty());
            EXPECT_LT(kept.size(), rows.size());
            for (std::size_t i = 0; i < kept.size(); ++i) EXPECT_EQ(kept[i], rows[i]);
        };
        if (brave) {
            check_prefix(result["web"]["results"]);
            check_prefix(result["news"]["results"]);
        } else {
            check_prefix(result["results"]);
        }
        std::ifstream log(path);
        const std::string diagnostics{std::istreambuf_iterator<char>(log), {}};
        EXPECT_NE(diagnostics.find("[debug] " + std::string(brave ? "Brave" : "Tavily")
            + " search output truncated: original_bytes="), std::string::npos);
        EXPECT_EQ(diagnostics.find("[warning]"), std::string::npos);
        EXPECT_NE(diagnostics.find("byte_limit=32768"), std::string::npos);
        EXPECT_NE(diagnostics.find("article19"), std::string::npos); // Full raw response remains logged.
    }
}

TEST(WebSearchResults, DropsOversizedEntriesBeforeTrimmingSmallerResults) {
    for (bool brave : {true, false}) {
        SCOPED_TRACE(brave);
        auto small = nlohmann::json::array();
        for (int i = 0; i < 15; ++i)
            small.push_back({{"title", "Article " + std::to_string(i)},
                {"url", "https://example.org/" + std::to_string(i)},
                {"content", std::string(1000, 'x')}});
        auto rows = small;
        rows.insert(rows.begin(), nlohmann::json{{"title", "Oversized first result"},
            {"url", "https://example.org/huge"}, {"content", std::string(40000, 'x')}});
        // A second oversized entry must also be removed before trimming by rank.
        rows.insert(rows.begin() + 8, nlohmann::json{{"title", "Oversized middle result"},
            {"url", "https://example.org/huge2"}, {"content", std::string(40000, 'y')}});
        nlohmann::json body{{"results", rows}};
        if (brave) body = {{"web", body}, {"news", {{"results", {small[0]}}}}};
        MockHttpServer server({http_response("application/json", body.dump())});
        server.start();
        const auto endpoint = "http://127.0.0.1:" + std::to_string(server.port());
        const auto output = brave
            ? search_brave("query", "key", std::atomic_bool{false}, endpoint)
            : search_tavily("query", "key", std::atomic_bool{false}, endpoint);
        server.join();
        EXPECT_LE(output.size(), 32u * 1024);
        const auto result = nlohmann::json::parse(output);
        EXPECT_EQ(result.at("truncated"), true);
        EXPECT_EQ(brave ? result.at("web").at("results") : result.at("results"), small);
        if (brave) EXPECT_EQ(result.at("news"), body.at("news"));
    }
}

TEST(WebSearchResults, EnforcesExactByteBoundaryAndBoundsOversizedMetadata) {
    for (bool brave : {true, false}) {
        for (std::size_t size : {32u * 1024, 32u * 1024 + 1}) {
            SCOPED_TRACE(brave);
            SCOPED_TRACE(size);
            nlohmann::json body{{"results", nlohmann::json::array()}};
            if (brave) body = {{"web", body}};
            body["metadata"] = "";
            body["metadata"] = std::string(size - body.dump().size(), 'x');
            ASSERT_EQ(body.dump().size(), size);
            MockHttpServer server({http_response("application/json", body.dump())});
            server.start();
            const auto endpoint = "http://127.0.0.1:" + std::to_string(server.port());
            const auto output = brave
                ? search_brave("query", "key", std::atomic_bool{false}, endpoint)
                : search_tavily("query", "key", std::atomic_bool{false}, endpoint);
            server.join();
            EXPECT_LE(output.size(), 32u * 1024);
            const auto result = nlohmann::json::parse(output);
            if (size == 32u * 1024) {
                EXPECT_EQ(result, body);
            } else {
                EXPECT_EQ(result["truncated"], true);
                EXPECT_TRUE(result.contains("error"));
            }
        }
    }
}

TEST(WebSearchResults, RejectsExcessiveNestingBeforeCleanupOrSerialization) {
    for (bool brave : {true, false}) {
        for (bool arrays : {true, false}) {
            for (int depth : {63, 64, 100000}) {
                SCOPED_TRACE(brave);
                SCOPED_TRACE(arrays);
                SCOPED_TRACE(depth);
                std::string body = brave
                    ? R"({"web":{"results":[]},"metadata":)"
                    : R"({"results":[],"metadata":)";
                for (int i = 0; i < depth; ++i) body += arrays ? "[" : R"({"x":)";
                body += "0";
                body.append(depth, arrays ? ']' : '}');
                body += '}';
                ASSERT_LT(body.size(), 1024u * 1024);
                MockHttpServer server({http_response("application/json", body)});
                server.start();
                const auto endpoint = "http://127.0.0.1:" + std::to_string(server.port());
                const auto search = [&] {
                    return brave
                        ? search_brave("query", "key", std::atomic_bool{false}, endpoint)
                        : search_tavily("query", "key", std::atomic_bool{false}, endpoint);
                };
                if (depth == 63) {
                    EXPECT_EQ(search(), body);
                } else {
                    try {
                        search();
                        ADD_FAILURE() << "Expected the nesting limit to reject this response";
                    } catch (const std::runtime_error& error) {
                        EXPECT_STREQ(error.what(), "Web search response nesting limit exceeded");
                    }
                }
                server.join();
            }
        }
    }
}

TEST(TavilySearch, HandlesEmptyResultsAndLimitsQueriesToWholeWords) {
    std::atomic_bool cancelled{};
    for (const auto* word : {"query ", "café "}) {
        std::string query;
        for (int i = 0; i < 100; ++i) query += word;
        MockHttpServer server({http_response("application/json", R"({"results":[]})")});
        server.start();
        const auto result = nlohmann::json::parse(search_tavily(query, "key", cancelled,
            "http://127.0.0.1:" + std::to_string(server.port())));
        server.join();
        EXPECT_TRUE(result["results"].empty());
        // Each word plus its space is six bytes. Only 66 whole words fit.
        const auto expected = query.substr(0, 66 * 6 - 1);
        ASSERT_EQ(server.requests().size(), 1u);
        const auto& request = server.requests().front();
        const auto sent = nlohmann::json::parse(request.substr(request.find("\r\n\r\n") + 4));
        EXPECT_EQ(sent["query"], expected);
        EXPECT_LE(sent["query"].get<std::string>().size(), 400u);
    }
    for (const auto& query : {std::string(400, 'x'), std::string(400, 'x') + " more"}) {
        MockHttpServer server({http_response("application/json", R"({"results":[]})")});
        server.start();
        const auto result = nlohmann::json::parse(search_tavily(query, "key", cancelled,
            "http://127.0.0.1:" + std::to_string(server.port())));
        server.join();
        EXPECT_TRUE(result["results"].empty());
        ASSERT_EQ(server.requests().size(), 1u);
        const auto& request = server.requests().front();
        const auto sent = nlohmann::json::parse(request.substr(request.find("\r\n\r\n") + 4));
        EXPECT_EQ(sent["query"], std::string(400, 'x'));
        EXPECT_EQ(sent["query"].get<std::string>().size(), 400u);
    }
    EXPECT_THROW(search_tavily(std::string(401, 'x'), "key", cancelled), std::runtime_error);
    EXPECT_THROW(search_tavily(" \n", "key", cancelled), std::runtime_error);
    EXPECT_THROW(search_tavily("query", "", cancelled), std::runtime_error);
    EXPECT_THROW(search_tavily("query", "key\r\nInjected: true", cancelled), std::runtime_error);
    cancelled.store(true);
    EXPECT_TRUE(search_tavily("query", "key", cancelled).empty());
}

TEST(TavilySearch, RejectsMalformedResponsesAndHttpFailuresWithoutLoggingSecrets) {
    test::TestWorkspace fixture;
    const auto path = fixture.root() / "tavily-warnings.log";
    initialize_diagnostic_logging(path, "warn");
    std::atomic_bool cancelled{};
    for (const auto* body : {"private-response-body", "null", "{}",
        R"({"results":{}})", R"({"error":"private-response-body"})",
        R"({"detail":{"error":"private-response-body"}})"}) {
        MockHttpServer server({http_response("application/json", body)});
        server.start();
        EXPECT_THROW(search_tavily("private-query", "private-key", cancelled,
            "http://127.0.0.1:" + std::to_string(server.port())), std::runtime_error);
        server.join();
    }
    for (const int status : {302, 401, 429, 432, 500}) {
        const std::string body = "private-response-body";
        MockHttpServer server({"HTTP/1.1 " + std::to_string(status)
            + " Error\r\nLocation: http://127.0.0.1:1/\r\nContent-Length: "
            + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body});
        server.start();
        EXPECT_THROW(search_tavily("private-query", "private-key", cancelled,
            "http://127.0.0.1:" + std::to_string(server.port())), std::runtime_error);
        server.join();
    }
    shutdown_diagnostic_logging();
    std::ifstream log(path);
    const std::string warnings{std::istreambuf_iterator<char>(log), {}};
    for (const auto* reason : {"Web search HTTP 302", "Web search HTTP 401", "Web search HTTP 429",
        "Web search HTTP 432", "Web search HTTP 500", "Invalid web search response", "Invalid web search results"})
        EXPECT_NE(warnings.find(std::string("Tavily search failed: ") + reason), std::string::npos);
    for (const auto* secret : {"private-query", "private-key", "private-response-body"})
        EXPECT_EQ(warnings.find(secret), std::string::npos);
}

TEST(TavilySearch, CancelsAnInFlightTransfer) {
    MockHttpServer server({"HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\n"}, true);
    server.start();
    std::atomic_bool cancelled{};
    auto result = std::async(std::launch::async, [&] {
        return search_tavily("query", "key", cancelled,
            "http://127.0.0.1:" + std::to_string(server.port()));
    });
    const bool received = server.wait_for_requests(1, 2s);
    cancelled.store(true);
    EXPECT_TRUE(received);
    EXPECT_EQ(result.wait_for(1s), std::future_status::ready);
    EXPECT_TRUE(result.get().empty());
    server.join();
}

TEST(WebSearchContext, ConcurrentRecipientsShareOneSearch) {
    WebSearchContext context;
    context.query.run.prompt_text = "latest news";
    std::atomic_bool cancelled{};
    std::atomic_int calls{};
    std::promise<void> entered;
    std::promise<void> release;
    auto ready = release.get_future().share();
    WebSearchExecutor search = [&](const auto&, auto query, const auto&) {
        EXPECT_EQ(query, "latest news");
        ++calls;
        entered.set_value();
        ready.wait();
        return "source context";
    };
    auto first = std::async(std::launch::async, [&] { return context.get({}, search, cancelled); });
    entered.get_future().wait();
    auto second = std::async(std::launch::async, [&] { return context.get({}, search, cancelled); });
    release.set_value();
    EXPECT_EQ(first.get(), "source context");
    EXPECT_EQ(second.get(), "source context");
    EXPECT_EQ(calls, 1);
}

TEST(WebSearchContext, FailedSearchIsNotRepeatedByAnotherRecipient) {
    WebSearchContext context;
    std::atomic_bool cancelled{};
    int calls{};
    WebSearchExecutor search = [&](const auto&, auto, const auto&) -> std::string {
        ++calls;
        throw std::runtime_error("private credential error");
    };
    EXPECT_TRUE(context.get({}, search, cancelled).empty());
    EXPECT_TRUE(context.get({}, search, cancelled).empty());
    EXPECT_EQ(calls, 1);
}

TEST(WebSearchContext, EmptyOrFailedRewriteDoesNotSearch) {
    struct Backend : ModelBackend {
        explicit Backend(bool fail) : fail(fail) {}
        bool fail;
        RequestPayload prepare(const GenerationRequest&) override { return {}; }
        GenerationResult perform(RequestPayload, const GenerationDeltaSink& sink,
            const std::atomic_bool&) override {
            sink({GenerationDeltaKind::reasoning, "not a search query"});
            sink({GenerationDeltaKind::answer, fail ? "partial query" : "  \n"});
            return {fail ? GenerationOutcome::transport_error : GenerationOutcome::completed};
        }
    };
    for (const bool fail : {false, true}) {
        WebSearchContext context;
        context.rewriter = std::make_shared<CharacterDefinition>();
        std::atomic_bool cancelled{};
        int calls{};
        EXPECT_TRUE(context.get([=](auto) { return std::make_unique<Backend>(fail); },
            [&](const auto&, auto, const auto&) { ++calls; return "unexpected"; }, cancelled).empty());
        EXPECT_EQ(calls, 0);
    }
}

TEST(WebSearchContext, LongStreamedRewriteStillSearchesWithWholeWords) {
    struct Backend : ModelBackend {
        RequestPayload prepare(const GenerationRequest&) override { return {}; }
        GenerationResult perform(RequestPayload, const GenerationDeltaSink& sink,
            const std::atomic_bool&) override {
            sink({GenerationDeltaKind::answer, "  \n"});
            for (int i = 0; i < 2000; ++i)
                sink({GenerationDeltaKind::answer, "  café"});
            return {};
        }
    };
    MockHttpServer server({http_response("application/json", R"({"web":{"results":[]}})")});
    server.start();
    WebSearchContext context;
    context.rewriter = std::make_shared<CharacterDefinition>();
    std::atomic_bool cancelled{};
    const auto result = context.get([](auto) { return std::make_unique<Backend>(); },
        [&](const auto&, auto query, const auto& cancel) {
            return search_brave(query, "key", cancel,
                "http://127.0.0.1:" + std::to_string(server.port()));
        }, cancelled);
    server.join();
    ASSERT_FALSE(result.empty());
    std::string encoded;
    for (int i = 0; i < 75; ++i) {
        if (i > 0) encoded += "%20%20";
        encoded += "caf%C3%A9";
    }
    EXPECT_EQ(nlohmann::json::parse(result), nlohmann::json::parse(R"({"web":{"results":[]}})"));
    ASSERT_EQ(server.requests().size(), 1u);
    EXPECT_TRUE(server.requests().front().starts_with("GET /?q=" + encoded
            + "&count=10&extra_snippets=true&text_decorations=false&result_filter=web,news,discussions,faq,infobox,query HTTP/1.1\r\n"));
}

} // namespace
} // namespace cha
