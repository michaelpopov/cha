#pragma once

#include "characters/character_config.h"
#include "providers/api_key_store.h"
#include "storage/folder_store_storage.h"

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
    std::string reason;
};

struct FolderStoreChoice {
    FolderStoreService service{FolderStoreService::openai};
    std::string api_key_id;
    std::string api_key_name;
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

[[nodiscard]] FolderStoreEligibility folder_store_match(
    const ModelBackendConfig& config, const ApiKeyStore& keys);
[[nodiscard]] FolderStoreEligibility folder_store_match(
    const ModelBackendConfig& config, std::span<const ApiKeyInfo> keys);
[[nodiscard]] std::vector<FolderStoreChoice> folder_store_choices_from_providers(
    std::span<const ModelBackendConfig> configs, const ApiKeyStore& keys);

FolderStoreHttpResponse folder_store_curl_transport(
    const FolderStoreHttpRequest& request, const std::atomic_bool& cancel);

class FolderStoreClient {
public:
    explicit FolderStoreClient(FolderStoreTransport transport = {});

    [[nodiscard]] FolderStoreRemoteResult openai_create_store(
        std::string_view name, std::string_view key, const std::atomic_bool& cancel);
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
        const std::atomic_bool& cancel_forward);
    [[nodiscard]] FolderStoreRemoteResult google_inspect_operation(
        std::string_view operation,
        std::string_view key,
        const std::atomic_bool& cancel);
    [[nodiscard]] FolderStoreRemoteResult google_inspect_store(
        std::string_view store_name, std::string_view key, const std::atomic_bool& cancel);
    [[nodiscard]] FolderStoreRemoteResult google_delete_store(
        std::string_view store_name, std::string_view key, const std::atomic_bool& cancel);

private:
    FolderStoreTransport transport_;
};

} // namespace cha
