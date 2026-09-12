#pragma once

#include <string>

namespace cha {

struct ApiKeyInfo {
    std::string id;
    std::string display_name;
    bool has_value{};
};

struct SavedApiKey {
    std::string id;
    std::string display_name;
    std::string value;
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

} // namespace cha
