#include "storage/session_repository.h"

#include "util/path_name.h"
#include "storage/session_database.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace cha {
namespace {

std::string timestamp_name(std::time_t now) {
    std::tm local{};
    if (::localtime_r(&now, &local) == nullptr) {
        throw std::runtime_error("Failed to read local time for session name");
    }
    std::ostringstream result;
    result << std::put_time(&local, "%Y-%m-%d-%H-%M-%S") << "-session";
    return result.str();
}

void validate_metadata(
    const std::filesystem::path& path,
    std::string_view expected_id,
    std::string_view expected_room,
    const SessionDatabaseMetadata& metadata) {

    if (metadata.id != expected_id) {
        throw std::runtime_error(
            "Session database '" + path.string()
            + "' does not match its filename");
    }
    if (metadata.room != expected_room) {
        throw std::runtime_error(
            "Session database '" + path.string() + "' does not belong to room '"
            + std::string(expected_room) + "'");
    }
}

} // namespace

SessionRepository::SessionRepository(
    std::filesystem::path directory,
    std::string room_name,
    Clock clock)
    : directory_(std::move(directory)),
      room_name_(std::move(room_name)),
      clock_(std::move(clock)) {
    if (!clock_) {
        clock_ = [] { return std::time(nullptr); };
    }
}

std::vector<Session> SessionRepository::list() const {
    if (!std::filesystem::exists(directory_)) {
        return {};
    }
    if (!std::filesystem::is_directory(directory_)) {
        throw std::runtime_error(
            "Sessions path '" + directory_.string() + "' is not a directory");
    }

    std::vector<Session> result;
    for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".sqlite3") {
            continue;
        }
        const std::string id = entry.path().stem().string();

        std::string label = id;
        std::string error;
        try {
            require_path_component(id, directory_);
            const SessionDatabaseMetadata metadata =
                read_session_database_metadata(entry.path());
            validate_metadata(
                entry.path(),
                id,
                room_name_,
                metadata);
            label = metadata.label;
        } catch (const std::exception& exception) {
            error = exception.what();
            label += " [invalid database]";
        }
        result.push_back({id, std::move(label), std::move(error)});
    }
    std::sort(result.begin(), result.end(), [](const Session& left, const Session& right) {
        return left.id < right.id;
    });
    return result;
}

Session SessionRepository::create(std::string label) const {
    std::filesystem::create_directories(directory_);

    const std::string base_id = timestamp_name(clock_());
    std::string id = base_id;
    for (std::size_t suffix = 2;; ++suffix) {
        const std::string effective_label = label.empty() ? id : label;
        if (create_session_database(
                database_path(id),
                {
                    .id = id,
                    .room = room_name_,
                    .label = effective_label,
                })) {
            return {id, effective_label};
        }
        id = base_id + "-" + std::to_string(suffix);
    }
}

std::filesystem::path SessionRepository::database_path(
    const std::string& session_id) const {
    require_path_component(session_id, directory_);
    return directory_ / (session_id + ".sqlite3");
}

std::filesystem::path SessionRepository::open_database_path(
    const std::string& session_id) const {

    const std::filesystem::path path = database_path(session_id);
    if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error(
            "Session '" + session_id + "' does not have a database");
    }
    const SessionDatabaseMetadata metadata =
        read_session_database_metadata(path);
    validate_metadata(
        path,
        session_id,
        room_name_,
        metadata);
    return path;
}

} // namespace cha
