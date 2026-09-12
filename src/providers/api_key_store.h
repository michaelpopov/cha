#pragma once

#include "providers/credentials.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

class WorkspaceConfigStore;

// Small facade over the active vault's database-backed system/keys tree.
class ApiKeyStore {
public:
    ApiKeyStore(
        WorkspaceConfigStore& config,
        std::filesystem::path legacy_path = {});

    ApiKeyStore(const ApiKeyStore&) = delete;
    ApiKeyStore& operator=(const ApiKeyStore&) = delete;

    [[nodiscard]] std::vector<ApiKeyInfo> list() const;
    [[nodiscard]] std::optional<ApiKeyInfo> find(std::string_view id) const;
    [[nodiscard]] std::optional<ApiKeyInfo> find_by_name(
        std::string_view display_name) const;
    [[nodiscard]] std::string value(std::string_view id) const;
    [[nodiscard]] std::string value_by_name(
        std::string_view display_name) const;
    [[nodiscard]] std::optional<R2StorageKey> r2() const;
    [[nodiscard]] std::optional<R2StorageInfo> r2_info() const;

    ApiKeyInfo create(std::string_view display_name, std::string_view value);
    ApiKeyInfo rename(std::string_view id, std::string_view display_name);
    ApiKeyInfo replace(std::string_view id, std::string_view value);
    void remove(std::string_view id);
    R2StorageInfo save_r2(
        std::string_view display_name,
        std::string_view url,
        std::string_view access_key_id,
        std::optional<std::string_view> secret_key);
    void remove_r2();
    void migrate_vault();

private:
    void migrate();

    WorkspaceConfigStore* config_{};
    std::filesystem::path legacy_path_;
    mutable std::mutex mutex_;
};

} // namespace cha
