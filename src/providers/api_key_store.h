#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

struct ApiKeyInfo {
    std::string id;
    std::string display_name;
    bool has_value{};
};

// Process-wide owner for inference credentials. The file lives beside the
// application config, not in the workspace database, and is always written
// with owner-only permissions.
class ApiKeyStore {
public:
    explicit ApiKeyStore(std::filesystem::path path);

    ApiKeyStore(const ApiKeyStore&) = delete;
    ApiKeyStore& operator=(const ApiKeyStore&) = delete;

    [[nodiscard]] std::vector<ApiKeyInfo> list() const;
    [[nodiscard]] std::optional<ApiKeyInfo> find(std::string_view id) const;
    [[nodiscard]] std::string value(std::string_view id) const;

    ApiKeyInfo create(std::string_view display_name, std::string_view value);
    ApiKeyInfo rename(std::string_view id, std::string_view display_name);
    ApiKeyInfo replace(std::string_view id, std::string_view value);
    void remove(std::string_view id);

private:
    struct Record {
        std::string display_name;
        std::string value;
    };

    static void validate_display_name(std::string_view value);
    static void validate_secret(std::string_view value);
    static ApiKeyInfo info(std::string_view id, const Record& record);
    void save_unlocked() const;

    std::filesystem::path path_;
    mutable std::mutex mutex_;
    std::map<std::string, Record, std::less<>> records_;
    std::uint64_t next_id_{1};
};

} // namespace cha
