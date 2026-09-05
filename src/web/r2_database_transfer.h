#pragma once

#include <cstdint>
#include <filesystem>

namespace cha::web {

struct R2DatabaseTransfer {
    std::uintmax_t byte_count{};
};

enum class R2DatabaseLease {
    acquire,
    already_held,
};

// Transfers of the configured workspace database. By default each operation
// acquires CHA's database lease. The in-process runtime instead closes SQLite
// while keeping its existing lease throughout. R2 object location and S3 API
// credentials come from CHA_R2_URL, CHA_R2_ACCESS_KEY_ID, and
// CHA_R2_SECRET_ACCESS_KEY.
R2DatabaseTransfer upload_database_to_r2(
    const std::filesystem::path& database_path,
    R2DatabaseLease lease = R2DatabaseLease::acquire);
R2DatabaseTransfer download_database_from_r2(
    const std::filesystem::path& database_path,
    R2DatabaseLease lease = R2DatabaseLease::acquire);

} // namespace cha::web
