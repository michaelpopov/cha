#include "session/session_storage_layout.h"

#include "util/path_name.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cha {
namespace {

void scan_directory(
    const std::filesystem::path& directory,
    std::string_view forum_id,
    bool archived,
    std::vector<LegacySessionSource>& result) {
    if (!std::filesystem::exists(directory)) return;
    if (!std::filesystem::is_directory(directory)) {
        throw std::runtime_error(
            "Legacy sessions path '" + utf8_path(directory)
            + "' is not a directory");
    }
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".sqlite3") {
            continue;
        }
        result.push_back({
            .path = entry.path(),
            .expected_identity = {
                std::string(forum_id),
                utf8_path(entry.path().stem()),
            },
            .archived = archived,
        });
    }
}

} // namespace

std::filesystem::path workspace_session_database_path(
    const Workspace& workspace) {
    return workspace.root() / "sessions.sqlite3";
}

std::filesystem::path workspace_session_migration_path(
    const Workspace& workspace) {
    return workspace.root() / ".sessions.sqlite3.migrating";
}

std::vector<LegacySessionSource> discover_legacy_session_sources(
    const Workspace& workspace) {
    std::vector<LegacySessionSource> result;
    for (const WorkspaceForum& forum : workspace.forums()) {
        const auto sessions = workspace.forum_session_directory(forum.id);
        if (!sessions) continue;
        scan_directory(*sessions, forum.id, false, result);
        scan_directory(*sessions / "deleted", forum.id, true, result);
    }
    std::sort(result.begin(), result.end(),
        [](const LegacySessionSource& left, const LegacySessionSource& right) {
            return utf8_path(left.path) < utf8_path(right.path);
        });
    return result;
}

} // namespace cha
