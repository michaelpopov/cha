#include "session/workspace.h"

#include "agents/agent.h"
#include "session/forum_personas.h"
#include "session/session_controller.h"
#include "session/session_database.h"
#include "session/session_catalog.h"
#include "session/session_lease.h"
#include "util/path_name.h"
#include "util/text.h"
#include "util/logging.h"
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
    log_info(
        "Loading forum persona definitions: forum_id=" + forum.name
        + " personas=" + std::to_string(forum.persona_names.size()));
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
        persona_directories,
        forum.directory,
        forum.display_name,
        base_config);
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

enum class SubdirectoryNameKind { path_component, url_identifier };

std::vector<std::string> subdirectory_names(
    const std::filesystem::path& directory,
    SubdirectoryNameKind kind) {
    std::vector<std::string> result;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory)) {
        if (!entry.is_directory()) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (kind == SubdirectoryNameKind::url_identifier) {
            if (!is_url_safe_identifier(name)) {
                log_warn(
                    "Invalid forum directory ignored: path="
                    + utf8_path(entry.path()));
                continue;
            }
        } else {
            require_path_component(name, directory);
        }
        result.push_back(name);
    }
    std::sort(result.begin(), result.end());
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

ApplicationConfig load_application_config(const std::filesystem::path& root) {
    const std::filesystem::path path = root / "app.toml";
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error(
            "Failed to read application config '" + utf8_path(path) + "'");
    }
    const toml::table table = toml::parse(file, utf8_path(path));
    const std::optional<std::string> host =
        table["host"].value<std::string>();
    if (!host || host->empty()) {
        throw std::runtime_error(
            "Application config '" + utf8_path(path)
            + "' requires a non-empty string 'host'");
    }
    const std::optional<int> port = table["port"].value<int>();
    if (!port || *port < 1 || *port > 65535) {
        throw std::runtime_error(
            "Application config '" + utf8_path(path)
            + "' requires an integer 'port' between 1 and 65535");
    }
    const toml::table* logging = table["logging"].as_table();
    if (!logging) {
        throw std::runtime_error(
            "Application config '" + utf8_path(path)
            + "' requires a [logging] table");
    }
    const std::optional<std::string> log_file =
        (*logging)["file"].value<std::string>();
    if (!log_file || log_file->empty()) {
        throw std::runtime_error(
            "Application config '" + utf8_path(path)
            + "' requires a non-empty string 'logging.file'");
    }
    const std::optional<std::string> log_level =
        (*logging)["level"].value<std::string>();
    if (!log_level || log_level->empty()) {
        throw std::runtime_error(
            "Application config '" + utf8_path(path)
            + "' requires a non-empty string 'logging.level'");
    }

    std::filesystem::path log_path = path_from_utf8(*log_file);
    if (log_path.is_relative()) {
        log_path = root / log_path;
    }
    return {
        .host = *host,
        .port = *port,
        .log_file = std::move(log_path),
        .log_level = *log_level,
    };
}

Workspace::Workspace(std::filesystem::path root)
    : Workspace(root, load_application_config(root)) {
}

Workspace::Workspace(
    std::filesystem::path root,
    ApplicationConfig app_config)
    : root_(std::move(root)), app_config_(std::move(app_config)) {
    if (!std::filesystem::is_directory(root_ / "forums")) {
        throw std::runtime_error(
            "Workspace '" + utf8_path(root_)
            + "' requires a forums/ directory");
    }
}

const ApplicationConfig& Workspace::app_config() const {
    return app_config_;
}

std::vector<std::string> Workspace::forums() const {
    const std::filesystem::path forums_directory = root_ / "forums";
    return subdirectory_names(
        forums_directory,
        SubdirectoryNameKind::url_identifier);
}

Forum Workspace::load_forum(const std::string& name) const {
    const std::filesystem::path directory = forum_directory(name);
    if (!std::filesystem::is_directory(directory)) {
        throw ForumNotFoundError("Forum '" + name + "' does not exist");
    }
    const std::vector<std::string> persona_names = subdirectory_names(
        directory / "personas",
        SubdirectoryNameKind::path_component);
    if (persona_names.empty()) {
        throw std::runtime_error(
            "Personas directory '" + utf8_path(directory / "personas")
            + "' does not contain an entry");
    }
    return {name, load_display_name(directory), persona_names, directory};
}

Forum Workspace::check_forum(const std::string& name) const {
    Forum forum = load_forum(name);
    const std::vector<AgentDefinition> definitions = load_definitions(
        forum, forum.directory / "personas" / "persona_defaults.toml");

    std::vector<PersonaInfo> personas;
    personas.reserve(definitions.size());
    for (const AgentDefinition& definition : definitions) {
        personas.push_back({
            .id = definition.config.id,
            .name = definition.config.name,
        });
    }
    (void)ForumPersonas(std::move(personas));
    return forum;
}

std::filesystem::path Workspace::forum_directory(
    const std::string& name) const {
    require_url_safe_identifier(name, root_ / "forums");
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

SessionSummary Workspace::session_summary(
    const std::string& forum_name,
    const std::string& session_id) const {
    const SessionCatalog catalog = session_catalog(*this, forum_name);
    return summarize(catalog.session(session_id));
}

void Workspace::check_session(
    const std::string& forum_name,
    const std::string& session_id) const {
    (void)session_summary(forum_name, session_id);
}

SessionSummary Workspace::create_stored_session(
    const std::string& forum_name,
    std::string label) const {
    const Forum forum = check_forum(forum_name);
    const SessionCatalog catalog(forum.directory / "sessions", forum.name);
    const Session session = catalog.create(std::move(label));
    log_info("Session stored");
    return summarize(session);
}

CreatedSession Workspace::create_session(
    const std::string& forum_name,
    std::string label,
    WakeNotifier& notifier) const {
    const SessionSummary created = create_stored_session(
        forum_name,
        std::move(label));
    return {
        .controller = open_session(forum_name, created.id, notifier),
        .id = created.id,
    };
}

std::unique_ptr<SessionController> Workspace::open_session(
    const std::string& forum_name,
    const std::string& session_id,
    WakeNotifier& notifier) const {
    const Forum forum = load_forum(forum_name);
    const SessionCatalog catalog(forum.directory / "sessions", forum.name);

    const std::filesystem::path database_path =
        catalog.open_database_path(session_id);
    SessionLease lease = SessionLease::acquire(database_path);
    SessionRestore restored = load_session_state(database_path);
    std::vector<AgentDefinition> definitions = load_definitions(
        forum, forum.directory / "personas" / "persona_defaults.toml");
    log_info("Session opened");
    return SessionController::from_definitions(
        std::move(definitions),
        database_path,
        std::move(lease),
        notifier,
        std::move(restored));
}

} // namespace cha
