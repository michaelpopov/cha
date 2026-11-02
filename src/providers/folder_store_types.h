#pragma once

#include <string>

namespace cha {

enum class FolderStoreService { openai, google };

// Request-owned access to an attached store. Credentials never enter model input.
struct FolderStoreSearchTarget {
    std::string folder_name;
    std::string remote_store_id;
    std::string api_key;
    FolderStoreService service{FolderStoreService::openai};
};

} // namespace cha
