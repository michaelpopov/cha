#include "session/workspace.h"

#include "agents/agent.h"
#include "session/session_controller.h"
#include "session/session_database.h"
#include "session/session_catalog.h"
#include "util/path_name.h"
#include "util/text.h"

#include <fstream>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace cha {
namespace {

std::vector<AgentDefinition> load_definitions(
    const Workspace& workspace,
    const Room& room,
    const std::filesystem::path& base_config_candidate) {
    std::vector<std::filesystem::path> persona_directories;
    persona_directories.reserve(room.persona_names.size());
    for (const std::string& persona : room.persona_names) {
        persona_directories.push_back(workspace.persona_directory(persona));
    }
    const std::optional<std::filesystem::path> base_config =
        std::filesystem::exists(base_config_candidate)
        ? std::optional<std::filesystem::path>(base_config_candidate)
        : std::nullopt;
    return load_agent_definitions(
        persona_directories, room.directory, base_config);
}

SessionCatalog session_catalog(
    const Workspace& workspace,
    const std::string& room_name) {
    const Room room = workspace.load_room(room_name);
    return SessionCatalog(room.directory / "sessions", room.name);
}

SessionSummary summarize(const Session& stored) {
    return {
        .id = stored.id,
        .label = stored.label,
        .error = stored.error,
    };
}

} // namespace

Workspace::Workspace(std::filesystem::path root)
    : root_(std::move(root)) {
    if (!std::filesystem::is_directory(root_ / "personas")
        || !std::filesystem::is_directory(root_ / "rooms")) {
        throw std::runtime_error(
            "Workspace '" + root_.string()
            + "' requires personas/ and rooms/ directories");
    }
}

std::vector<std::string> Workspace::rooms() const {
    const std::filesystem::path list_path = root_ / "rooms" / "rooms.list";
    std::ifstream file(list_path);
    if (!file) {
        throw std::runtime_error(
            "Failed to read rooms list '" + list_path.string() + "'");
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
            throw std::runtime_error(
                "Room '" + std::string(name) + "' listed in '"
                + list_path.string() + "' does not exist");
        }
        result.emplace_back(name);
    }
    if (result.empty()) {
        throw std::runtime_error(
            "Rooms list '" + list_path.string() + "' does not name a room");
    }
    return result;
}

Room Workspace::load_room(const std::string& name) const {
    const std::filesystem::path directory = room_directory(name);
    if (!std::filesystem::is_directory(directory)) {
        throw std::runtime_error("Room '" + name + "' does not exist");
    }
    const std::vector<std::string> persona_names =
        read_name_list(directory / "personas.list");
    return {name, persona_names, directory};
}

std::filesystem::path Workspace::persona_directory(
    std::string_view persona_name) const {
    require_path_component(persona_name, root_ / "personas");
    const std::filesystem::path directory =
        root_ / "personas" / std::string(persona_name);
    if (!std::filesystem::is_directory(directory)) {
        throw std::runtime_error(
            "Persona '" + std::string(persona_name) + "' does not exist");
    }
    return directory;
}

std::filesystem::path Workspace::room_directory(
    const std::string& name) const {
    require_path_component(name, root_ / "rooms" / "rooms.list");
    return root_ / "rooms" / name;
}

std::vector<std::string> Workspace::read_name_list(
    const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        throw std::runtime_error(
            "Failed to read personas list '" + path.string() + "'");
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
            throw std::runtime_error(
                "Personas list '" + path.string() + "' names persona '"
                + std::string(value) + "' more than once");
        }
        result.emplace_back(value);
    }
    if (result.empty()) {
        throw std::runtime_error(
            "Personas list '" + path.string() + "' does not name a persona");
    }
    return result;
}

std::vector<SessionSummary> Workspace::sessions(
    const std::string& room_name) const {
    const SessionCatalog catalog = session_catalog(*this, room_name);
    const std::vector<Session> stored = catalog.list();

    std::vector<SessionSummary> result;
    result.reserve(stored.size());
    for (const Session& session : stored) {
        result.push_back(summarize(session));
    }
    return result;
}

CreatedSession Workspace::create_session(
    const std::string& room_name,
    std::string label) const {
    const Room room = load_room(room_name);
    std::vector<AgentDefinition> definitions = load_definitions(
        *this, room, root_ / "personas" / "base_config.toml");
    const SessionCatalog catalog(room.directory / "sessions", room.name);

    const Session session = catalog.create(std::move(label));
    return {
        .controller = SessionController::from_definitions(
            std::move(definitions),
            catalog.database_path(session.id)),
        .id = session.id,
    };
}

std::unique_ptr<SessionController> Workspace::open_session(
    const std::string& room_name,
    const std::string& session_id) const {
    const Room room = load_room(room_name);
    std::vector<AgentDefinition> definitions = load_definitions(
        *this, room, root_ / "personas" / "base_config.toml");
    const SessionCatalog catalog(room.directory / "sessions", room.name);

    const std::filesystem::path database_path =
        catalog.open_database_path(session_id);
    SessionRestore restored = load_session_state(database_path);
    return SessionController::from_definitions(
        std::move(definitions),
        database_path,
        std::move(restored));
}

} // namespace cha
