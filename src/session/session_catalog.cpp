#include "session/session_catalog.h"

#include "session/catalog_lease.h"
#include "session/session_lease.h"
#include "util/path_name.h"
#include "util/logging.h"
#include "util/utf8_path.h"
#include "session/session_database.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace cha {
namespace {

bool local_time(std::time_t now, std::tm& result) {
    // std::localtime is standard C++, but returns a pointer to shared storage.
    // Copy it while serializing this use so session-name generation remains
    // safe when catalogs create sessions on multiple threads.
    static std::mutex mutex;
    const std::lock_guard lock(mutex);
    const std::tm* const local = std::localtime(&now);
    if (local == nullptr) {
        return false;
    }
    result = *local;
    return true;
}

std::string timestamp_name(std::time_t now) {
    std::tm local{};
    if (!local_time(now, local)) {
        throw std::runtime_error("Failed to read local time for session name");
    }
    std::ostringstream result;
    result << std::put_time(&local, "%Y-%m-%d-%H-%M-%S") << "-session";
    return result.str();
}

void validate_metadata(
    const std::filesystem::path& path,
    std::string_view expected_id,
    std::string_view expected_forum,
    const SessionDatabaseMetadata& metadata) {

    if (metadata.id != expected_id) {
        throw std::runtime_error(
            "Session database '" + utf8_path(path)
            + "' does not match its filename");
    }
    if (metadata.forum != expected_forum) {
        throw std::runtime_error(
            "Session database '" + utf8_path(path) + "' does not belong to forum '"
            + std::string(expected_forum) + "'");
    }
}

} // namespace

SessionCatalog::SessionCatalog(
    std::filesystem::path directory,
    std::string forum_name,
    Clock clock)
    : directory_(std::move(directory)),
      forum_name_(std::move(forum_name)),
      clock_(std::move(clock)) {
    require_url_safe_identifier(forum_name_, directory_.parent_path());
    if (!clock_) {
        clock_ = [] { return std::time(nullptr); };
    }
}

Session SessionCatalog::validated_session(
    const std::filesystem::path& path,
    const std::string& session_id) const {
    const SessionDatabaseMetadata metadata = read_session_database_metadata(path);
    validate_metadata(path, session_id, forum_name_, metadata);
    return {session_id, metadata.label};
}

std::vector<Session> SessionCatalog::list() const {
    if (!std::filesystem::exists(directory_)) return {};
    if (!std::filesystem::is_directory(directory_)) {
        throw std::runtime_error("Sessions path '" + utf8_path(directory_) + "' is not a directory");
    }
    std::vector<Session> result;
    for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".sqlite3") continue;
        const std::string id = utf8_path(entry.path().stem());
        if (!is_url_safe_identifier(id)) {
            log_warn("Invalid session database ignored: path=" + utf8_path(entry.path())
                + " reason=session ID is not URL-safe");
            continue;
        }
        std::string label = id;
        std::string error;
        try {
            label = validated_session(entry.path(), id).label;
        } catch (const std::exception& exception) {
            error = exception.what();
            label += " [invalid database]";
            log_warn("Invalid session database ignored: path=" + utf8_path(entry.path())
                + " reason=" + error);
        }
        result.push_back({id, std::move(label), std::move(error)});
    }
    std::sort(result.begin(), result.end(), [](const Session& left, const Session& right) {
        return left.id < right.id;
    });
    return result;
}

Session SessionCatalog::session(const std::string& session_id) const {
    const std::filesystem::path path = database_path(session_id);
    if (!std::filesystem::is_regular_file(path)) {
        throw SessionNotFoundError(
            "Session '" + session_id + "' does not have a database");
    }
    return validated_session(path, session_id);
}

Session SessionCatalog::create(std::string label) const {
    std::filesystem::create_directories(directory_);
    CatalogLease catalog_lease = CatalogLease::acquire(directory_);
    const std::string base_id = timestamp_name(clock_());
    for (std::size_t suffix = 1;; ++suffix) {
        const std::string id = suffix == 1 ? base_id : base_id + "-" + std::to_string(suffix);
        const std::filesystem::path path = database_path(id);
        if (std::filesystem::exists(path)) continue;
        try {
            SessionLease lease = SessionLease::acquire(path);
            const std::string effective_label = label.empty() ? id : label;
            if (create_session_database(path, {.id = id, .forum = forum_name_, .label = effective_label})) {
                return {id, effective_label};
            }
        } catch (const SessionBusyError&) {
            // A stale companion file is harmless; its held kernel lock means
            // this stem cannot be used by this creation attempt.
        }
    }
}

std::filesystem::path SessionCatalog::database_path(
    const std::string& session_id) const {
    require_url_safe_identifier(session_id, directory_);
    return directory_ / path_from_utf8(session_id + ".sqlite3");
}

std::filesystem::path SessionCatalog::open_database_path(
    const std::string& session_id) const {

    const std::filesystem::path path = database_path(session_id);
    if (!std::filesystem::is_regular_file(path)) {
        throw SessionNotFoundError(
            "Session '" + session_id + "' does not have a database");
    }
    (void)validated_session(path, session_id);
    return path;
}

} // namespace cha
