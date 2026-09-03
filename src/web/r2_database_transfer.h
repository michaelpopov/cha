#pragma once

#include <cstdint>
#include <filesystem>

namespace cha::web {

struct R2DatabaseTransfer {
    std::uintmax_t byte_count{};
};

// Offline transfers of the configured workspace database. Both operations
// acquire CHA's database lease and read their R2 object location and S3 API
// credentials from CHA_R2_URL, CHA_R2_ACCESS_KEY_ID, and
// CHA_R2_SECRET_ACCESS_KEY.
R2DatabaseTransfer upload_database_to_r2(
    const std::filesystem::path& database_path);
R2DatabaseTransfer download_database_from_r2(
    const std::filesystem::path& database_path);

} // namespace cha::web
