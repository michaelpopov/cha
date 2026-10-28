#include "providers/folder_store_client.h"

#include "util/curl.h"
#include "util/logging.h"
#include "util/text.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <memory>
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
            }
        }
        return result;
    }
    if (response.status == 404 && structured_not_found(body)) {
        result.kind = FolderStoreRemoteKind::not_found;
        result.message = "The remote resource was not found.";
        return result;
    }
    if (response.status == 400 && json_error_object(body) && !allocating) {
        result.kind = FolderStoreRemoteKind::rejected;
        result.message = "The provider rejected the request.";
        return result;
    }
    if ((response.status == 400 || response.status == 409)
        && json_error_object(body) && allocating) {
        result.kind = FolderStoreRemoteKind::rejected;
        result.message = "The provider rejected the request.";
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
    const ModelBackendConfig& config, const ApiKeyStore& keys, std::string& reason) {
    std::optional<ApiKeyInfo> info;
    if (!config.api_key_id.empty()) info = keys.find(config.api_key_id);
    else if (!config.api_key_env.empty()) info = keys.find_by_name(config.api_key_env);
    if (!info) {
        reason = "The saved API key was not found.";
        return {};
    }
    if (!info->has_value) {
        reason = "The saved API key is empty.";
        return {};
    }
    return info->id;
}

} // namespace

FolderStoreEligibility folder_store_match(
    const ModelBackendConfig& config, const ApiKeyStore& keys) {
    FolderStoreEligibility result;
    if (config.mode != Mode::net) {
        result.reason = "Store access requires a network provider.";
        return result;
    }
    if (config.auth != ProviderAuth::none) {
        result.reason = "Store access requires a saved API key.";
        return result;
    }
    if (!config.https) {
        result.reason = "Store access requires HTTPS.";
        return result;
    }
    if (config.port != 443) {
        result.reason = "Store access requires port 443.";
        return result;
    }
    const auto& host = config.host;
    if (config.api == ProviderApi::responses
        && ascii_iequals(host, "api.openai.com")
        && config.base_path.empty()) {
        result.api_key_id = resolve_key_id(config, keys, result.reason);
        if (result.api_key_id.empty()) return result;
        if (!config.openrouter_targets.empty()) {
            log_warn("Ignoring unused OpenRouter targets for folder store access.");
        }
        result.service = FolderStoreService::openai;
        return result;
    }
    if (config.api == ProviderApi::google
        && ascii_iequals(host, google_host)
        && config.base_path == "/v1beta") {
        result.api_key_id = resolve_key_id(config, keys, result.reason);
        if (result.api_key_id.empty()) return result;
        result.service = FolderStoreService::google;
        return result;
    }
    result.reason = "This provider is not a supported OpenAI or Google store endpoint.";
    return result;
}

std::vector<FolderStoreChoice> folder_store_choices_from_providers(
    std::span<const ModelBackendConfig> configs, const ApiKeyStore& keys) {
    std::vector<FolderStoreChoice> choices;
    for (const auto& config : configs) {
        const auto match = folder_store_match(config, keys);
        if (!match.service) continue;
        const auto info = keys.find(match.api_key_id);
        if (!info) continue;
        FolderStoreChoice choice{
            .service = *match.service,
            .api_key_id = match.api_key_id,
            .api_key_name = info->display_name,
        };
        const auto duplicate = std::find_if(
            choices.begin(), choices.end(), [&](const FolderStoreChoice& existing) {
                return existing.service == choice.service
                    && existing.api_key_id == choice.api_key_id;
            });
        if (duplicate == choices.end()) choices.push_back(std::move(choice));
    }
    return choices;
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
        if (result.status == "failed" || result.status == "cancelled") return invalid_response("Indexing did not complete.");
        if (result.status != "in_progress" && result.status != "completed") return invalid_response("The provider returned an invalid indexing status.");
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
            result.message = "Indexing did not complete.";
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
            || (body.contains("id") && body["id"] != store_id)) return invalid_response("The provider did not confirm resource deletion.");
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
            || (body.contains("id") && body["id"] != file_id)) return invalid_response("The provider did not confirm resource deletion.");
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
    const std::atomic_bool& cancel) {
    if (!google_store_valid(store_name)) return invalid_response("The remote store name is invalid.");
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
        result.message = "Indexing did not complete.";
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
            result.message = "Indexing did not complete.";
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
    FolderStoreHttpRequest request;
    request.method = "DELETE";
    request.url = std::string(google_origin) + "/v1beta/" + std::string(store_name)
        + "?force=true";
    request.headers.push_back({"x-goog-api-key", std::string(key)});
    return classify(transport_(request, cancel), false, "name");
}

} // namespace cha
