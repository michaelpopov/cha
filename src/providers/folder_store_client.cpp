#include "providers/folder_store_client.h"

#include "util/curl.h"
#include "util/logging.h"
#include "util/text.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>

namespace cha {
namespace {

using Json = nlohmann::json;

constexpr std::string_view openai_origin = "https://api.openai.com/v1";
constexpr std::string_view google_origin = "https://generativelanguage.googleapis.com";
constexpr std::string_view google_host = "generativelanguage.googleapis.com";

bool safe_component(std::string_view value) {
    return !value.empty() && value.size() <= 512 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

bool google_store_valid(std::string_view value) {
    constexpr std::string_view prefix = "fileSearchStores/";
    return value.starts_with(prefix) && safe_component(value.substr(prefix.size()));
}

bool google_operation_valid(std::string_view value, std::string_view store = {}) {
    constexpr std::string_view middle = "/upload/operations/";
    const auto split = value.find(middle);
    return split != std::string_view::npos && google_store_valid(value.substr(0, split))
        && (store.empty() || value.substr(0, split) == store)
        && safe_component(value.substr(split + middle.size()));
}

FolderStoreRemoteResult invalid_response(std::string_view message, bool allocating = false) {
    FolderStoreRemoteResult result;
    result.message = message;
    result.unknown_allocation = allocating;
    return result;
}

bool json_error_object(const Json& body) {
    return body.is_object() && body.contains("error") && body["error"].is_object();
}

// Provider messages can echo uploaded text or credentials. Translate only known
// machine-readable categories; never display an unrecognized code or message.
std::string_view safe_error_reason(const Json& error) {
    if (!error.is_object()) return {};
    const auto code = error.find("code");
    if (code != error.end()) {
        if (*code == "invalid_file") return "The provider could not read this file. Check the note content before uploading again.";
        if (*code == "unsupported_file") return "The provider does not support this file.";
        if (*code == "server_error") return "The provider could not process the request. Try again later.";
        if (*code == "insufficient_quota") return "The provider account has insufficient quota. Check its billing and quota settings.";
        if (*code == "rate_limit_exceeded") return "The provider rate limit was reached. Try again later.";
        if (*code == "invalid_api_key") return "The saved API key was rejected.";
    }
    const auto status = error.find("status");
    const auto google_error = [&](int number, std::string_view name) {
        return (code != error.end() && *code == number)
            || (status != error.end() && *status == std::string(name));
    };
    if (google_error(3, "INVALID_ARGUMENT")) return "The provider rejected the file or request parameters. Check the note content and configuration.";
    if (google_error(7, "PERMISSION_DENIED")) return "The saved API key cannot access this resource. Check its project and permissions.";
    if (google_error(8, "RESOURCE_EXHAUSTED")) return "The provider quota or rate limit was reached. Check its quota settings or try again later.";
    if (google_error(4, "DEADLINE_EXCEEDED")) return "The provider timed out. Try again later.";
    if (google_error(13, "INTERNAL") || google_error(14, "UNAVAILABLE")) return "The provider could not process the request. Try again later.";
    if (google_error(16, "UNAUTHENTICATED")) return "The saved API key was rejected.";
    return {};
}

bool structured_not_found(const Json& body) {
    if (!json_error_object(body)) return false;
    const auto& error = body["error"];
    return (error.contains("type") && error["type"] == "invalid_request_error"
            && error.contains("message") && error["message"].is_string() && !error["message"].get_ref<const std::string&>().empty())
        || (error.contains("code") && error["code"] == 404 && error.contains("status") && error["status"] == "NOT_FOUND");
}

FolderStoreRemoteResult classify(
    const FolderStoreHttpResponse& response, bool allocating, std::string_view identity_field = "id") {
    FolderStoreRemoteResult result;
    if (response.cancelled) {
        result.kind = FolderStoreRemoteKind::cancelled;
        result.message = "The request was cancelled.";
        result.unknown_allocation = allocating;
        return result;
    }
    if (!response.error.empty()) {
        result.kind = FolderStoreRemoteKind::failed;
        result.message = "The store connection failed.";
        result.unknown_allocation = allocating;
        return result;
    }
    Json body = Json::parse(response.body, nullptr, false);
    if (response.status >= 200 && response.status < 300) {
        if (!body.is_object()) return invalid_response("The provider returned an invalid response.", allocating);
        result.kind = FolderStoreRemoteKind::ok;
        if (body.is_object()) {
            const auto identity = body.find(identity_field);
            if (identity != body.end() && identity->is_string()) {
                result.id = identity->get<std::string>();
            }
            if (body.contains("status") && body["status"].is_string()) {
                result.status = body["status"].get<std::string>();
            }
            if (body.contains("done") && !body["done"].is_boolean()) return invalid_response("The provider returned an invalid operation status.", allocating);
            if (body.contains("done") && body["done"].is_boolean()) {
                result.done = body["done"].get<bool>();
            }
            if (body.contains("error") && !body["error"].is_null()) {
                result.operation_error = true;
                result.message = safe_error_reason(body["error"]);
            } else if (body.contains("last_error")) {
                result.message = safe_error_reason(body["last_error"]);
            }
        }
        return result;
    }
    if (response.status == 404 && structured_not_found(body)) {
        result.kind = FolderStoreRemoteKind::not_found;
        result.message = "The remote resource was not found.";
        return result;
    }
    if ((response.status == 400 || (allocating && response.status == 409))
        && json_error_object(body)) {
        result.kind = FolderStoreRemoteKind::rejected;
        result.message = safe_error_reason(body["error"]);
        if (result.message.empty()) result.message = "The provider rejected the request.";
        return result;
    }
    result.kind = FolderStoreRemoteKind::failed;
    result.unknown_allocation = allocating && response.status != 401
        && response.status != 403;
    if (response.status == 401 || response.status == 403) {
        result.message = "The saved API key was rejected.";
        result.unknown_allocation = false;
    } else if (response.status == 404) {
        result.message = "The provider returned an unusable not-found response.";
    } else {
        result.message = "The provider request failed.";
    }
    const auto reason = json_error_object(body) ? safe_error_reason(body["error"]) : std::string_view{};
    if (!reason.empty()) result.message = reason;
    else if (response.status == 413) result.message = "The provider's file-size limit was exceeded. Reduce the note size before uploading again.";
    else if (response.status == 429) result.message = "The provider quota or rate limit was reached. Check its quota settings or try again later.";
    else if (response.status >= 500 && response.status < 600) result.message = "The provider could not process the request. Try again later.";
    return result;
}

std::int64_t decimal_count(const Json& value) {
    if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        std::int64_t result{};
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
        if (!text.empty() && text.front() >= '0' && text.front() <= '9'
            && error == std::errc{} && end == text.data() + text.size() && result >= 0) return result;
    }
    throw FolderStoreError("The store document counts are not valid.");
}

std::string header_value(
    const std::vector<FolderStoreHttpHeader>& headers, std::string_view name) {
    for (const auto& header : headers) {
        if (ascii_iequals(header.name, name)) return header.value;
    }
    return {};
}

bool google_upload_url_valid(std::string_view url) {
    const std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(
        curl_url(), curl_url_cleanup);
    const std::string owned(url);
    if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL, owned.c_str(), 0) != CURLUE_OK) {
        return false;
    }
    char* raw_scheme = nullptr;
    if (curl_url_get(parsed.get(), CURLUPART_SCHEME, &raw_scheme, 0) != CURLUE_OK) {
        return false;
    }
    const std::unique_ptr<char, decltype(&curl_free)> scheme(raw_scheme, curl_free);
    if (!ascii_iequals(scheme.get(), "https")) return false;
    char* raw_host = nullptr;
    if (curl_url_get(parsed.get(), CURLUPART_HOST, &raw_host, 0) != CURLUE_OK) {
        return false;
    }
    const std::unique_ptr<char, decltype(&curl_free)> host(raw_host, curl_free);
    if (!ascii_iequals(host.get(), google_host)) return false;
    for (const auto part : {CURLUPART_USER, CURLUPART_PASSWORD, CURLUPART_FRAGMENT}) {
        char* raw = nullptr;
        const auto code = curl_url_get(parsed.get(), part, &raw, 0);
        const std::unique_ptr<char, decltype(&curl_free)> value(raw, curl_free);
        if (code == CURLUE_OK) return false;
    }
    char* raw_port = nullptr;
    const auto port_code = curl_url_get(parsed.get(), CURLUPART_PORT, &raw_port, 0);
    const std::unique_ptr<char, decltype(&curl_free)> port(raw_port, curl_free);
    return port_code == CURLUE_NO_PORT || (port_code == CURLUE_OK && std::string_view(port.get()) == "443");
}

std::string resolve_key_id(
    const ModelBackendConfig& config, std::span<const ApiKeyInfo> keys) {
    std::optional<ApiKeyInfo> info;
    for (const auto& key : keys) {
        if ((!config.api_key_id.empty() && key.id == config.api_key_id)
            || (config.api_key_id.empty() && !config.api_key_env.empty()
                && key.display_name == config.api_key_env)) {
            if (info) {
                return {};
            }
            info = key;
            if (!config.api_key_id.empty()) break;
        }
    }
    if (!info || !info->has_value) return {};
    return info->id;
}

} // namespace

std::optional<FolderStoreService> folder_store_endpoint_service(const ModelBackendConfig& config) {
    if (config.mode != Mode::net || !config.https || config.port != 443) return {};
    if (config.api == ProviderApi::responses && ascii_iequals(config.host, "api.openai.com")
        && config.base_path.empty()) return FolderStoreService::openai;
    if (config.api == ProviderApi::google && ascii_iequals(config.host, google_host)
        && config.base_path == "/v1beta") return FolderStoreService::google;
    return {};
}

FolderStoreEligibility folder_store_match(
    const ModelBackendConfig& config, std::span<const ApiKeyInfo> keys) {
    FolderStoreEligibility result;
    const auto service = folder_store_endpoint_service(config);
    if (!service || config.auth != ProviderAuth::none) return result;
    result.api_key_id = resolve_key_id(config, keys);
    if (result.api_key_id.empty()) return result;
    if (*service == FolderStoreService::openai && !config.openrouter_targets.empty()) {
        log_warn("Ignoring unused OpenRouter targets for folder store access.");
    }
    result.service = service;
    return result;
}

FolderStoreHttpResponse folder_store_curl_transport(
    const FolderStoreHttpRequest& request, const std::atomic_bool& cancel) {
    FolderStoreHttpResponse result;
    try {
        CurlHandle curl;
        CurlHeaders headers;
        for (const auto& header : request.headers) {
            headers.append(header.name + ": " + header.value);
        }
        const auto require = [&](CURLcode code) {
            if (code != CURLE_OK) {
                throw std::runtime_error("Could not configure the store request.");
            }
        };
        require(curl_easy_setopt(curl.get(), CURLOPT_URL, request.url.c_str()));
        require(curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, request.method.c_str()));
        require(curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get()));
        require(curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L));
        require(curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L));
        require(curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 600L));
        require(curl_easy_setopt(curl.get(), CURLOPT_NOPROGRESS, 0L));
        require(curl_easy_setopt(curl.get(), CURLOPT_XFERINFODATA, &cancel));
        require(curl_easy_setopt(curl.get(), CURLOPT_XFERINFOFUNCTION,
            +[](void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
                return static_cast<const std::atomic_bool*>(user)->load() ? 1 : 0;
            }));
        require(curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https"));
        require(curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 1L));
        require(curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 2L));
        curl_mime* mime = nullptr;
        std::unique_ptr<curl_mime, decltype(&curl_mime_free)> mime_owner(nullptr, curl_mime_free);
        if (!request.multipart.empty()) {
            mime = curl_mime_init(curl.get());
            if (!mime) throw std::runtime_error("Could not create the upload form.");
            mime_owner.reset(mime);
            for (const auto& part : request.multipart) {
                auto* field = curl_mime_addpart(mime);
                if (!field) throw std::runtime_error("Could not create the upload form.");
                require(curl_mime_name(field, part.name.c_str()));
                if (!part.filename.empty()) {
                    require(curl_mime_filename(field, part.filename.c_str()));
                    require(curl_mime_type(field, part.mime_type.c_str()));
                    require(curl_mime_data(field, part.data.data(), part.data.size()));
                } else {
                    require(curl_mime_data(field, part.data.c_str(), CURL_ZERO_TERMINATED));
                }
            }
            require(curl_easy_setopt(curl.get(), CURLOPT_MIMEPOST, mime));
        } else if (request.method != "GET" && request.method != "DELETE") {
            require(curl_easy_setopt(
                curl.get(), CURLOPT_POSTFIELDS, request.body.c_str()));
            require(curl_easy_setopt(
                curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size())));
        }
        require(curl_easy_setopt(
            curl.get(), CURLOPT_WRITEFUNCTION,
            +[](char* data, size_t size, size_t count, void* user) -> size_t {
                auto& output = *static_cast<std::string*>(user);
                const auto bytes = size * count;
                try {
                    output.append(data, bytes);
                } catch (...) {
                    return 0;
                }
                return bytes;
            }));
        require(curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &result.body));
        std::string header_block;
        if (request.capture_headers) {
            require(curl_easy_setopt(
                curl.get(), CURLOPT_HEADERFUNCTION,
                +[](char* data, size_t size, size_t count, void* user) -> size_t {
                    auto& output = *static_cast<std::string*>(user);
                    const auto bytes = size * count;
                    try {
                        output.append(data, bytes);
                    } catch (...) {
                        return 0;
                    }
                    return bytes;
                }));
            require(curl_easy_setopt(curl.get(), CURLOPT_HEADERDATA, &header_block));
        }
        const std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> multi(
            curl_multi_init(), curl_multi_cleanup);
        if (!multi) throw std::runtime_error("Could not create the store transfer.");
        if (curl_multi_add_handle(multi.get(), curl.get()) != CURLM_OK) {
            throw std::runtime_error("The store transfer failed.");
        }
        const auto remove = [&](CURL* handle) {
            curl_multi_remove_handle(multi.get(), handle);
        };
        const std::unique_ptr<CURL, decltype(remove)> attached(curl.get(), remove);
        int running{};
        do {
            if (cancel.load()) {
                result.cancelled = true;
                return result;
            }
            if (curl_multi_perform(multi.get(), &running) != CURLM_OK) {
                throw std::runtime_error("The store transfer failed.");
            }
            if (running) {
                curl_multi_poll(multi.get(), nullptr, 0, 100, nullptr);
            }
        } while (running);
        // A complete response still carries ownership IDs when lifecycle
        // cancellation races completion. Let the caller record/log those IDs.
        int messages{};
        const auto* completed = curl_multi_info_read(multi.get(), &messages);
        if (!completed || completed->msg != CURLMSG_DONE) {
            throw std::runtime_error("The store transfer returned no result.");
        }
        if (completed->data.result != CURLE_OK) {
            result.cancelled = completed->data.result == CURLE_ABORTED_BY_CALLBACK;
            result.error = "The store connection failed.";
            return result;
        }
        require(curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &result.status));
        if (request.capture_headers) {
            std::string line;
            for (const char c : header_block) {
                if (c == '\n') {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    const auto colon = line.find(':');
                    if (colon != std::string::npos) {
                        auto name = line.substr(0, colon);
                        auto value = line.substr(colon + 1);
                        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                            value.erase(value.begin());
                        }
                        result.headers.push_back({std::move(name), std::move(value)});
                    }
                    line.clear();
                } else {
                    line.push_back(c);
                }
            }
        }
        return result;
    } catch (const std::exception& error) {
        result.error = error.what();
        return result;
    }
}

FolderStoreClient::FolderStoreClient(FolderStoreTransport transport)
    : transport_(transport ? std::move(transport) : folder_store_curl_transport) {}

std::string FolderStoreClient::search_stores(
    std::span<const FolderStoreSearchTarget> stores, std::string_view query,
    const std::atomic_bool& cancel) {
    if (trim_view(query).empty() || query.size() > 2048)
        return R"({"error":"Search requires a non-empty query of at most 2048 bytes."})";
    Json output{{"stores", Json::array()}, {"truncated", false}};
    std::size_t result_bytes = 64;
    for (const auto& store : stores) {
        if (cancel.load()) return R"({"error":"Search cancelled."})";
        Json entry{{"folder", utf8_prefix(store.folder_name, 256)}, {"matches", Json::array()}};
        try {
            if (store.api_key.empty()) throw FolderStoreError("The saved API key is missing or empty.");
            FolderStoreHttpRequest request;
            request.method = "POST";
            const bool google = store.service == FolderStoreService::google;
            if (google) {
                if (!google_store_valid(store.remote_store_id)) throw FolderStoreError("The store ID is invalid.");
                // Update this retrieval-only model when Google retires it.
                request.url = std::string(google_origin) + "/v1beta/models/gemini-3.8-flash:generateContent";
                request.headers = {{"x-goog-api-key", store.api_key}, {"Content-Type", "application/json"}};
                // File Search needs generation to run. A one-token limit stops it before grounding is returned.
                request.body = Json{
                    {"contents", Json::array({{{"role", "user"}, {"parts", Json::array({{{"text", query}}})}}})},
                    {"tools", Json::array({{{"fileSearch", {{"fileSearchStoreNames", {store.remote_store_id}}}}}})},
                    {"generationConfig", {{"maxOutputTokens", 2048}}},
                }.dump();
            } else {
                if (!safe_component(store.remote_store_id)) throw FolderStoreError("The store ID is invalid.");
                request.url = std::string(openai_origin) + "/vector_stores/" + store.remote_store_id + "/search";
                request.headers = {{"Authorization", "Bearer " + store.api_key}, {"Content-Type", "application/json"}};
                request.body = Json{{"query", query}, {"max_num_results", 5}}.dump();
            }
            const auto response = transport_(request, cancel);
            if (cancel.load() || response.cancelled) return R"({"error":"Search cancelled."})";
            const auto status = classify(response, false);
            if (status.kind != FolderStoreRemoteKind::ok || status.operation_error)
                throw FolderStoreError(status.message.empty() ? "The store search failed." : status.message);
            const auto body = Json::parse(response.body);
            if (google) {
                const auto& candidate = body.at("candidates").at(0);
                const auto reason = candidate.value("finishReason", "");
                if (reason != "STOP" && reason != "MAX_TOKENS")
                    throw FolderStoreError("Gemini could not complete the retrieval request.");
                const auto grounding = candidate.value("groundingMetadata", Json::object());
                const auto chunks = grounding.value("groundingChunks", Json::array());
                if (!chunks.is_array()) throw std::runtime_error("Invalid grounding chunks");
                for (const auto& chunk : chunks) {
                    const auto context = chunk.value("retrievedContext", Json::object());
                    const auto text = context.value("text", std::string{});
                    if (text.empty()) continue;
                    if (entry["matches"].size() == 5) {
                        output["truncated"] = true;
                        break;
                    }
                    const auto prefix = utf8_prefix(text, 4096);
                    if (prefix.size() != text.size()) output["truncated"] = true;
                    entry["matches"].push_back({{"filename", utf8_prefix(context.value("title", std::string{}), 256)},
                        {"text", prefix}});
                }
                // A normal completed search may return no passages; an incomplete one is unverified.
                if (entry["matches"].empty() && reason != "STOP")
                    throw FolderStoreError("Gemini returned no source passages; the search could not be verified.");
            } else {
                const auto& data = body.at("data");
                if (!data.is_array()) throw std::runtime_error("Invalid search results");
                if (body.value("has_more", false) || data.size() > 5) output["truncated"] = true;
                for (const auto& match : data) {
                    if (entry["matches"].size() == 5) break;
                    const auto filename = match.at("filename").get<std::string>();
                    const auto file_id = match.at("file_id").get<std::string>();
                    const auto& content = match.at("content");
                    if (!content.is_array()) throw std::runtime_error("Invalid search content");
                    std::string text;
                    for (const auto& part : content) {
                        if (part.at("type") != "text") continue;
                        const auto& passage = part.at("text").get_ref<const std::string&>();
                        if (!text.empty() && text.size() < 4096) text += '\n';
                        const auto prefix = utf8_prefix(passage, 4096 - text.size());
                        if (prefix.size() != passage.size()) output["truncated"] = true;
                        text += prefix;
                    }
                    entry["matches"].push_back({{"filename", utf8_prefix(filename, 256)},
                        {"file_id", utf8_prefix(file_id, 256)}, {"text", std::move(text)}});
                }
            }
        } catch (const FolderStoreError& error) {
            entry["error"] = error.what();
        } catch (const std::exception&) {
            entry["matches"] = Json::array();
            entry["error"] = "The store search failed or returned invalid results.";
        }
        if (entry.contains("error"))
            log_warn("Store search failed for " + entry["folder"].get<std::string>()
                + ": " + entry["error"].get<std::string>());
        const auto bytes = entry.dump().size() + 1;
        if (result_bytes + bytes > 65536) {
            output["truncated"] = true;
            break;
        }
        result_bytes += bytes;
        output["stores"].push_back(std::move(entry));
    }
    return output.dump();
}

FolderStoreRemoteResult FolderStoreClient::openai_create_store(
    std::string_view name, std::string_view key, const std::atomic_bool& cancel) {
    FolderStoreHttpRequest request;
    request.method = "POST";
    request.url = std::string(openai_origin) + "/vector_stores";
    request.headers.push_back({"Authorization", "Bearer " + std::string(key)});
    request.headers.push_back({"Content-Type", "application/json"});
    request.body = Json{{"name", name}}.dump();
    auto result = classify(transport_(request, cancel), true);
    if (result.kind == FolderStoreRemoteKind::ok && !safe_component(result.id)) {
        result.kind = FolderStoreRemoteKind::failed;
        result.message = "The provider did not return a store ID.";
        result.unknown_allocation = true;
        result.id.clear();
    }
    return result;
}

FolderStoreRemoteResult FolderStoreClient::openai_upload_file(
    std::string_view filename,
    std::string_view markdown,
    std::string_view key,
    const std::atomic_bool& cancel) {
    FolderStoreHttpRequest request;
    request.method = "POST";
    request.url = std::string(openai_origin) + "/files";
    request.headers.push_back({"Authorization", "Bearer " + std::string(key)});
    request.multipart.push_back({.name = "purpose", .data = "assistants"});
    request.multipart.push_back({
        .name = "file",
        .filename = std::string(filename),
        .mime_type = "text/markdown",
        .data = std::string(markdown),
    });
    auto result = classify(transport_(request, cancel), true);
    if (result.kind == FolderStoreRemoteKind::ok && !safe_component(result.id)) {
        result.kind = FolderStoreRemoteKind::failed;
        result.message = "The provider did not return a file ID.";
        result.unknown_allocation = true;
        result.id.clear();
    }
    return result;
}

FolderStoreRemoteResult FolderStoreClient::openai_attach_file(
    std::string_view store_id,
    std::string_view file_id,
    std::string_view key,
    const std::atomic_bool& cancel) {
    if (!safe_component(store_id) || !safe_component(file_id)) return invalid_response("The remote resource ID is invalid.");
    FolderStoreHttpRequest request;
    request.method = "POST";
    request.url = std::string(openai_origin) + "/vector_stores/" + std::string(store_id)
        + "/files";
    request.headers.push_back({"Authorization", "Bearer " + std::string(key)});
    request.headers.push_back({"Content-Type", "application/json"});
    request.body = Json{{"file_id", file_id}}.dump();
    auto result = classify(transport_(request, cancel), false);
    if (result.kind == FolderStoreRemoteKind::ok) {
        if (result.status == "failed" || result.status == "cancelled") {
            result.kind = FolderStoreRemoteKind::failed;
            if (result.message.empty()) result.message = "Indexing did not complete.";
        } else if (result.status != "in_progress" && result.status != "completed") {
            return invalid_response("The provider returned an invalid indexing status.");
        }
    }
    return result;
}

FolderStoreRemoteResult FolderStoreClient::openai_inspect_file(
    std::string_view store_id,
    std::string_view file_id,
    std::string_view key,
    const std::atomic_bool& cancel) {
    if (!safe_component(store_id) || !safe_component(file_id)) return invalid_response("The remote resource ID is invalid.");
    FolderStoreHttpRequest request;
    request.method = "GET";
    request.url = std::string(openai_origin) + "/vector_stores/" + std::string(store_id)
        + "/files/" + std::string(file_id);
    request.headers.push_back({"Authorization", "Bearer " + std::string(key)});
    auto result = classify(transport_(request, cancel), false);
    if (result.kind == FolderStoreRemoteKind::ok) {
        if (result.status == "failed" || result.status == "cancelled") {
            result.kind = FolderStoreRemoteKind::failed;
            if (result.message.empty()) result.message = "Indexing did not complete.";
        } else if (result.status != "completed" && result.status != "in_progress") {
            return invalid_response("The provider returned an invalid indexing status.");
        }
    }
    return result;
}

FolderStoreRemoteResult FolderStoreClient::openai_delete_store(
    std::string_view store_id, std::string_view key, const std::atomic_bool& cancel) {
    if (!safe_component(store_id)) return invalid_response("The remote resource ID is invalid.");
    FolderStoreHttpRequest request;
    request.method = "DELETE";
    request.url = std::string(openai_origin) + "/vector_stores/" + std::string(store_id);
    request.headers.push_back({"Authorization", "Bearer " + std::string(key)});
    const auto response = transport_(request, cancel);
    auto result = classify(response, false);
    if (result.kind == FolderStoreRemoteKind::ok) {
        const auto body = Json::parse(response.body, nullptr, false);
        if (!body.contains("deleted") || body["deleted"] != true
            || (body.contains("id") && body["id"] != std::string(store_id))) return invalid_response("The provider did not confirm resource deletion.");
    }
    return result;
}

FolderStoreRemoteResult FolderStoreClient::openai_delete_file(
    std::string_view file_id, std::string_view key, const std::atomic_bool& cancel) {
    if (!safe_component(file_id)) return invalid_response("The remote resource ID is invalid.");
    FolderStoreHttpRequest request;
    request.method = "DELETE";
    request.url = std::string(openai_origin) + "/files/" + std::string(file_id);
    request.headers.push_back({"Authorization", "Bearer " + std::string(key)});
    const auto response = transport_(request, cancel);
    auto result = classify(response, false);
    if (result.kind == FolderStoreRemoteKind::ok) {
        const auto body = Json::parse(response.body, nullptr, false);
        if (!body.contains("deleted") || body["deleted"] != true
            || (body.contains("id") && body["id"] != std::string(file_id))) return invalid_response("The provider did not confirm resource deletion.");
    }
    return result;
}

FolderStoreRemoteResult FolderStoreClient::google_create_store(
    std::string_view name, std::string_view key, const std::atomic_bool& cancel) {
    FolderStoreHttpRequest request;
    request.method = "POST";
    request.url = std::string(google_origin) + "/v1beta/fileSearchStores";
    request.headers.push_back({"x-goog-api-key", std::string(key)});
    request.headers.push_back({"Content-Type", "application/json"});
    request.body = Json{{"displayName", name}}.dump();
    auto result = classify(transport_(request, cancel), true, "name");
    if (result.kind == FolderStoreRemoteKind::ok && !google_store_valid(result.id)) {
        return invalid_response("The provider did not return a valid store name.", true);
    }
    return result;
}

FolderStoreRemoteResult FolderStoreClient::google_upload_document(
    std::string_view store_name,
    std::string_view display_name,
    std::string_view markdown,
    std::string_view key,
    const std::atomic_bool& cancel,
    const std::atomic_bool& cancel_forward) {
    if (!google_store_valid(store_name)) return invalid_response("The remote store name is invalid.");
    if (cancel_forward.load()) return {.kind = FolderStoreRemoteKind::cancelled, .message = "The request was cancelled."};
    FolderStoreHttpRequest start;
    start.method = "POST";
    start.url = std::string(google_origin) + "/upload/v1beta/" + std::string(store_name)
        + ":uploadToFileSearchStore";
    start.headers.push_back({"x-goog-api-key", std::string(key)});
    start.headers.push_back({"X-Goog-Upload-Protocol", "resumable"});
    start.headers.push_back({"X-Goog-Upload-Command", "start"});
    start.headers.push_back(
        {"X-Goog-Upload-Header-Content-Length", std::to_string(markdown.size())});
    start.headers.push_back({"X-Goog-Upload-Header-Content-Type", "text/markdown"});
    start.headers.push_back({"Content-Type", "application/json"});
    start.body = Json{{"displayName", display_name}}.dump();
    start.capture_headers = true;
    auto started = transport_(start, cancel);
    // A successful resumable start returns its session URL in a header and may
    // have no JSON body.
    if (started.status >= 200 && started.status < 300 && started.body.empty()) started.body = "{}";
    auto started_result = classify(started, false, "name");
    if (started_result.kind != FolderStoreRemoteKind::ok) return started_result;
    const auto upload_url = header_value(started.headers, "X-Goog-Upload-URL");
    if (upload_url.empty() || !google_upload_url_valid(upload_url)) {
        FolderStoreRemoteResult failed;
        failed.kind = FolderStoreRemoteKind::failed;
        failed.message = "The provider returned an invalid upload URL.";
        return failed;
    }
    FolderStoreHttpRequest upload;
    upload.method = "POST";
    upload.url = upload_url;
    upload.headers.push_back({"x-goog-api-key", std::string(key)});
    upload.headers.push_back({"X-Goog-Upload-Command", "upload, finalize"});
    upload.headers.push_back({"X-Goog-Upload-Offset", "0"});
    upload.headers.push_back({"Content-Type", "text/markdown"});
    upload.headers.push_back({"Content-Length", std::to_string(markdown.size())});
    upload.body = std::string(markdown);
    // A user cancel stops the next transfer, but an already-sent finalize must
    // still return its operation ID. Only lifecycle interruption aborts it.
    if (cancel_forward.load()) return {.kind = FolderStoreRemoteKind::cancelled, .message = "The request was cancelled."};
    auto result = classify(transport_(upload, cancel), false, "name");
    if (result.kind == FolderStoreRemoteKind::ok) {
        if (!google_operation_valid(result.id, store_name)) {
            result.kind = FolderStoreRemoteKind::failed;
            result.message = "The provider returned an invalid upload operation.";
            result.id.clear();
        }
    }
    if (result.kind == FolderStoreRemoteKind::ok && result.operation_error) {
        result.kind = FolderStoreRemoteKind::failed;
        if (result.message.empty()) result.message = "Indexing did not complete.";
    }
    return result;
}

FolderStoreRemoteResult FolderStoreClient::google_inspect_operation(
    std::string_view operation,
    std::string_view key,
    const std::atomic_bool& cancel) {
    if (!google_operation_valid(operation)) return invalid_response("The upload operation name is invalid.");
    FolderStoreHttpRequest request;
    request.method = "GET";
    request.url = std::string(google_origin) + "/v1beta/" + std::string(operation);
    request.headers.push_back({"x-goog-api-key", std::string(key)});
    auto result = classify(transport_(request, cancel), false, "name");
    if (result.kind == FolderStoreRemoteKind::ok) {
        if (result.id != operation) return invalid_response("The provider returned an invalid upload operation.");
        if (result.operation_error) {
            result.kind = FolderStoreRemoteKind::failed;
            if (result.message.empty()) result.message = "Indexing did not complete.";
        }
    }
    return result;
}

FolderStoreRemoteResult FolderStoreClient::google_inspect_store(
    std::string_view store_name, std::string_view key, const std::atomic_bool& cancel) {
    if (!google_store_valid(store_name)) return invalid_response("The remote store name is invalid.");
    FolderStoreHttpRequest request;
    request.method = "GET";
    request.url = std::string(google_origin) + "/v1beta/" + std::string(store_name);
    request.headers.push_back({"x-goog-api-key", std::string(key)});
    auto response = transport_(request, cancel);
    auto result = classify(response, false, "name");
    if (result.kind != FolderStoreRemoteKind::ok) return result;
    const auto body = Json::parse(response.body, nullptr, false);
    try {
        if (body.contains("activeDocumentsCount")) {
            result.active = decimal_count(body["activeDocumentsCount"]);
        }
        if (body.contains("pendingDocumentsCount")) {
            result.pending = decimal_count(body["pendingDocumentsCount"]);
        }
        if (body.contains("failedDocumentsCount")) {
            result.failed = decimal_count(body["failedDocumentsCount"]);
        }
    } catch (const FolderStoreError& error) {
        result.kind = FolderStoreRemoteKind::failed;
        result.message = error.what();
    }
    return result;
}

FolderStoreRemoteResult FolderStoreClient::google_delete_store(
    std::string_view store_name, std::string_view key, const std::atomic_bool& cancel) {
    if (!google_store_valid(store_name)) return invalid_response("The remote store name is invalid.");
    const auto store_url = std::string(google_origin) + "/v1beta/" + std::string(store_name);
    const auto document_prefix = std::string(store_name) + "/documents/";
    FolderStoreHttpRequest request;
    request.method = "GET";
    request.headers.push_back({"x-goog-api-key", std::string(key)});
    const auto send = [&] {
        if (cancel.load()) return FolderStoreHttpResponse{.cancelled = true};
        return transport_(request, cancel);
    };

    // Google's force-delete can remove the store while leaving its Documents
    // readable. List even when the parent was deleted by an earlier attempt.
    // Finish pagination before deleting anything so removals cannot shift pages.
    std::vector<std::string> documents;
    std::set<std::string> page_tokens;
    std::string page_token;
    do {
        request.url = store_url + "/documents?pageSize=20";
        if (!page_token.empty()) {
            const std::unique_ptr<char, decltype(&curl_free)> encoded(
                curl_easy_escape(nullptr, page_token.data(), static_cast<int>(page_token.size())), curl_free);
            if (!encoded) return invalid_response("The document page token could not be encoded.");
            request.url += "&pageToken=" + std::string(encoded.get());
        }
        const auto response = send();
        auto result = classify(response, false, "name");
        if (result.kind == FolderStoreRemoteKind::not_found && page_token.empty()) break;
        if (result.kind != FolderStoreRemoteKind::ok) {
            if (result.kind == FolderStoreRemoteKind::not_found)
                return invalid_response("The provider's document listing disappeared during cleanup.");
            return result;
        }
        if (result.operation_error) {
            result.kind = FolderStoreRemoteKind::failed;
            return result;
        }
        const auto body = Json::parse(response.body, nullptr, false);
        if (body.contains("documents")) {
            if (!body["documents"].is_array()) return invalid_response("The provider returned an invalid document listing.");
            for (const auto& document : body["documents"]) {
                if (!document.is_object() || !document.contains("name") || !document["name"].is_string())
                    return invalid_response("The provider returned an invalid document name.");
                const auto& name = document["name"].get_ref<const std::string&>();
                if (!name.starts_with(document_prefix) || !safe_component(std::string_view(name).substr(document_prefix.size())))
                    return invalid_response("The provider returned an invalid document name.");
                documents.push_back(name);
            }
        }
        page_token.clear();
        if (body.contains("nextPageToken")) {
            if (!body["nextPageToken"].is_string()) return invalid_response("The provider returned an invalid document page token.");
            page_token = body["nextPageToken"].get<std::string>();
            if (!page_token.empty() && !page_tokens.insert(page_token).second)
                return invalid_response("The provider returned an invalid document page token.");
        }
    } while (!page_token.empty());

    request.method = "DELETE";
    for (const auto& document : documents) {
        request.url = std::string(google_origin) + "/v1beta/" + document + "?force=true";
        auto result = classify(send(), false, "name");
        if (result.operation_error) result.kind = FolderStoreRemoteKind::failed;
        if (result.kind != FolderStoreRemoteKind::ok && result.kind != FolderStoreRemoteKind::not_found) return result;
    }
    // Keep the known parent ID until all document deletions succeed. An explicit
    // retry can discover the remaining documents without another persistent list.
    request.url = store_url + "?force=true";
    auto result = classify(send(), false, "name");
    if (result.operation_error) result.kind = FolderStoreRemoteKind::failed;
    return result;
}

} // namespace cha
