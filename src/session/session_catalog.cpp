#include "session/session_catalog.h"

#include "session/catalog_lease.h"
#include "util/path_name.h"
#include "util/public_name.h"
#include "util/text.h"
#include "util/logging.h"
#include "util/utf8_path.h"
#include "session/session_database.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <optional>
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

void validate_requested_session_name(std::string_view name) {
    try {
        validate_public_name(name, "Session name", "requested session");
    } catch (const std::exception&) {
        throw std::runtime_error("Session name is not a valid public name");
    }
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

std::vector<Session> SessionCatalog::list_by_name() const {
    if (!std::filesystem::exists(directory_)) {
        return {};
    }
    if (!std::filesystem::is_directory(directory_)) {
        throw std::runtime_error(
            "Sessions path '" + utf8_path(directory_) + "' is not a directory");
    }

    std::vector<Session> result;
    for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".sqlite3") {
            continue;
        }
        const std::string id = utf8_path(entry.path().stem());
        if (!is_url_safe_identifier(id)) {
            log_warn(
                "Invalid session database ignored: path=" + utf8_path(entry.path())
                + " reason=session ID is not URL-safe");
            continue;
        }

        std::string label = "Unavailable session";
        std::string error;
        try {
            const std::string candidate = validated_session(entry.path(), id).label;
            validate_public_name(candidate, "Session name", entry.path());
            label = candidate;
        } catch (const std::exception& exception) {
            error = "Stored session is corrupt";
            log_warn(
                "Invalid session database ignored: path=" + utf8_path(entry.path())
                + " reason=" + exception.what());
        }
        result.push_back({id, std::move(label), std::move(error)});
    }
    std::sort(result.begin(), result.end(), [](const Session& left, const Session& right) {
        const std::string left_folded = fold_ascii(left.label);
        const std::string right_folded = fold_ascii(right.label);
        return left_folded == right_folded ? left.id < right.id : left_folded < right_folded;
    });
    for (std::size_t first = 0; first < result.size();) {
        if (!result[first].error.empty()) {
            ++first;
            continue;
        }
        std::size_t last = first + 1;
        while (last < result.size()
               && ( !result[last].error.empty()
                   || fold_ascii(result[first].label) == fold_ascii(result[last].label))) {
            ++last;
        }
        std::size_t healthy_count{};
        for (std::size_t index = first; index < last; ++index) {
            if (result[index].error.empty()) ++healthy_count;
        }
        if (healthy_count > 1) {
            for (std::size_t index = first; index < last; ++index) {
                if (result[index].error.empty()) result[index].ambiguous = true;
            }
        }
        first = last;
    }
    return result;
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
    if (!label.empty()) return create_by_name(std::move(label)).session;

    std::filesystem::create_directories(directory_);
    CatalogLease catalog_lease = CatalogLease::acquire(directory_);
    const std::string base_id = timestamp_name(clock_());
    for (std::size_t suffix = 2;; ++suffix) {
        const std::string id = suffix == 2 ? base_id : base_id + "-" + std::to_string(suffix);
        const std::filesystem::path path = database_path(id);
        if (std::filesystem::exists(path)) continue;
        try {
            SessionLease lease = SessionLease::acquire(path);
            if (create_session_database(path, {.id = id, .forum = forum_name_, .label = id})) {
                return {id, id};
            }
        } catch (const SessionBusyError&) {
            // A stale companion file is harmless; its held kernel lock means
            // this stem cannot be used by this creation attempt.
        }
    }
}

Session SessionCatalog::session_by_name(const std::string& name) const {
    validate_requested_session_name(name);
    std::vector<Session> matches;
    for (const Session& candidate : list_by_name()) {
        if (candidate.error.empty() && fold_ascii(candidate.label) == fold_ascii(name)) matches.push_back(candidate);
    }
    if (matches.empty()) {
        throw SessionNotFoundError("Session '" + name + "' does not exist in forum '" + forum_name_ + "'");
    }
    if (matches.size() != 1 || matches.front().ambiguous) {
        throw SessionNameAmbiguousError("Session name '" + name + "' is ambiguous in forum '" + forum_name_ + "'");
    }
    return matches.front();
}

PreparedSession SessionCatalog::create_by_name(std::string name) const {
    validate_requested_session_name(name);
    std::filesystem::create_directories(directory_);
    // The only nested lock order is CatalogLease then a lease for a new,
    // unpublished session. Listing and opening intentionally take no catalog lease.
    CatalogLease catalog_lease = CatalogLease::acquire(directory_);
    for (const Session& existing : list_by_name()) {
        if (existing.error.empty() && fold_ascii(existing.label) == fold_ascii(name)) {
            throw SessionNameExistsError("Session '" + name + "' already exists in forum '" + forum_name_ + "'; use /open");
        }
    }
    const std::string base_id = timestamp_name(clock_());
    std::string id = base_id;
    for (std::size_t suffix = 2;; ++suffix) {
        const std::filesystem::path path = database_path(id);
        if (std::filesystem::exists(path)) {
            id = base_id + "-" + std::to_string(suffix);
            continue;
        }
        std::optional<SessionLease> lease;
        try {
            lease.emplace(SessionLease::acquire(path));
        } catch (const SessionBusyError&) {
            if (!std::filesystem::exists(path)) throw;
        }
        if (!lease) {
            id = base_id + "-" + std::to_string(suffix);
            continue;
        }
        if (create_session_database(path,
                {
                    .id = id,
                    .forum = forum_name_,
                    .label = name,
                })) {
            return PreparedSession({id, name}, path, std::move(*lease));
        }
        id = base_id + "-" + std::to_string(suffix);
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
