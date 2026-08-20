#include "session/session_snapshot.h"

#include "session/session_database.h"
#include "util/path_name.h"

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

void export_and_bootstrap_sessions(
    const std::vector<ForumSessionDirectory>& directories) {
    for (const ForumSessionDirectory& forum : directories) {
        if (!std::filesystem::exists(forum.directory)) continue;
        if (!std::filesystem::is_directory(forum.directory)) {
            throw std::runtime_error(
                "Sessions path '" + utf8_path(forum.directory)
                + "' is not a directory");
        }

        std::vector<std::filesystem::path> databases;
        std::vector<std::filesystem::path> snapshots;
        for (const auto& entry :
             std::filesystem::directory_iterator(forum.directory)) {
            if (!entry.is_regular_file()) continue;
            const std::filesystem::path extension = entry.path().extension();
            const std::string id = utf8_path(entry.path().stem());
            if (!is_url_safe_identifier(id)) continue;
            if (extension == ".sqlite3") databases.push_back(entry.path());
            else if (extension == ".sql") snapshots.push_back(entry.path());
        }
        std::sort(databases.begin(), databases.end());
        std::sort(snapshots.begin(), snapshots.end());

        for (const std::filesystem::path& database : databases) {
            const std::string id = utf8_path(database.stem());
            export_session_database_sql(
                database,
                with_extension(database, ".sql"),
                {forum.forum_id, id});
        }

        for (const std::filesystem::path& snapshot : snapshots) {
            const std::string id = utf8_path(snapshot.stem());
            const std::filesystem::path database =
                with_extension(snapshot, ".sqlite3");
            const std::filesystem::path deleted =
                forum.directory / "deleted" / database.filename();
            if (std::filesystem::exists(database)
                || std::filesystem::exists(deleted)) continue;
            (void)import_session_database_sql(
                snapshot, database, {forum.forum_id, id});
        }
    }
}

} // namespace cha
