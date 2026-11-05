#pragma once
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cha {
struct ForumLibraryCatalogueEntry {
    std::string name;
    std::string description;
};

struct ForumLibrary {
    std::string api_key_id;
    std::vector<std::string> store_ids;
    std::optional<std::string> provider_id;
    std::map<std::string, ForumLibraryCatalogueEntry, std::less<>> catalogue;
};
} // namespace cha
