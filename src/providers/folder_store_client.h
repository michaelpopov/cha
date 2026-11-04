#pragma once

#include "characters/character_config.h"
#include "providers/credentials.h"
#include "storage/folder_store_storage.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

struct FolderStoreEligibility {
    std::optional<FolderStoreService> service;
    std::string api_key_id;
};

struct FolderStoreHttpHeader {
    std::string name;
    std::string value;
};

struct FolderStoreMultipartPart {
    std::string name;
    std::string filename;
    std::string mime_type;
    std::string data;
};

struct FolderStoreHttpRequest {
    std::string method;
    std::string url;
    std::vector<FolderStoreHttpHeader> headers;
    std::string body;
    std::vector<FolderStoreMultipartPart> multipart;
    bool capture_headers{};
};

struct FolderStoreHttpResponse {
    long status{};
    std::string body;
    std::vector<FolderStoreHttpHeader> headers;
    std::string error;
    bool cancelled{};
};

using FolderStoreTransport = std::function<FolderStoreHttpResponse(
    const FolderStoreHttpRequest&, const std::atomic_bool&)>;

enum class FolderStoreRemoteKind {
    ok,
    not_found,
    rejected,
    failed,
    cancelled,
};

struct FolderStoreRemoteResult {
    FolderStoreRemoteKind kind{FolderStoreRemoteKind::failed};
    std::string message;
    std::string id;
    std::string status;
    std::int64_t active{};
    std::int64_t pending{};
    std::int64_t failed{};
    bool done{};
    bool operation_error{};
    bool unknown_allocation{};
};

struct FolderStoreListItem {
    std::string id;
    std::string name;
};

struct FolderStoreList {
    FolderStoreRemoteResult result;
    std::vector<FolderStoreListItem> stores;
};

struct FolderStoreDetail {
    FolderStoreRemoteResult result;
    std::string id;
    std::string name;
    std::string description;
    std::string created_at;
};

[[nodiscard]] std::optional<FolderStoreService> folder_store_endpoint_service(
    const ModelBackendConfig& config);
[[nodiscard]] FolderStoreEligibility folder_store_match(
    const ModelBackendConfig& config, std::span<const ApiKeyInfo> keys);
FolderStoreHttpResponse folder_store_curl_transport(
    const FolderStoreHttpRequest& request, const std::atomic_bool& cancel);

class FolderStoreClient {
public:
    explicit FolderStoreClient(FolderStoreTransport transport = {});

    [[nodiscard]] std::string search_stores(
        std::span<const FolderStoreSearchTarget> stores, std::string_view query,
        const std::atomic_bool& cancel);

    // Reads every page of the stores that the key can access. A store without
    // a name has an empty name.
    [[nodiscard]] FolderStoreList list_stores(
        FolderStoreService service, std::string_view key, const std::atomic_bool& cancel);
    // created_at is an RFC 3339 UTC string. A missing description is empty.
    [[nodiscard]] FolderStoreDetail get_store(
        FolderStoreService service, std::string_view store_id, std::string_view key,
        const std::atomic_bool& cancel);
    // Deletes the store's files or documents, then the store. An absent store
    // after a complete child cleanup counts as deleted.
    [[nodiscard]] FolderStoreRemoteResult delete_store(
        FolderStoreService service, std::string_view store_id, std::string_view key,
        const std::atomic_bool& cancel);

    [[nodiscard]] FolderStoreRemoteResult openai_create_store(
        std::string_view name, std::string_view description, std::string_view key, const std::atomic_bool& cancel);
    [[nodiscard]] FolderStoreRemoteResult openai_upload_file(
        std::string_view filename,
        std::string_view markdown,
        std::string_view key,
        const std::atomic_bool& cancel);
    [[nodiscard]] FolderStoreRemoteResult openai_attach_file(
        std::string_view store_id,
        std::string_view file_id,
        std::string_view key,
        const std::atomic_bool& cancel);
    [[nodiscard]] FolderStoreRemoteResult openai_inspect_file(
        std::string_view store_id,
        std::string_view file_id,
        std::string_view key,
        const std::atomic_bool& cancel);
    [[nodiscard]] FolderStoreRemoteResult openai_delete_store(
        std::string_view store_id, std::string_view key, const std::atomic_bool& cancel);
    [[nodiscard]] FolderStoreRemoteResult openai_delete_file(
        std::string_view file_id, std::string_view key, const std::atomic_bool& cancel);

    [[nodiscard]] FolderStoreRemoteResult google_create_store(
        std::string_view name, std::string_view key, const std::atomic_bool& cancel);
    [[nodiscard]] FolderStoreRemoteResult google_upload_document(
        std::string_view store_name,
        std::string_view display_name,
        std::string_view markdown,
        std::string_view key,
        const std::atomic_bool& cancel,
        const std::atomic_bool& cancel_forward,
        std::string_view mime_type = "text/markdown",
        const nlohmann::json& custom_metadata = nullptr);
    [[nodiscard]] FolderStoreRemoteResult google_upload_store_metadata(
        std::string_view store_name, std::string_view name, std::string_view description,
        std::string_view key, const std::atomic_bool& cancel, const std::atomic_bool& cancel_forward);
    [[nodiscard]] FolderStoreRemoteResult google_inspect_operation(
        std::string_view operation,
        std::string_view key,
        const std::atomic_bool& cancel);
    [[nodiscard]] FolderStoreRemoteResult google_inspect_store(
        std::string_view store_name, std::string_view key, const std::atomic_bool& cancel);
    // force_store=false refuses to delete a store that still has documents.
    [[nodiscard]] FolderStoreRemoteResult google_delete_store(
        std::string_view store_name, std::string_view key, const std::atomic_bool& cancel,
        bool force_store = true);

private:
    FolderStoreTransport transport_;
};

} // namespace cha
