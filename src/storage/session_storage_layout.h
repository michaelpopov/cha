#pragma once

#include <filesystem>

namespace cha {

// Permanent import preflight for an incomplete manual cutover. It only checks
// for regular legacy .sqlite3 files in the two old source locations; it never
// opens or modifies them.
[[nodiscard]] bool has_legacy_session_databases(
    const std::filesystem::path& workspace_root);

} // namespace cha
