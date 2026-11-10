#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace cha {

struct ApiKeyInfo {
    std::string id;
    std::string display_name;
    bool has_value{};
    std::string cloud_store{"none"};
};

struct SavedApiKey {
    std::string id;
    std::string display_name;
    std::string value;
    std::string cloud_store{"none"};
};

struct R2StorageKey {
    std::string id;
    std::string display_name;
    std::string url;
    std::string access_key_id;
    std::string secret_key;
};

struct R2StorageInfo {
    std::string id;
    std::string display_name;
    std::string url;
    std::string access_key_id;
    bool has_secret_key{};
};

inline bool valid_d1_credential_field(
    std::string_view value, bool reject_only_space = true) {
    return !value.empty() && value.size() <= 16 * 1024
        && value.find_first_of("\r\n") == std::string_view::npos
        && (!reject_only_space || std::ranges::any_of(value, [](unsigned char c) {
            return std::isspace(c) == 0;
        }));
}

// D1 sync credentials share the saved-key namespace (system/keys/<id>/).
struct D1StorageKey {
    std::string id;
    std::string display_name;
    std::string account_id;
    std::string database_id;
    std::string api_token;

    // The loader represents unusable connection fields as empty strings.
    [[nodiscard]] bool usable() const {
        return !account_id.empty() && !database_id.empty() && !api_token.empty();
    }
};

struct D1StorageInfo {
    std::string id;
    std::string display_name;
    std::string account_id;
    std::string database_id;
    bool has_api_token{};
};

} // namespace cha
