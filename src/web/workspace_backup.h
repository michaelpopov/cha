#pragma once

#include <filesystem>

namespace cha::web {

// Archives the complete workspace as chaweb-YYYY-MM-DD-HH-MM.tar.gz in the
// supplied directory and returns the archive path. Throws when tar fails.
[[nodiscard]] std::filesystem::path backup_workspace(
    const std::filesystem::path& workspace,
    const std::filesystem::path& backup_dir);

} // namespace cha::web
