#include "providers/web_search.h"

#include "util/curl.h"
#include "util/logging.h"
#include "util/text.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace cha {
namespace {

[[noreturn]] void fail_search(std::string_view provider, std::string message) {
    // Only locally constructed diagnostics belong here, never response bodies,
    // credentials, queries, or exceptions from other providers.
    log_warn(std::string(provider) + " search failed: " + message);
    throw std::runtime_error(std::move(message));
}

std::string_view limit_query(std::string_view query, std::size_t byte_limit) {
    query = trim_view(query);
    constexpr std::string_view whitespace = " \t\r\n\f\v";
    std::size_t end = 0;
    std::size_t start = 0;
    for (int words = 0; words < 75 && start < query.size(); ++words) {
        auto word_end = query.find_first_of(whitespace, start);
        if (word_end == std::string_view::npos) word_end = query.size();
        if (word_end > byte_limit) break;
        end = word_end;
        start = query.find_first_not_of(whitespace, end);
    }
    // An ASCII whitespace boundary cannot split a UTF-8 character. A first
    // word longer than the byte limit leaves no usable query.
    return query.substr(0, end);
}

nlohmann::ordered_json prefer_distinct_hosts(nlohmann::ordered_json sources) {
    auto selected = nlohmann::ordered_json::array();
    auto skipped = nlohmann::ordered_json::array();
    std::unordered_set<std::string> hosts;
    const std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), curl_url_cleanup);
    if (!parsed) throw std::runtime_error("Could not parse search result URLs");
    for (auto& source : sources) {
        const auto& url = source["url"].get_ref<const std::string&>();
        if (url.find('\0') != std::string::npos
            || curl_url_set(parsed.get(), CURLUPART_URL, url.c_str(), 0) != CURLUE_OK) continue;
        char* raw_host = nullptr;
        if (curl_url_get(parsed.get(), CURLUPART_HOST, &raw_host, 0) != CURLUE_OK) continue;
        const std::unique_ptr<char, decltype(&curl_free)> owned_host(raw_host, curl_free);
        auto host = fold_ascii(raw_host);
        if (host.ends_with('.')) host.pop_back();
        if (host.starts_with("www.")) host.erase(0, 4);
        if (hosts.insert(std::move(host)).second) {
            selected.push_back(std::move(source));
            if (selected.size() == 5) break;
        } else {
            skipped.push_back(std::move(source));
        }
    }
    // Fill unused slots from repeated hosts, preserving their original ranking.
    for (auto& source : skipped) {
        if (selected.size() == 5) break;
        selected.push_back(std::move(source));
    }
    return selected;
}

nlohmann::json request_search(std::string_view provider, CurlHandle& curl,
    const CurlHeaders& headers, const std::string& url, const std::string& body,
    const std::atomic_bool& cancelled) {
    std::string response;
    const auto require = [provider](CURLcode code) {
        if (code != CURLE_OK) fail_search(provider, "Could not configure web search transport");
    };
    require(curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str()));
    require(curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get()));
    if (!body.empty()) {
        require(curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str()));
        require(curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size())));
    }
    require(curl_easy_setopt(curl.get(), CURLOPT_ACCEPT_ENCODING, ""));
    require(curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L));
    require(curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, 10000L));
    require(curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L));
    require(curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION,
        +[](char* data, size_t size, size_t count, void* user) -> size_t {
            auto& output = *static_cast<std::string*>(user);
            const auto bytes = size * count;
            if (bytes > 1024 * 1024 - output.size()) return 0;
            try { output.append(data, bytes); } catch (...) { return 0; }
            return bytes;
        }));
    require(curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response));

    const std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> multi(curl_multi_init(), curl_multi_cleanup);
    if (!multi) fail_search(provider, "Could not create web search transfer");
    const auto require_multi = [provider](CURLMcode code) {
        if (code != CURLM_OK) fail_search(provider, "Web search transfer failed");
    };
    require_multi(curl_multi_add_handle(multi.get(), curl.get()));
    const auto remove = [&](CURL* handle) { curl_multi_remove_handle(multi.get(), handle); };
    const std::unique_ptr<CURL, decltype(remove)> attached(curl.get(), remove);
    int running{};
    do {
        if (cancelled.load()) return {};
        require_multi(curl_multi_perform(multi.get(), &running));
        if (running) require_multi(curl_multi_poll(multi.get(), nullptr, 0, 100, nullptr));
    } while (running);
    if (cancelled.load()) return {};
    int messages{};
    const auto* completed = curl_multi_info_read(multi.get(), &messages);
    if (!completed || completed->msg != CURLMSG_DONE)
        fail_search(provider, "Web search transfer returned no result");
    if (completed->data.result != CURLE_OK)
        fail_search(provider, "Web search connection failed: " + std::string(curl_easy_strerror(completed->data.result)));
    long status{};
    require(curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status));
    if (status != 200)
        fail_search(provider, "Web search HTTP " + std::to_string(status));

    const auto parsed = nlohmann::json::parse(response, nullptr, false);
    if (!parsed.is_object() || parsed.contains("error"))
        fail_search(provider, "Invalid web search response");
    return parsed;
}

} // namespace

std::string search_brave(std::string_view query, std::string_view key,
    const std::atomic_bool& cancelled, std::string_view endpoint) {
    if (cancelled.load()) return {};
    query = limit_query(query, 600);
    if (query.empty())
        fail_search("Brave", "Web search query is empty after the length limit");
    if (key.empty() || key.find_first_of("\r\n") != std::string_view::npos)
        fail_search("Brave", "Brave API key is missing or invalid");

    CurlHandle curl;
    CurlHeaders headers;
    headers.append("Accept: application/json");
    headers.append("X-Subscription-Token: " + std::string(key));
    const std::unique_ptr<char, decltype(&curl_free)> encoded(
        curl_easy_escape(curl.get(), query.data(), static_cast<int>(query.size())), curl_free);
    if (!encoded) fail_search("Brave", "Could not encode web search query");
    constexpr std::string_view reduce_commentary =
        "/opinion/$discard\n"
        "/opinions/$discard\n"
        "/editorial/$discard\n"
        "/editorials/$discard\n"
        "/commentary/$discard\n"
        "/columnists/$discard\n"
        "/commentisfree/$discard\n"
        "/op-ed/$discard\n"
        "/oped/$discard\n"
        ".gov/$boost=2";
    const std::unique_ptr<char, decltype(&curl_free)> encoded_goggles(
        curl_easy_escape(curl.get(), reduce_commentary.data(), static_cast<int>(reduce_commentary.size())),
        curl_free);
    if (!encoded_goggles) fail_search("Brave", "Could not encode web search goggles");
    const std::string url = std::string(endpoint) + "?q=" + encoded.get()
        + "&count=10&result_filter=web&text_decorations=false&extra_snippets=true"
        + "&goggles=" + encoded_goggles.get();
    const auto parsed = request_search("Brave", curl, headers, url, {}, cancelled);
    if (parsed.is_null()) return {};
    nlohmann::ordered_json sources = nlohmann::ordered_json::array();
    if (parsed.contains("web")) {
        const auto& web = parsed.at("web");
        if (!web.is_object() || !web.contains("results") || !web.at("results").is_array())
            fail_search("Brave", "Invalid web search results");
        const auto& results = web.at("results");
        for (const auto& result : results) {
            if (!result.is_object() || !result.contains("url") || !result["url"].is_string()) continue;
            const auto url = result["url"].get<std::string>();
            if (!url.starts_with("https://") && !url.starts_with("http://")) continue;
            nlohmann::ordered_json source = {{"url", url}};
            if (result.contains("title") && result["title"].is_string()) source["title"] = result["title"];
            std::vector<std::string> snippets;
            const auto add_snippet = [&](const nlohmann::json& value) {
                if (!value.is_string()) return;
                const auto& text = value.get_ref<const std::string&>();
                if (!trim_view(text).empty() && std::find(snippets.begin(), snippets.end(), text) == snippets.end())
                    snippets.push_back(text);
            };
            if (result.contains("description")) add_snippet(result["description"]);
            const auto snippet_limit = snippets.size() + 5;
            if (result.contains("extra_snippets") && result["extra_snippets"].is_array()) {
                for (const auto& snippet : result["extra_snippets"]) {
                    add_snippet(snippet);
                    if (snippets.size() == snippet_limit) break;
                }
            }
            source["snippets"] = std::move(snippets);
            sources.push_back(std::move(source));
        }
    }
    return nlohmann::ordered_json{{"query", query}, {"results", prefer_distinct_hosts(std::move(sources))}}.dump();
}

std::string search_tavily(std::string_view query, std::string_view key,
    const std::atomic_bool& cancelled, std::string_view endpoint) {
    if (cancelled.load()) return {};
    query = limit_query(query, 400);
    if (query.empty())
        fail_search("Tavily", "Web search query is empty after the length limit");
    if (key.empty() || key.find_first_of("\r\n") != std::string_view::npos)
        fail_search("Tavily", "Tavily API key is missing or invalid");

    CurlHandle curl;
    CurlHeaders headers;
    headers.append("Accept: application/json");
    headers.append("Content-Type: application/json");
    headers.append("Authorization: Bearer " + std::string(key));
    const auto body = nlohmann::json{{"query", query}, {"search_depth", "basic"},
        {"max_results", 10}, {"include_answer", false}, {"include_raw_content", false}}.dump();
    const auto parsed = request_search("Tavily", curl, headers, std::string(endpoint), body, cancelled);
    if (parsed.is_null()) return {};
    if (!parsed.contains("results") || !parsed["results"].is_array())
        fail_search("Tavily", "Invalid web search results");

    nlohmann::ordered_json sources = nlohmann::ordered_json::array();
    for (const auto& result : parsed["results"]) {
        if (!result.is_object() || !result.contains("url") || !result["url"].is_string()) continue;
        const auto url = result["url"].get<std::string>();
        if (!url.starts_with("https://") && !url.starts_with("http://")) continue;
        nlohmann::ordered_json source = {{"url", url}};
        if (result.contains("title") && result["title"].is_string()) source["title"] = result["title"];
        if (result.contains("content") && result["content"].is_string()) source["description"] = result["content"];
        sources.push_back(std::move(source));
    }
    return nlohmann::ordered_json{{"query", query}, {"results", prefer_distinct_hosts(std::move(sources))}}.dump();
}

const std::string& WebSearchContext::get(const ProviderClientFactory& factory,
    const WebSearchExecutor& search, const std::atomic_bool& cancelled) {
    std::call_once(once_, [&] {
        try {
            if (cancelled.load()) return;
            if (!search) throw std::runtime_error("Web search executor unavailable");
            std::string text = query.run.prompt_text;
            if (rewriter) {
                auto backend = factory(rewriter);
                if (!backend) throw std::runtime_error("Query provider unavailable");
                std::string rewritten;
                const auto result = backend->perform(backend->prepare(query),
                    [&](GenerationDelta delta) {
                        if (delta.kind == GenerationDeltaKind::answer) {
                            if (rewritten.empty())
                                delta.text.erase(0, delta.text.find_first_not_of(" \t\r\n\f\v"));
                            // Keep one byte past the largest provider limit (600 bytes)
                            // so limit_query can distinguish a complete last word from a cut one.
                            rewritten.append(delta.text, 0, 601 - rewritten.size());
                        }
                    }, cancelled);
                if (cancelled.load() || result.outcome == GenerationOutcome::cancelled) return;
                if (result.outcome != GenerationOutcome::completed || trim_view(rewritten).empty())
                    throw std::runtime_error("Web search query rewrite failed");
                text = trim_view(rewritten);
            }
            if (!cancelled.load()) {
                log_info("Web search initiated: trigger=jev query_bytes=" + std::to_string(text.size()));
                context_ = search(config, text, cancelled);
                if (!cancelled.load()) {
                    log_info("Web search completed: trigger=jev query_bytes=" + std::to_string(text.size())
                        + " result_bytes=" + std::to_string(context_.size()));
                }
            }
        } catch (...) {
            // Provider errors and credential lookups can contain secrets.
            if (!cancelled.load()) log_warn("Web search failed; continuing without search results");
        }
    });
    return context_;
}

} // namespace cha
