#include "storage/workspace.h"

#include "util/path_name.h"
#include "util/text.h"

#include <fstream>
#include <stdexcept>
#include <unordered_set>
#include <string_view>

namespace cha {

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
        const std::string_view name = trim_view(line);
        if (name.empty() || name.front() == '#') {
            continue;
        }
        require_path_component(name, list_path);
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
    const std::vector<std::string> persona_names = read_name_list(directory / "personas.list");
    return {name, persona_names, directory};
}

std::filesystem::path Workspace::persona_directory(std::string_view persona_name) const {
    require_path_component(persona_name, root_ / "personas");
    const std::filesystem::path persona_directory = root_ / "personas" / std::string(persona_name);
    if (!std::filesystem::is_directory(persona_directory)) {
        throw std::runtime_error("Persona '" + std::string(persona_name) + "' does not exist");
    }
    return persona_directory;
}

std::filesystem::path Workspace::room_directory(const std::string& name) const {
    require_path_component(name, root_ / "rooms" / "rooms.list");
    return root_ / "rooms" / name;
}

std::vector<std::string> Workspace::read_name_list(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error("Failed to read personas list '" + path.string() + "'");
    }
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;
    std::string line;
    while (std::getline(file, line)) {
        const std::string_view value = trim_view(line);
        if (value.empty() || value.front() == '#') {
            continue;
        }
        require_path_component(value, path);
        if (!seen.insert(std::string(value)).second) {
            throw std::runtime_error("Personas list '" + path.string() + "' names persona '" + std::string(value) + "' more than once");
        }
        result.emplace_back(value);
    }
    if (result.empty()) {
        throw std::runtime_error("Personas list '" + path.string() + "' does not name a persona");
    }
    return result;
}

} // namespace cha
