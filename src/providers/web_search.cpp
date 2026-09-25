#include "providers/web_search.h"

#include "util/curl.h"
#include "util/logging.h"
#include "util/text.h"

#include <nlohmann/json.hpp>

#include <stdexcept>

namespace cha {
namespace {

[[noreturn]] void fail_brave(std::string message) {
    // Only locally constructed diagnostics belong here, never response bodies,
    // credentials, queries, or exceptions from other providers.
    log_warn("Brave search failed: " + message);
    throw std::runtime_error(std::move(message));
}

std::string_view limit_query(std::string_view query) {
    query = trim_view(query);
    constexpr std::string_view whitespace = " \t\r\n\f\v";
    std::size_t end = 0;
    std::size_t start = 0;
    for (int words = 0; words < 75 && start < query.size(); ++words) {
        auto word_end = query.find_first_of(whitespace, start);
        if (word_end == std::string_view::npos) word_end = query.size();
        if (word_end > 600) break;
        end = word_end;
        start = query.find_first_not_of(whitespace, end);
    }
    // An ASCII whitespace boundary cannot split a UTF-8 character. A first
    // word longer than the byte limit leaves no usable query.
    return query.substr(0, end);
}

} // namespace

std::string search_brave(std::string_view query, std::string_view key,
    const std::atomic_bool& cancelled, std::string_view endpoint) {
    if (cancelled.load()) return {};
    query = limit_query(query);
    if (query.empty())
        fail_brave("Web search query is empty after the length limit");
    if (key.empty() || key.find_first_of("\r\n") != std::string_view::npos)
        fail_brave("Brave API key is missing or invalid");

    CurlHandle curl;
    CurlHeaders headers;
    headers.append("Accept: application/json");
    headers.append("X-Subscription-Token: " + std::string(key));
    const std::unique_ptr<char, decltype(&curl_free)> encoded(
        curl_easy_escape(curl.get(), query.data(), static_cast<int>(query.size())), curl_free);
    if (!encoded) fail_brave("Could not encode web search query");
    const std::string url = std::string(endpoint) + "?q=" + encoded.get()
        + "&count=5&result_filter=web&text_decorations=false&extra_snippets=true";
    std::string response;
    const auto require = [](CURLcode code) {
        if (code != CURLE_OK) fail_brave("Could not configure web search transport");
    };
    require(curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str()));
    require(curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get()));
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
    if (!multi) fail_brave("Could not create web search transfer");
    const auto require_multi = [](CURLMcode code) {
        if (code != CURLM_OK) fail_brave("Web search transfer failed");
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
        fail_brave("Web search transfer returned no result");
    if (completed->data.result != CURLE_OK)
        fail_brave("Web search connection failed: " + std::string(curl_easy_strerror(completed->data.result)));
    long status{};
    require(curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status));
    if (status != 200)
        fail_brave("Web search HTTP " + std::to_string(status));

    const auto parsed = nlohmann::json::parse(response, nullptr, false);
    if (!parsed.is_object() || parsed.contains("error"))
        fail_brave("Invalid web search response");
    nlohmann::ordered_json sources = nlohmann::ordered_json::array();
    if (parsed.contains("web")) {
        const auto& web = parsed.at("web");
        if (!web.is_object() || !web.contains("results") || !web.at("results").is_array())
            fail_brave("Invalid web search results");
        const auto& results = web.at("results");
        for (const auto& result : results) {
            if (!result.is_object() || !result.contains("url") || !result["url"].is_string()) continue;
            const auto url = result["url"].get<std::string>();
            if (!url.starts_with("https://") && !url.starts_with("http://")) continue;
            nlohmann::ordered_json source = {{"url", url}};
            for (const auto* field : {"title", "description"}) {
                if (result.contains(field) && result[field].is_string()) source[field] = result[field];
            }
            if (result.contains("extra_snippets") && result["extra_snippets"].is_array()) {
                for (const auto& snippet : result["extra_snippets"]) {
                    if (snippet.is_string()) source["extra_snippets"].push_back(snippet.get<std::string>());
                    if (source.contains("extra_snippets") && source["extra_snippets"].size() == 5) break;
                }
            }
            sources.push_back(std::move(source));
            if (sources.size() == 5) break;
        }
    }
    return nlohmann::ordered_json{{"query", query}, {"results", std::move(sources)}}.dump();
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
                            // Keep one byte past the limit so search_brave can
                            // distinguish a complete last word from a cut one.
                            rewritten.append(delta.text, 0, 601 - rewritten.size());
                        }
                    }, cancelled);
                if (cancelled.load() || result.outcome == GenerationOutcome::cancelled) return;
                if (result.outcome != GenerationOutcome::completed || trim_view(rewritten).empty())
                    throw std::runtime_error("Web search query rewrite failed");
                text = trim_view(rewritten);
            }
            if (!cancelled.load()) context_ = search(config, text, cancelled);
        } catch (...) {
            // Provider errors and credential lookups can contain secrets.
            if (!cancelled.load()) log_warn("Web search failed; continuing without search results");
        }
    });
    return context_;
}

} // namespace cha
