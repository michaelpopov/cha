#include "session_repository.h"

#include "conversation_file.h"
#include "path_name.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace cha {
namespace {

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
            const toml::table meta = toml::parse_file(meta_path.string());
            if (const auto configured_label = meta["label"].value<std::string>()) {
                label = *configured_label;
            }
        } catch (const toml::parse_error& error) {
            metadata_error = "Failed to parse session metadata '" + meta_path.string() + "': "
                + std::string(error.description());
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
    for (std::size_t suffix = 2; std::filesystem::exists(directory_ / (id + ".meta")); ++suffix) {
        id = base_id + "-" + std::to_string(suffix);
    }
    if (label.empty()) {
        label = id;
    }

    std::ofstream meta(directory_ / (id + ".meta"));
    if (!meta) {
        throw std::runtime_error("Failed to create session metadata in '" + directory_.string() + "'");
    }
    meta << "version = 1\n"
         << "room = " << toml_string(room_name_) << "\n"
         << "persona = " << toml_string(persona_name_) << "\n"
         << "label = " << toml_string(label) << "\n";
    if (!meta) {
        throw std::runtime_error("Failed to write session metadata in '" + directory_.string() + "'");
    }
    prepare_conversation_file(data_path(id));
    return {id, std::move(label)};
}

std::filesystem::path SessionRepository::data_path(const std::string& session_id) const {
    require_path_component(session_id, directory_);
    return directory_ / (session_id + ".data");
}

} // namespace cha
