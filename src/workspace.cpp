#include "workspace.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace cha {
namespace {

std::string_view trim(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.remove_prefix(1);
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.remove_suffix(1);
    }
    return value;
}

void require_name(std::string_view name, const std::filesystem::path& source) {
    const std::filesystem::path path{name};
    if (name.empty() || path.is_absolute() || path.has_parent_path() || name == "." || name == "..") {
        throw std::runtime_error("Invalid name '" + std::string(name) + "' in '" + source.string() + "'");
    }
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to read '" + path.string() + "'");
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

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

Workspace::Workspace(std::filesystem::path root) : root_(std::move(root)) {
    if (!std::filesystem::is_directory(root_ / "personas") || !std::filesystem::is_directory(root_ / "rooms")) {
        throw std::runtime_error("Workspace '" + root_.string() + "' requires personas/ and rooms/ directories");
    }
}

std::vector<std::string> Workspace::rooms() const {
    const std::filesystem::path list_path = root_ / "rooms" / "rooms.list";
    std::ifstream file(list_path);
    if (!file) {
        throw std::runtime_error("Failed to read rooms list '" + list_path.string() + "'");
    }

    std::vector<std::string> result;
    std::string line;
    while (std::getline(file, line)) {
        const std::string_view name = trim(line);
        if (name.empty() || name.front() == '#') {
            continue;
        }
        require_name(name, list_path);
        const std::filesystem::path directory = room_directory(std::string(name));
        if (!std::filesystem::is_directory(directory)) {
            throw std::runtime_error("Room '" + std::string(name) + "' listed in '" + list_path.string() + "' does not exist");
        }
        result.emplace_back(name);
    }
    if (result.empty()) {
        throw std::runtime_error("Rooms list '" + list_path.string() + "' does not name a room");
    }
    return result;
}

Room Workspace::load_room(const std::string& name) const {
    const std::filesystem::path directory = room_directory(name);
    if (!std::filesystem::is_directory(directory)) {
        throw std::runtime_error("Room '" + name + "' does not exist");
    }
    const std::string persona_name = read_single_name_list(directory / "personas.list");
    const std::filesystem::path persona_directory = root_ / "personas" / persona_name;
    if (!std::filesystem::is_directory(persona_directory)) {
        throw std::runtime_error("Persona '" + persona_name + "' selected by room '" + name + "' does not exist");
    }

    const std::filesystem::path system_path = persona_directory / "SYSTEM.md";
    const std::filesystem::path user_path = directory / "USER.md";
    Config config = Config::load(persona_directory / "config.toml");
    config.name = persona_name;
    config.system_prompt = read_text(system_path) + "\n\n" + read_text(user_path);
    return {name, persona_name, std::move(config), directory};
}

std::vector<Session> Workspace::sessions(const Room& room) const {
    const std::filesystem::path directory = room.directory / "sessions";
    if (!std::filesystem::exists(directory)) {
        return {};
    }
    if (!std::filesystem::is_directory(directory)) {
        throw std::runtime_error("Sessions path '" + directory.string() + "' is not a directory");
    }

    std::vector<Session> result;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".data") {
            continue;
        }
        const std::string id = entry.path().stem().string();
        require_name(id, directory);
        const std::filesystem::path meta_path = directory / (id + ".meta");
        if (std::filesystem::is_regular_file(meta_path)) {
            std::string label = id;
            try {
                const toml::table meta = toml::parse_file(meta_path.string());
                if (const auto configured_label = meta["label"].value<std::string>()) {
                    label = *configured_label;
                }
            } catch (const toml::parse_error& error) {
                throw std::runtime_error(
                    "Failed to parse session metadata '" + meta_path.string() + "': "
                    + std::string(error.description()));
            }
            result.push_back({id, std::move(label)});
        }
    }
    std::sort(result.begin(), result.end(), [](const Session& left, const Session& right) {
        return left.id < right.id;
    });
    return result;
}

Session Workspace::create_session(const Room& room, std::string label) const {
    const std::filesystem::path directory = room.directory / "sessions";
    std::filesystem::create_directories(directory);

    const std::string base_id = timestamp_name();
    std::string id = base_id;
    for (std::size_t suffix = 2; std::filesystem::exists(directory / (id + ".meta")); ++suffix) {
        id = base_id + "-" + std::to_string(suffix);
    }
    if (label.empty()) {
        label = id;
    }

    std::ofstream meta(directory / (id + ".meta"));
    if (!meta) {
        throw std::runtime_error("Failed to create session metadata in '" + directory.string() + "'");
    }
    meta << "version = 1\n"
         << "room = \"" << room.name << "\"\n"
         << "persona = \"" << room.persona_name << "\"\n"
         << "label = " << toml_string(label) << "\n";
    if (!meta) {
        throw std::runtime_error("Failed to write session metadata in '" + directory.string() + "'");
    }
    return {id, std::move(label)};
}

std::filesystem::path Workspace::session_data_path(const Room& room, const std::string& session) const {
    require_name(session, room.directory / "sessions");
    const std::filesystem::path path = room.directory / "sessions" / (session + ".data");
    if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error("Session data file '" + path.string() + "' does not exist");
    }
    return path;
}

std::filesystem::path Workspace::room_directory(const std::string& name) const {
    require_name(name, root_ / "rooms" / "rooms.list");
    return root_ / "rooms" / name;
}

std::string Workspace::read_single_name_list(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("Failed to read personas list '" + path.string() + "'");
    }
    std::string result;
    std::string line;
    while (std::getline(file, line)) {
        const std::string_view value = trim(line);
        if (value.empty() || value.front() == '#') {
            continue;
        }
        require_name(value, path);
        if (!result.empty()) {
            throw std::runtime_error("Personas list '" + path.string() + "' must name exactly one persona");
        }
        result = value;
    }
    if (result.empty()) {
        throw std::runtime_error("Personas list '" + path.string() + "' does not name a persona");
    }
    return result;
}

} // namespace cha
