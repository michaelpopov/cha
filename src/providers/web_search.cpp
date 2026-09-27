#include "providers/web_search.h"

#include "util/curl.h"
#include "util/logging.h"
#include "util/text.h"

#include <nlohmann/json.hpp>

#include <stdexcept>

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

void remove_search_media(nlohmann::ordered_json& value) {
    if (value.is_object()) {
        for (const auto* field : {"image", "images", "img", "video", "videos", "audio",
                 "thumbnail", "thumbnails", "favicon", "logo", "icons", "pictures", "schemas"})
            value.erase(field);
    }
    if (value.is_object() || value.is_array()) {
        for (auto& child : value) remove_search_media(child);
    }
}

std::string bounded_search_output(std::string_view provider, nlohmann::ordered_json response) {
    constexpr std::size_t byte_limit = 32 * 1024;
    // Web results can still carry thumbnails and other media metadata.
    remove_search_media(response);
    std::string output = response.dump();
    if (output.size() <= byte_limit) return output;
    const auto original_bytes = output.size();
    response["truncated"] = true;
    response["truncation_reason"] = "Search output size limit; oversized or lower-ranked results were omitted.";
    do {
        nlohmann::ordered_json* largest = nullptr;
        std::size_t largest_bytes = 0;
        const auto consider = [&](nlohmann::ordered_json& category) {
            if (!category.is_object() || !category.contains("results")) return;
            auto& results = category["results"];
            if (!results.is_array() || results.empty()) return;
            // An entry that cannot fit on its own must not displace smaller results.
            for (auto it = results.begin(); it != results.end();) {
                if (it->dump().size() > byte_limit) it = results.erase(it);
                else ++it;
            }
            if (results.empty()) return;
            const auto bytes = results.dump().size();
            if (bytes > largest_bytes) {
                largest = &results;
                largest_bytes = bytes;
            }
        };
        consider(response); // Tavily's top-level results.
        for (auto& category : response) consider(category); // Brave's categories.
        output = response.dump();
        if (output.size() <= byte_limit) break;
        if (!largest) {
            // Metadata alone can exceed the limit. Do not pass broken JSON or
            // silently present an empty result set as a successful search.
            response = {{"truncated", true}, {"error",
                "Search response exceeded the output size limit. Try a more specific query."}};
        } else {
            largest->erase(largest->end() - 1);
        }
        output = response.dump();
    } while (output.size() > byte_limit);
    log_debug(std::string(provider) + " search output truncated: original_bytes="
        + std::to_string(original_bytes) + " output_bytes=" + std::to_string(output.size())
        + " byte_limit=" + std::to_string(byte_limit));
    return output;
}

nlohmann::ordered_json request_search(std::string_view provider, CurlHandle& curl,
    const CurlHeaders& headers, const std::string& url, const std::string& body,
    const std::atomic_bool& cancelled, std::string_view key) {
    const auto started = std::chrono::steady_clock::now();
    log_debug_payload(std::string(provider) + " search request URL", url, key);
    log_debug_payload(std::string(provider) + " search request body", body, key);
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
    long status{};
    require(curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status));
    log_debug(std::string(provider) + " search response: status=" + std::to_string(status)
        + " curl_code=" + std::to_string(completed->data.result)
        + " duration_ms=" + std::to_string(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count())
        + " response_bytes=" + std::to_string(response.size()));
    log_debug_payload(std::string(provider) + " search raw response", response, key);
    if (completed->data.result != CURLE_OK)
        fail_search(provider, "Web search connection failed: " + std::string(curl_easy_strerror(completed->data.result)));
    if (status != 200)
        fail_search(provider, "Web search HTTP " + std::to_string(status));

    // Bound depth while parsing, before recursive cleanup and serialization.
    const auto check_depth = [provider](int depth, nlohmann::ordered_json::parse_event_t,
        nlohmann::ordered_json&) {
        if (depth > 64) fail_search(provider, "Web search response nesting limit exceeded");
        return true;
    };
    const auto parsed = nlohmann::ordered_json::parse(response, check_depth, false);
    if (!parsed.is_object() || parsed.contains("error"))
        fail_search(provider, "Invalid web search response");
    return parsed;
}

} // namespace

std::string search_brave(std::string_view query, std::string_view key,
    const std::atomic_bool& cancelled, std::string_view endpoint) {
    if (cancelled.load()) return {};
    log_debug_payload("Brave search original query", query, key);
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
    const std::string url = std::string(endpoint) + "?q=" + encoded.get()
        + "&count=10&extra_snippets=true&text_decorations=false&result_filter=web,news,discussions,faq,infobox,query";
    log_debug_payload("Brave search effective query", query, key);
    const auto parsed = request_search("Brave", curl, headers, url, {}, cancelled, key);
    if (parsed.is_null()) return {};
    if (parsed.contains("web")) {
        const auto& web = parsed.at("web");
        if (!web.is_object() || !web.contains("results") || !web.at("results").is_array())
            fail_search("Brave", "Invalid web search results");
    }
    return bounded_search_output("Brave", parsed);
}

std::string search_tavily(std::string_view query, std::string_view key,
    const std::atomic_bool& cancelled, std::string_view endpoint) {
    if (cancelled.load()) return {};
    log_debug_payload("Tavily search original query", query, key);
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
    const auto body = nlohmann::json{{"query", query}, {"max_results", 10},
        {"include_images", false}, {"include_image_descriptions", false},
        {"include_favicon", false}}.dump();
    const auto parsed = request_search("Tavily", curl, headers, std::string(endpoint), body, cancelled, key);
    if (parsed.is_null()) return {};
    if (!parsed.contains("results") || !parsed["results"].is_array())
        fail_search("Tavily", "Invalid web search results");

    return bounded_search_output("Tavily", parsed);
}

const std::string& WebSearchContext::get(const ProviderClientFactory& factory,
    const WebSearchExecutor& search, const std::atomic_bool& cancelled) {
    std::call_once(once_, [&] {
        try {
            if (cancelled.load()) return;
            if (!search) throw std::runtime_error("Web search executor unavailable");
            std::string text = query.run.prompt_text;
            log_debug("Jev search preparation: forum_id=" + query.run.session.forum_id
                + " session_id=" + query.run.session.session_id
                + " rewrite=" + (rewriter ? "true" : "false"));
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
