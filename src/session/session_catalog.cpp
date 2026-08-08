#include "session/session_catalog.h"

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

std::string SessionCatalog::validated_label(
    const std::filesystem::path& path,
    const std::string& session_id) const {
    const SessionDatabaseMetadata metadata = read_session_database_metadata(path);
    validate_session_database_identity(path, {forum_name_, session_id}, metadata);
    return metadata.label;
}

std::vector<StoredSession> SessionCatalog::list() const {
    if (!std::filesystem::exists(directory_)) return {};
    if (!std::filesystem::is_directory(directory_)) {
        throw std::runtime_error("Sessions path '" + utf8_path(directory_) + "' is not a directory");
    }
    std::vector<StoredSession> result;
    for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".sqlite3") continue;
        const std::string id = utf8_path(entry.path().stem());
        if (!is_url_safe_identifier(id)) {
            log_warn("Invalid session database ignored: path=" + utf8_path(entry.path())
                + " reason=session ID is not URL-safe");
            continue;
        }
        // Path and timestamp come from the entry being inspected, so no caller
        // has to rebuild them; a timestamp that cannot be read fails the whole
        // listing rather than becoming one row's metadata error.
        StoredSession stored{
            .identity = {forum_name_, id},
            .label = id,
            .database_path = entry.path(),
            .updated_at = entry.last_write_time(),
        };
        try {
            stored.label = validated_label(entry.path(), id);
        } catch (const std::exception& exception) {
            stored.error = exception.what();
            stored.label += " [invalid database]";
            log_warn("Invalid session database ignored: path=" + utf8_path(entry.path())
                + " reason=" + stored.error);
        }
        result.push_back(std::move(stored));
    }
    std::sort(result.begin(), result.end(),
        [](const StoredSession& left, const StoredSession& right) {
            return left.identity.session_id < right.identity.session_id;
        });
    return result;
}

StoredSession SessionCatalog::inspect(const std::string& session_id) const {
    const std::filesystem::path path = database_path(session_id);
    if (!std::filesystem::is_regular_file(path)) {
        throw missing_session_error(session_id);
    }
    return {
        .identity = {forum_name_, session_id},
        .label = validated_label(path, session_id),
        .database_path = path,
        .updated_at = std::filesystem::last_write_time(path),
    };
}

StoredSession SessionCatalog::create(std::string label) const {
    std::filesystem::create_directories(directory_);
    const std::string base_id = timestamp_name(clock_());
    for (std::size_t suffix = 1;; ++suffix) {
        const std::string id = suffix == 1 ? base_id : base_id + "-" + std::to_string(suffix);
        const std::filesystem::path path = database_path(id);
        // An advisory fast path only: publication below is the authority, so a
        // destination appearing after this check is still detected as a
        // collision rather than overwritten.
        if (std::filesystem::exists(path)) continue;
        try {
            SessionLease lease = SessionLease::acquire(path);
            std::string effective_label = label.empty() ? id : label;
            if (create_session_database(path, {.id = id, .forum = forum_name_, .label = effective_label})) {
                // Publication has already committed, so a timestamp that cannot
                // be read is reported rather than rolled back.
                return {
                    .identity = {forum_name_, id},
                    .label = std::move(effective_label),
                    .database_path = path,
                    .updated_at = std::filesystem::last_write_time(path),
                };
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

} // namespace cha
