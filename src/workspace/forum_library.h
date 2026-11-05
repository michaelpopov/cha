#pragma once
#include <optional>
#include <string>
#include <vector>

namespace cha {
struct ForumLibrary {
    std::string api_key_id;
    std::vector<std::string> store_ids;
    std::optional<std::string> provider_id;
};
} // namespace cha
