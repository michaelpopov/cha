#pragma once

#include <string>

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

// D1 sync credentials share the saved-key namespace (system/keys/<id>/).
struct D1StorageKey {
    std::string id;
    std::string display_name;
    std::string account_id;
    std::string database_id;
    std::string api_token;
};

struct D1StorageInfo {
    std::string id;
    std::string display_name;
    std::string account_id;
    std::string database_id;
    bool has_api_token{};
};

} // namespace cha
