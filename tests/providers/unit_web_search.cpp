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

TEST(BraveSearch, SendsEncodedQueryAndCommentaryGogglesAndKeyAndExtractsSourceData) {
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
        "GET /res/v1/web/search?q=C%2B%2B%20%26%20caf%C3%A9%3F"
        "&count=10&result_filter=web&text_decorations=false&extra_snippets=true"
        "&goggles=%2Fopinion%2F%24discard%0A%2Fopinions%2F%24discard%0A"
        "%2Feditorial%2F%24discard%0A%2Feditorials%2F%24discard%0A"
        "%2Fcommentary%2F%24discard%0A%2Fcolumnists%2F%24discard%0A"
        "%2Fcommentisfree%2F%24discard%0A%2Fop-ed%2F%24discard%0A"
        "%2Foped%2F%24discard%0A.gov%2F%24boost%3D2 HTTP/1.1\r\n"));
    EXPECT_NE(request.find("X-Subscription-Token: test-secret\r\n"), std::string::npos);
    EXPECT_EQ(result["query"], "C++ & café?");
    ASSERT_EQ(result["results"].size(), 2u);
    EXPECT_EQ(result["results"][0]["title"], "A source");
    EXPECT_EQ(result["results"][0]["url"], "https://example.org/a");
    EXPECT_EQ(result["results"][0]["snippets"], nlohmann::json::array({"A summary", "More context"}));
    EXPECT_FALSE(result["results"][0].contains("description"));
    EXPECT_FALSE(result["results"][0].contains("extra_snippets"));
    EXPECT_FALSE(result["results"][0].contains("unused"));
    EXPECT_EQ(result["results"][1]["snippets"], nlohmann::json::array());
}

TEST(BraveSearch, KeepsFiveDistinctExtraSnippetsPerSource) {
    MockHttpServer server({http_response("application/json", R"json({"web":{"results":[
        {"url":"https://example.org/a","description":"Main excerpt",
         "extra_snippets":["Main excerpt","One","One","Two","Three","Four","Five","Six"]},
        {"url":"https://example.org/b","description":123,
         "extra_snippets":["One","Two","Three","Four","Five","Six"]},
        {"url":"https://example.org/c","description":" \n\t","extra_snippets":null}
    ]}})json")});
    server.start();
    std::atomic_bool cancelled{};
    const auto result = nlohmann::json::parse(search_brave("query", "key", cancelled,
        "http://127.0.0.1:" + std::to_string(server.port())));
    server.join();
    ASSERT_EQ(result["results"].size(), 3u);
    EXPECT_EQ(result["results"][0]["snippets"],
        nlohmann::json::array({"Main excerpt", "One", "Two", "Three", "Four", "Five"}));
    EXPECT_EQ(result["results"][1]["snippets"],
        nlohmann::json::array({"One", "Two", "Three", "Four", "Five"}));
    EXPECT_EQ(result["results"][2]["snippets"], nlohmann::json::array());
}

TEST(BraveSearch, HandlesEmptyResultsAndRejectsMalformedAndHttpFailures) {
    std::atomic_bool cancelled{};
    for (const auto* body : {R"({"query":{}})", R"({"web":{"results":[]}})"}) {
        MockHttpServer server({http_response("application/json", body)});
        server.start();
        const auto result = nlohmann::json::parse(search_brave("query", "key", cancelled,
            "http://127.0.0.1:" + std::to_string(server.port())));
        server.join();
        EXPECT_TRUE(result["results"].empty());
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
        std::string expected;
        for (int i = 0; i < 100; ++i) {
            if (i > 0) query += ' ';
            query += word;
            if (i < retained_words) expected = query;
        }
        MockHttpServer server({http_response("application/json", R"({"web":{"results":[]}})")});
        server.start();
        std::atomic_bool cancelled{};
        const auto result = nlohmann::json::parse(search_brave(query, "key", cancelled,
            "http://127.0.0.1:" + std::to_string(server.port())));
        server.join();
        EXPECT_EQ(result["query"], expected);
        EXPECT_LE(result["query"].get<std::string>().size(), 600u);
        std::string encoded;
        for (int i = 0; i < retained_words; ++i) {
            if (i > 0) encoded += "%20";
            encoded += word == "word" ? "word" : std::string(198, 'x') + "%C3%A9";
        }
        ASSERT_EQ(server.requests().size(), 1u);
        EXPECT_TRUE(server.requests().front().starts_with("GET /?q=" + encoded + "&count=10"));
    }
    std::atomic_bool cancelled{};
    EXPECT_THROW(search_brave(std::string(601, 'x'), "key", cancelled,
        "http://127.0.0.1:1"), std::runtime_error);
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

TEST(TavilySearch, PostsQueryAndKeyAndExtractsAtMostFiveSources) {
    const std::string body = R"json({"results":[
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
    EXPECT_EQ(sent, (nlohmann::json{{"query", query}, {"search_depth", "basic"},
        {"max_results", 10}, {"include_answer", false}, {"include_raw_content", false}}));
    EXPECT_EQ(result["query"], query);
    ASSERT_EQ(result["results"].size(), 5u);
    EXPECT_EQ(result["results"][0], (nlohmann::json{{"title", "A source"},
        {"url", "https://example.org/a"}, {"description", "A summary"}}));
    EXPECT_EQ(result["results"][1], (nlohmann::json{{"url", "http://example.org/b"}}));
    EXPECT_EQ(result["results"][4]["url"], "https://example.org/e");
}

TEST(WebSearchResults, PrefersDistinctHostsAndFillsFromOriginalRankingForBothProviders) {
    struct Case {
        std::vector<std::string> urls;
        std::vector<std::size_t> selected;
    };
    const std::vector<Case> cases{
        {{"https://example.org/first", "https://www.example.org/second",
          "http://EXAMPLE.ORG:8080/third", "https://research.example.org/paper",
          "https://other.org/first", "https://example.org/fourth",
          "https://third.org/first", "https://fourth.org/first",
          "https://fifth.org/first", "https://sixth.org/first"}, {0, 3, 4, 6, 7}},
        {{"https://example.org/first", "https://www.example.org/second",
          "https://other.org/first", "https://EXAMPLE.ORG.:443/third",
          "https://other.org/second", "https://example.org/fourth"}, {0, 2, 1, 3, 4}},
        {{"https://example.org/first", "https://example.org/second",
          "https://example.org/third"}, {0, 1, 2}},
        {{"https://[broken", "https://bad host.org/page",
          std::string("https://hidden.example/") + '\0' + "suffix",
          "https://example.org/valid", "https://other.org/valid"}, {3, 4}},
    };
    for (const bool brave : {true, false}) {
        SCOPED_TRACE(brave ? "Brave" : "Tavily");
        for (std::size_t index = 0; index < cases.size(); ++index) {
            SCOPED_TRACE(index);
            const auto& test = cases[index];
            auto rows = nlohmann::json::array();
            for (const auto& url : test.urls) rows.push_back({{"url", url}, {"title", url}});
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
            ASSERT_EQ(result["results"].size(), test.selected.size());
            for (std::size_t i = 0; i < test.selected.size(); ++i) {
                EXPECT_EQ(result["results"][i]["url"], test.urls[test.selected[i]]);
                EXPECT_EQ(result["results"][i]["title"], test.urls[test.selected[i]]);
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
        EXPECT_EQ(result["query"], expected);
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
        EXPECT_EQ(result["query"], std::string(400, 'x'));
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
    std::string expected;
    for (int i = 0; i < 75; ++i) {
        if (i > 0) expected += "  ";
        expected += "café";
    }
    EXPECT_EQ(nlohmann::json::parse(result)["query"], expected);
    EXPECT_EQ(server.requests().size(), 1u);
}

} // namespace
} // namespace cha
