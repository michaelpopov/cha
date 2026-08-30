#include "session/session_snapshot.h"

#include "session/session_database.h"
#include "util/path_name.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <vector>

namespace cha {
namespace {

std::filesystem::path with_extension(
    std::filesystem::path path,
    const std::filesystem::path& extension) {
    path.replace_extension(extension);
    return path;
}

} // namespace

void bootstrap_sessions_from_sql(
    const Workspace& workspace) {
    for (const WorkspaceForum& forum : workspace.forums()) {
        const std::optional<std::filesystem::path> directory =
            workspace.forum_session_directory(forum.id);
        if (!directory || !std::filesystem::exists(*directory)) continue;
        if (!std::filesystem::is_directory(*directory)) {
            throw std::runtime_error(
                "Sessions path '" + utf8_path(*directory)
                + "' is not a directory");
        }

        std::vector<std::filesystem::path> snapshots;
        for (const auto& entry :
             std::filesystem::directory_iterator(*directory)) {
            if (!entry.is_regular_file()) continue;
            const std::filesystem::path extension = entry.path().extension();
            const std::string id = utf8_path(entry.path().stem());
            if (!is_url_safe_identifier(id)) continue;
            if (extension == ".sql") snapshots.push_back(entry.path());
        }
        std::sort(snapshots.begin(), snapshots.end());

        for (const std::filesystem::path& snapshot : snapshots) {
            const std::string id = utf8_path(snapshot.stem());
            const std::filesystem::path database =
                with_extension(snapshot, ".sqlite3");
            const std::filesystem::path deleted =
                *directory / "deleted" / database.filename();
            if (std::filesystem::exists(database)
                || std::filesystem::exists(deleted)) continue;
            (void)import_session_database_sql(
                snapshot, database, {forum.id, id});
        }
    }
}

} // namespace cha
