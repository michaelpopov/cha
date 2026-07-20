#include "session_repository.h"

#include "conversation_file.h"
#include "path_name.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <fcntl.h>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unistd.h>

namespace cha {
namespace {

constexpr std::int64_t session_metadata_version = 1;

// Holds the strictly validated metadata associated with one persisted session pair.
struct SessionMetadata {
    std::string room;
    std::string persona;
    std::string label;
};

std::string toml_string(std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2);
    result.push_back('"');
    for (const char character : value) {
        if (character == '"' || character == '\\') {
            result.push_back('\\');
        }
        result.push_back(character);
    }
    result.push_back('"');
    return result;
}

std::string timestamp_name() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    if (::localtime_r(&now, &local) == nullptr) {
        throw std::runtime_error("Failed to read local time for session name");
    }
    std::ostringstream result;
    result << std::put_time(&local, "%Y-%m-%d-%H-%M-%S") << "-session";
    return result.str();
}

SessionMetadata read_metadata(
    const std::filesystem::path& path,
    std::string_view expected_room,
    std::string_view expected_persona) {

    toml::table meta;
    try {
        meta = toml::parse_file(path.string());
    } catch (const toml::parse_error& error) {
        throw std::runtime_error(
            "Failed to parse session metadata '" + path.string() + "': "
            + std::string(error.description()));
    }

    const auto version = meta["version"].value<std::int64_t>();
    const auto room = meta["room"].value<std::string>();
    const auto persona = meta["persona"].value<std::string>();
    const auto label = meta["label"].value<std::string>();
    if (!version || *version != session_metadata_version) {
        throw std::runtime_error(
            "Session metadata '" + path.string() + "' requires version "
            + std::to_string(session_metadata_version));
    }
    if (!room || *room != expected_room) {
        throw std::runtime_error(
            "Session metadata '" + path.string() + "' does not belong to room '"
            + std::string(expected_room) + "'");
    }
    if (!persona || *persona != expected_persona) {
        throw std::runtime_error(
            "Session metadata '" + path.string() + "' does not belong to persona '"
            + std::string(expected_persona) + "'");
    }
    if (!label || label->empty()) {
        throw std::runtime_error(
            "Session metadata '" + path.string() + "' requires a non-empty string label");
    }
    return {*room, *persona, *label};
}

void write_exclusive_file(const std::filesystem::path& path, std::string_view contents) {
    const int descriptor = ::open(
        path.c_str(),
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
        0600);
    if (descriptor == -1) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "Failed to create session file '" + path.string() + "'");
    }

    try {
        while (!contents.empty()) {
            const ssize_t written = ::write(descriptor, contents.data(), contents.size());
            if (written == -1 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                throw std::system_error(
                    errno,
                    std::generic_category(),
                    "Failed to write session file '" + path.string() + "'");
            }
            contents.remove_prefix(static_cast<std::size_t>(written));
        }
        while (::fsync(descriptor) == -1) {
            if (errno != EINTR) {
                throw std::system_error(
                    errno,
                    std::generic_category(),
                    "Failed to sync session file '" + path.string() + "'");
            }
        }
    } catch (...) {
        ::close(descriptor);
        throw;
    }
    if (::close(descriptor) == -1) {
        throw std::system_error(
            errno,
            std::generic_category(),
            "Failed to close session file '" + path.string() + "'");
    }
}

std::filesystem::path temporary_metadata_path(
    const std::filesystem::path& directory,
    std::string_view id) {

    const std::string base = "." + std::string(id) + ".meta.tmp-" + std::to_string(::getpid());
    std::filesystem::path path = directory / base;
    for (std::size_t suffix = 2; std::filesystem::exists(path); ++suffix) {
        path = directory / (base + "-" + std::to_string(suffix));
    }
    return path;
}

} // namespace

SessionRepository::SessionRepository(std::filesystem::path directory, std::string room_name, std::string persona_name)
    : directory_(std::move(directory)),
      room_name_(std::move(room_name)),
      persona_name_(std::move(persona_name)) {
}

std::vector<Session> SessionRepository::list() const {
    if (!std::filesystem::exists(directory_)) {
        return {};
    }
    if (!std::filesystem::is_directory(directory_)) {
        throw std::runtime_error("Sessions path '" + directory_.string() + "' is not a directory");
    }

    std::vector<Session> result;
    for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".data") {
            continue;
        }
        const std::string id = entry.path().stem().string();
        require_path_component(id, directory_);
        const std::filesystem::path meta_path = directory_ / (id + ".meta");
        if (!std::filesystem::is_regular_file(meta_path)) {
            continue;
        }

        std::string label = id;
        std::string metadata_error;
        try {
            label = read_metadata(meta_path, room_name_, persona_name_).label;
        } catch (const std::exception& error) {
            metadata_error = error.what();
            label += " [invalid metadata]";
        }
        result.push_back({id, std::move(label), std::move(metadata_error)});
    }
    std::sort(result.begin(), result.end(), [](const Session& left, const Session& right) {
        return left.id < right.id;
    });
    return result;
}

Session SessionRepository::create(std::string label) const {
    std::filesystem::create_directories(directory_);

    const std::string base_id = timestamp_name();
    std::string id = base_id;
    for (std::size_t suffix = 2;; ++suffix) {
        const std::filesystem::path meta_path = directory_ / (id + ".meta");
        const std::filesystem::path data = data_path(id);
        if (std::filesystem::exists(meta_path) || std::filesystem::exists(data)) {
            id = base_id + "-" + std::to_string(suffix);
            continue;
        }

        const std::string effective_label = label.empty() ? id : label;
        const std::string metadata =
            "version = " + std::to_string(session_metadata_version) + "\n"
            + "room = " + toml_string(room_name_) + "\n"
            + "persona = " + toml_string(persona_name_) + "\n"
            + "label = " + toml_string(effective_label) + "\n";
        const std::filesystem::path temporary_meta =
            temporary_metadata_path(directory_, id);

        try {
            create_conversation_file_exclusive(data);
        } catch (const std::system_error& error) {
            if (error.code() == std::errc::file_exists) {
                id = base_id + "-" + std::to_string(suffix);
                continue;
            }
            std::error_code ignored;
            std::filesystem::remove(data, ignored);
            throw;
        }

        try {
            write_exclusive_file(temporary_meta, metadata);
            std::filesystem::create_hard_link(temporary_meta, meta_path);
        } catch (const std::system_error& error) {
            std::error_code ignored;
            std::filesystem::remove(temporary_meta, ignored);
            std::filesystem::remove(data, ignored);
            if (error.code() == std::errc::file_exists) {
                id = base_id + "-" + std::to_string(suffix);
                continue;
            }
            throw;
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(temporary_meta, ignored);
            std::filesystem::remove(data, ignored);
            throw;
        }

        std::error_code ignored;
        std::filesystem::remove(temporary_meta, ignored);
        return {id, effective_label};
    }
}

std::filesystem::path SessionRepository::data_path(const std::string& session_id) const {
    require_path_component(session_id, directory_);
    return directory_ / (session_id + ".data");
}

std::filesystem::path SessionRepository::open_data_path(const std::string& session_id) const {
    const std::filesystem::path data = data_path(session_id);
    const std::filesystem::path meta = directory_ / (session_id + ".meta");
    if (!std::filesystem::is_regular_file(data)
        || !std::filesystem::is_regular_file(meta)) {
        throw std::runtime_error(
            "Session '" + session_id + "' does not have a complete metadata and data pair");
    }
    (void)read_metadata(meta, room_name_, persona_name_);
    return data;
}

} // namespace cha
