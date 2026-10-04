#include "providers/web_search.h"

#include "util/curl.h"
#include "util/logging.h"
#include "util/text.h"

#include <nlohmann/json.hpp>

#include <stdexcept>

namespace cha {
namespace {

[[noreturn]] void fail_web(std::string_view provider, std::string_view subject, std::string message) {
    // Only locally constructed diagnostics belong here, never response bodies,
    // credentials, queries, or exceptions from other providers.
    log_warn(std::string(provider) + " " + fold_ascii(subject) + " failed: " + message);
    throw WebToolError(std::move(message));
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

nlohmann::ordered_json request_web_json(std::string_view provider, CurlHandle& curl,
    const CurlHeaders& headers, const std::string& url, const std::string& body,
    const std::atomic_bool& cancelled, std::string_view key,
    std::string subject, long timeout_ms = 10000L,
    std::size_t max_bytes = 1024 * 1024) {
    const auto lower_subject = fold_ascii(subject);
    const std::string label = std::string(provider) + " " + lower_subject;
    const auto started = std::chrono::steady_clock::now();
    log_debug_payload(label + " request URL", url, key);
    log_debug_payload(label + " request body", body, key);
    struct ResponseBuffer {
        std::string text;
        std::size_t max_bytes;
        bool too_large{false};
    } buffer{{}, max_bytes};
    auto& response = buffer.text;
    const auto require = [&](CURLcode code) {
        if (code != CURLE_OK) fail_web(provider, subject, "Could not configure " + lower_subject + " transport");
    };
    require(curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str()));
    require(curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get()));
    if (!body.empty()) {
        require(curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str()));
        require(curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size())));
    }
    require(curl_easy_setopt(curl.get(), CURLOPT_ACCEPT_ENCODING, ""));
    require(curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L));
    require(curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT_MS, timeout_ms));
    require(curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L));
    require(curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION,
        +[](char* data, size_t size, size_t count, void* user) -> size_t {
            auto& received = *static_cast<ResponseBuffer*>(user);
            auto& output = received.text;
            const auto bytes = size * count;
            if (bytes > received.max_bytes - output.size()) {
                received.too_large = true;
                return 0;
            }
            try { output.append(data, bytes); } catch (...) { return 0; }
            return bytes;
        }));
    require(curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &buffer));

    const std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> multi(curl_multi_init(), curl_multi_cleanup);
    if (!multi) fail_web(provider, subject, "Could not create " + lower_subject + " transfer");
    const auto require_multi = [&](CURLMcode code) {
        if (code != CURLM_OK) fail_web(provider, subject, subject + " transfer failed");
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
        fail_web(provider, subject, subject + " transfer returned no result");
    long status{};
    require(curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status));
    log_debug(label + " response: status=" + std::to_string(status)
        + " curl_code=" + std::to_string(completed->data.result)
        + " duration_ms=" + std::to_string(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count())
        + " response_bytes=" + std::to_string(response.size()));
    log_debug_payload(label + " raw response", response, key);
    if (buffer.too_large) fail_web(provider, subject, subject + " response size limit exceeded");
    if (completed->data.result != CURLE_OK)
        fail_web(provider, subject, subject + " connection failed: " + std::string(curl_easy_strerror(completed->data.result)));
    if (status != 200)
        fail_web(provider, subject, subject + " HTTP " + std::to_string(status));

    // Bound depth while parsing, before recursive cleanup and serialization.
    const auto check_depth = [&](int depth, nlohmann::ordered_json::parse_event_t,
        nlohmann::ordered_json&) {
        if (depth > 64) fail_web(provider, subject, subject + " response nesting limit exceeded");
        return true;
    };
    const auto parsed = nlohmann::ordered_json::parse(response, check_depth, false);
    if (!parsed.is_object() || parsed.contains("error"))
        fail_web(provider, subject, "Invalid " + lower_subject + " response");
    return parsed;
}

std::string read_url(std::string_view provider, std::string_view url, std::string_view key) {
    url = trim_view(url);
    if (url.empty() || url.size() > 8192 || url.find_first_of(" \t\r\n") != std::string_view::npos
        || url.find('\0') != std::string_view::npos)
        fail_web(provider, "Page reading", "Page reading requires an absolute HTTP or HTTPS URL");
    const std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(curl_url(), curl_url_cleanup);
    const std::string text(url);
    if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL, text.c_str(), 0) != CURLUE_OK)
        fail_web(provider, "Page reading", "Invalid page URL");
    char* raw_scheme = nullptr;
    if (curl_url_get(parsed.get(), CURLUPART_SCHEME, &raw_scheme, 0) != CURLUE_OK)
        fail_web(provider, "Page reading", "Invalid page URL scheme");
    const std::unique_ptr<char, decltype(&curl_free)> scheme(raw_scheme, curl_free);
    if (std::string_view(scheme.get()) != "http" && std::string_view(scheme.get()) != "https")
        fail_web(provider, "Page reading", "Page reading supports only HTTP and HTTPS URLs");
    for (auto part : {CURLUPART_USER, CURLUPART_PASSWORD}) {
        char* raw_value = nullptr;
        const auto status = curl_url_get(parsed.get(), part, &raw_value, 0);
        const std::unique_ptr<char, decltype(&curl_free)> value(raw_value, curl_free);
        if (status == CURLUE_OK) fail_web(provider, "Page reading", "Page URLs must not contain credentials");
    }
    if (key.empty() || key.find_first_of("\r\n") != std::string_view::npos
        || key.find('\0') != std::string_view::npos)
        fail_web(provider, "Page reading", "Page reading API key is missing or invalid");
    return text;
}

std::string utf8_prefix(std::string_view value, std::size_t count) {
    count = std::min(count, value.size());
    while (count < value.size() && count > 0
        && (static_cast<unsigned char>(value[count]) & 0xc0) == 0x80) --count;
    return std::string(value.substr(0, count));
}

// Markdown destinations can contain balanced parentheses and escaped characters.
std::size_t markdown_delimiter_end(std::string_view text, std::size_t start,
    char open, char close) {
    int depth = 0;
    for (auto i = start; i < text.size(); ++i) {
        if (text[i] == '\\') {
            ++i;
        } else if (text[i] == open) {
            ++depth;
        } else if (text[i] == close && --depth == 0) {
            return i + 1;
        }
    }
    return std::string_view::npos;
}

std::size_t markdown_code_end(std::string_view text, std::size_t start) {
    const char marker = text[start];
    if (marker != '`' && !(marker == '~' && text.substr(start).starts_with("~~~")
            && (start == 0 || text[start - 1] == '\n'))) return start;
    auto end = start + 1;
    while (end < text.size() && text[end] == marker) ++end;
    const auto delimiter = text.substr(start, end - start);
    const auto close = text.find(delimiter, end);
    if (close == std::string_view::npos) return delimiter.size() >= 3 ? text.size() : start;
    return close + delimiter.size();
}

std::string without_page_images(std::string_view markdown) {
    std::string text;
    text.reserve(markdown.size());
    for (std::size_t i = 0; i < markdown.size();) {
        if (const auto end = markdown_code_end(markdown, i); end > i) {
            text.append(markdown.substr(i, end - i));
            i = end;
        } else if (markdown[i] == '\\' && i + 1 < markdown.size()) {
            text.append(markdown.substr(i, 2));
            i += 2;
        } else if (markdown.substr(i).starts_with("![")) {
            const auto label_end = markdown_delimiter_end(markdown, i + 1, '[', ']');
            const auto end = label_end < markdown.size() && markdown[label_end] == '('
                ? markdown_delimiter_end(markdown, label_end, '(', ')') : std::string_view::npos;
            if (end == std::string_view::npos) {
                // Keep malformed Markdown unchanged, without repeatedly scanning its tail.
                text.append(markdown.substr(i));
                break;
            }
            i = end;
            // Drop an enclosing link when the image was its only content.
            if (!text.empty() && text.back() == '[' && markdown.substr(i).starts_with("](")) {
                const auto link_end = markdown_delimiter_end(markdown, i + 1, '(', ')');
                if (link_end != std::string_view::npos) {
                    text.pop_back();
                    i = link_end;
                }
            }
        } else {
            text.push_back(markdown[i++]);
        }
    }
    return text;
}

std::string page_prefix(std::string_view markdown, std::size_t byte_limit) {
    std::size_t paragraph = 0, line = 0, word = 0;
    for (std::size_t i = 0; i < byte_limit;) {
        auto end = markdown_code_end(markdown, i);
        if (end == i && markdown[i] == '[') {
            const auto label_end = markdown_delimiter_end(markdown, i, '[', ']');
            if (label_end == std::string_view::npos) break;
            end = label_end;
            if (label_end < markdown.size() && markdown[label_end] == '(')
                end = markdown_delimiter_end(markdown, label_end, '(', ')');
            if (end == std::string_view::npos) break;
        }
        if (end > i && end != std::string_view::npos) {
            if (end > byte_limit) break;
            i = end;
            continue;
        }
        if (markdown[i] == '\n') {
            line = i;
            if (i > 0 && markdown[i - 1] == '\n') paragraph = i - 1;
        } else if (is_space(markdown[i])) {
            word = i;
        }
        ++i;
    }
    // Do not discard most of a large paragraph or an unbroken word just to find a boundary.
    for (const auto boundary : {paragraph, line, word}) {
        if (boundary > 0 && boundary >= byte_limit / 2) {
            auto prefix = markdown.substr(0, boundary);
            while (!prefix.empty() && is_space(prefix.back())) prefix.remove_suffix(1);
            return std::string(prefix);
        }
    }
    return utf8_prefix(markdown, byte_limit);
}

std::string page_output(std::string_view provider, std::string_view url,
    std::string_view title, std::string_view markdown) {
    const auto text = without_page_images(markdown);
    markdown = text;
    if (trim_view(markdown).empty()) fail_web(provider, "Page reading", "Page returned no readable content");
    constexpr std::size_t byte_limit = 64 * 1024;
    nlohmann::ordered_json result{{"url", url}, {"title", utf8_prefix(title, 1024)},
        {"markdown", utf8_prefix(markdown, byte_limit)}, {"truncated", false}};
    if (markdown.size() <= byte_limit && result.dump().size() <= byte_limit) return result.dump();
    result["truncated"] = true;
    result["truncation_reason"] = "Page content exceeded the output size limit; only the beginning is included.";
    // Bound serialized JSON too: quotes, newlines, and control bytes need escaping.
    std::size_t low = 0, high = std::min(markdown.size(), byte_limit);
    while (low < high) {
        const auto mid = low + (high - low + 1) / 2;
        result["markdown"] = utf8_prefix(markdown, mid);
        if (result.dump().size() <= byte_limit) low = mid;
        else high = mid - 1;
    }
    result["markdown"] = page_prefix(markdown, low);
    return result.dump();
}

} // namespace

std::string search_brave(std::string_view query, std::string_view key,
    const std::atomic_bool& cancelled, std::string_view endpoint) {
    if (cancelled.load()) return {};
    log_debug_payload("Brave search original query", query, key);
    query = limit_query(query, 600);
    if (query.empty())
        fail_web("Brave", "Web search", "Web search query is empty after the length limit");
    if (key.empty() || key.find_first_of("\r\n") != std::string_view::npos)
        fail_web("Brave", "Web search", "Brave API key is missing or invalid");

    CurlHandle curl;
    CurlHeaders headers;
    headers.append("Accept: application/json");
    headers.append("X-Subscription-Token: " + std::string(key));
    const std::unique_ptr<char, decltype(&curl_free)> encoded(
        curl_easy_escape(curl.get(), query.data(), static_cast<int>(query.size())), curl_free);
    if (!encoded) fail_web("Brave", "Web search", "Could not encode web search query");
    const std::string url = std::string(endpoint) + "?q=" + encoded.get()
        + "&count=10&extra_snippets=true&text_decorations=false&result_filter=web,news,discussions,faq,infobox,query";
    log_debug_payload("Brave search effective query", query, key);
    const auto parsed = request_web_json("Brave", curl, headers, url, {}, cancelled, key, "Web search");
    if (parsed.is_null()) return {};
    if (parsed.contains("web")) {
        const auto& web = parsed.at("web");
        if (!web.is_object() || !web.contains("results") || !web.at("results").is_array())
            fail_web("Brave", "Web search", "Invalid web search results");
    }
    return bounded_search_output("Brave", parsed);
}

std::string search_tavily(std::string_view query, std::string_view key,
    const std::atomic_bool& cancelled, std::string_view endpoint) {
    if (cancelled.load()) return {};
    log_debug_payload("Tavily search original query", query, key);
    query = limit_query(query, 400);
    if (query.empty())
        fail_web("Tavily", "Web search", "Web search query is empty after the length limit");
    if (key.empty() || key.find_first_of("\r\n") != std::string_view::npos)
        fail_web("Tavily", "Web search", "Tavily API key is missing or invalid");

    CurlHandle curl;
    CurlHeaders headers;
    headers.append("Accept: application/json");
    headers.append("Content-Type: application/json");
    headers.append("Authorization: Bearer " + std::string(key));
    const auto body = nlohmann::json{{"query", query}, {"max_results", 10},
        {"include_images", false}, {"include_image_descriptions", false},
        {"include_favicon", false}}.dump();
    const auto parsed = request_web_json("Tavily", curl, headers, std::string(endpoint), body, cancelled, key, "Web search");
    if (parsed.is_null()) return {};
    if (!parsed.contains("results") || !parsed["results"].is_array())
        fail_web("Tavily", "Web search", "Invalid web search results");

    return bounded_search_output("Tavily", parsed);
}

std::string read_firecrawl(std::string_view url, std::string_view key,
    const std::atomic_bool& cancelled, std::string_view endpoint) {
    if (cancelled.load()) return {};
    const auto target = read_url("Firecrawl", url, key);
    CurlHandle curl;
    CurlHeaders headers;
    headers.append("Accept: application/json");
    headers.append("Content-Type: application/json");
    headers.append("Authorization: Bearer " + std::string(key));
    const auto body = nlohmann::json{{"url", target}, {"formats", {"markdown"}},
        {"excludeTags", {"img", "picture", "video", "audio", "source", "iframe", "svg", "canvas"}},
        {"onlyMainContent", true}, {"timeout", 45000}, {"skipTlsVerification", false}}.dump();
    const auto response = request_web_json("Firecrawl", curl, headers, std::string(endpoint),
        body, cancelled, key, "Page reading", 60000L, 8 * 1024 * 1024);
    if (response.is_null()) return {};
    if (!response.contains("success") || response["success"] != true
        || !response.contains("data") || !response["data"].is_object())
        fail_web("Firecrawl", "Page reading", "Invalid page reading response");
    const auto& data = response["data"];
    const auto metadata = data.value("metadata", nlohmann::ordered_json::object());
    if (!metadata.is_object()) fail_web("Firecrawl", "Page reading", "Invalid page metadata");
    const auto status = metadata.contains("statusCode") && metadata["statusCode"].is_number()
        ? metadata["statusCode"].get<double>() : 200.0;
    if ((status < 200 || status >= 300) && status != 304)
        fail_web("Firecrawl", "Page reading", "Target page could not be loaded");
    if (metadata.contains("error") && !metadata["error"].is_null()
        && metadata["error"] != "") fail_web("Firecrawl", "Page reading", "Target page could not be loaded");
    const auto title = metadata.contains("title") && metadata["title"].is_string()
        ? metadata["title"].get<std::string>() : std::string{};
    return page_output("Firecrawl", target, title,
        data.value("markdown", std::string{}));
}

} // namespace cha
