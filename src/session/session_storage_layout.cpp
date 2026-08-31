#include "session/session_storage_layout.h"

#include <filesystem>

namespace cha {
namespace {

bool contains_legacy_database(const std::filesystem::path& directory) {
    if (!std::filesystem::exists(directory)) return false;
    if (!std::filesystem::is_directory(directory)) return false;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file()
            && entry.path().extension() == ".sqlite3") {
            return true;
        }
    }
    return false;
}

} // namespace

bool has_legacy_session_databases(
    const std::filesystem::path& workspace_root) {
    const std::filesystem::path forums = workspace_root / "forums";
    if (!std::filesystem::is_directory(forums)) return false;
    for (const std::filesystem::directory_entry& forum :
         std::filesystem::directory_iterator(forums)) {
        if (!forum.is_directory()) continue;
        const std::filesystem::path sessions = forum.path() / "sessions";
        if (contains_legacy_database(sessions)
            || contains_legacy_database(sessions / "deleted")) {
            return true;
        }
    }
    return false;
}

} // namespace cha
