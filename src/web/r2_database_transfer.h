#pragma once

#include "providers/credentials.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

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
// credentials come from the active vault's type="R2" key.
R2DatabaseTransfer upload_database_to_r2(
    const std::filesystem::path& database_path,
    const std::filesystem::path& vault_definition_path,
    const R2StorageKey& storage,
    R2DatabaseLease lease = R2DatabaseLease::acquire);
R2DatabaseTransfer download_database_from_r2(
    const std::filesystem::path& database_path,
    const std::filesystem::path& vault_definition_path,
    const R2StorageKey& storage,
    R2DatabaseLease lease = R2DatabaseLease::acquire);

// Lists root-level SQLite database objects and downloads one into a new local
// file. These operations power the vault picker and never replace an existing
// database.
std::vector<std::string> list_r2_database_names(
    const R2StorageKey& storage);
R2DatabaseTransfer download_new_database_from_r2(
    const std::filesystem::path& database_path,
    const std::filesystem::path& vault_definition_path,
    std::string_view database_name,
    const R2StorageKey& storage);

} // namespace cha::web
