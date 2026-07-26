#include "session/workspace.h"

#include "agents/agent.h"
#include "session/session_controller.h"
#include "session/session_database.h"
#include "session/session_catalog.h"
#include "util/path_name.h"
#include "util/text.h"
#include "util/utf8_path.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace cha {
namespace {

std::vector<AgentDefinition> load_definitions(
    const Forum& forum,
    const std::filesystem::path& base_config_candidate) {
    std::vector<std::filesystem::path> persona_directories;
    persona_directories.reserve(forum.persona_names.size());
    for (const std::string& persona : forum.persona_names) {
        persona_directories.push_back(
            forum.directory / "personas" / path_from_utf8(persona));
    }
    const std::optional<std::filesystem::path> base_config =
        std::filesystem::exists(base_config_candidate)
        ? std::optional<std::filesystem::path>(base_config_candidate)
        : std::nullopt;
    return load_agent_definitions(
        persona_directories, forum.directory, base_config);
}

SessionCatalog session_catalog(
    const Workspace& workspace,
    const std::string& forum_name) {
    const Forum forum = workspace.load_forum(forum_name);
    return SessionCatalog(forum.directory / "sessions", forum.name);
}

SessionSummary summarize(const Session& stored) {
    return {
        .id = stored.id,
        .label = stored.label,
        .error = stored.error,
    };
}

std::vector<std::string> subdirectory_names(
    const std::filesystem::path& directory,
    std::string_view kind) {
    std::vector<std::string> result;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory)) {
        if (!entry.is_directory()) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        require_path_component(name, directory);
        result.push_back(name);
    }
    std::sort(result.begin(), result.end());
    if (result.empty()) {
        throw std::runtime_error(
            std::string(kind) + " directory '" + utf8_path(directory)
            + "' does not contain an entry");
    }
    return result;
}

std::string load_display_name(const std::filesystem::path& directory) {
    const std::filesystem::path path = directory / "config.toml";
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error(
            "Failed to read forum config '" + utf8_path(path) + "'");
    }
    const toml::table table = toml::parse(file, utf8_path(path));
    const std::optional<std::string> display_name =
        table["display_name"].value<std::string>();
    if (!display_name || display_name->empty()) {
        throw std::runtime_error(
            "Forum config '" + utf8_path(path)
            + "' requires a non-empty string 'display_name'");
    }
    return *display_name;
}

} // namespace

Workspace::Workspace(std::filesystem::path root)
    : root_(std::move(root)) {
    if (!std::filesystem::is_directory(root_ / "forums")) {
        throw std::runtime_error(
            "Workspace '" + utf8_path(root_)
            + "' requires a forums/ directory");
    }
}

std::vector<std::string> Workspace::forums() const {
    const std::filesystem::path forums_directory = root_ / "forums";
    return subdirectory_names(forums_directory, "Forums");
}

Forum Workspace::load_forum(const std::string& name) const {
    const std::filesystem::path directory = forum_directory(name);
    if (!std::filesystem::is_directory(directory)) {
        throw std::runtime_error("Forum '" + name + "' does not exist");
    }
    const std::vector<std::string> persona_names = subdirectory_names(
        directory / "personas", "Personas");
    return {name, load_display_name(directory), persona_names, directory};
}

std::filesystem::path Workspace::forum_directory(
    const std::string& name) const {
    require_path_component(name, root_ / "forums");
    return root_ / "forums" / path_from_utf8(name);
}

std::vector<SessionSummary> Workspace::sessions(
    const std::string& forum_name) const {
    const SessionCatalog catalog = session_catalog(*this, forum_name);
    const std::vector<Session> stored = catalog.list();

    std::vector<SessionSummary> result;
    result.reserve(stored.size());
    for (const Session& session : stored) {
        result.push_back(summarize(session));
    }
    return result;
}

CreatedSession Workspace::create_session(
    const std::string& forum_name,
    std::string label,
    WakeNotifier& notifier) const {
    const Forum forum = load_forum(forum_name);
    std::vector<AgentDefinition> definitions = load_definitions(
        forum, forum.directory / "personas" / "base_config.toml");
    const SessionCatalog catalog(forum.directory / "sessions", forum.name);

    const Session session = catalog.create(std::move(label));
    return {
        .controller = SessionController::from_definitions(
            std::move(definitions),
            catalog.database_path(session.id),
            notifier),
        .id = session.id,
    };
}

std::unique_ptr<SessionController> Workspace::open_session(
    const std::string& forum_name,
    const std::string& session_id,
    WakeNotifier& notifier) const {
    const Forum forum = load_forum(forum_name);
    std::vector<AgentDefinition> definitions = load_definitions(
        forum, forum.directory / "personas" / "base_config.toml");
    const SessionCatalog catalog(forum.directory / "sessions", forum.name);

    const std::filesystem::path database_path =
        catalog.open_database_path(session_id);
    SessionRestore restored = load_session_state(database_path);
    return SessionController::from_definitions(
        std::move(definitions),
        database_path,
        notifier,
        std::move(restored));
}

} // namespace cha
