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
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <unordered_map>
#include <utility>

namespace cha {
namespace {

std::vector<AgentDefinition> load_definitions(
    const Forum& forum,
    const UserRoster& users,
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
        users,
        base_config);
}

void validate_forum_personas(
    const std::vector<AgentDefinition>& definitions) {
    std::vector<PersonaInfo> personas;
    personas.reserve(definitions.size());
    for (const AgentDefinition& definition : definitions) {
        personas.push_back({
            .id = definition.config.id,
            .name = definition.config.name,
        });
    }
    (void)ForumPersonas(std::move(personas));
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

Session store_session(const Forum& forum, std::string label) {
    const SessionCatalog catalog(forum.directory / "sessions", forum.name);
    Session stored = catalog.create(std::move(label));
    log_info("Session stored");
    return stored;
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

bool is_user_id(std::string_view id) {
    if (id.empty()) return false;
    const auto is_letter = [](unsigned char character) {
        return (character >= 'A' && character <= 'Z')
            || (character >= 'a' && character <= 'z');
    };
    const unsigned char first = static_cast<unsigned char>(id.front());
    if (!is_letter(first) && first != '_') return false;
    for (const char value : id) {
        const unsigned char character = static_cast<unsigned char>(value);
        if (!is_letter(character)
            && !(character >= '0' && character <= '9')
            && character != '_') {
            return false;
        }
    }
    return true;
}

bool is_reserved_participant_name(std::string_view name) {
    const std::string folded = fold_ascii(name);
    return std::ranges::any_of(
        reserved_participant_names,
        [&folded](std::string_view reserved) { return folded == reserved; });
}

char32_t next_utf8_code_point(std::string_view text, std::size_t& offset) {
    const auto byte = [&text](std::size_t index) {
        return static_cast<unsigned char>(text[index]);
    };
    const unsigned char first = byte(offset++);
    if (first < 0x80) return first;

    std::size_t continuation_count{};
    char32_t code_point{};
    if ((first & 0xe0) == 0xc0) {
        continuation_count = 1;
        code_point = first & 0x1f;
    } else if ((first & 0xf0) == 0xe0) {
        continuation_count = 2;
        code_point = first & 0x0f;
    } else if ((first & 0xf8) == 0xf0) {
        continuation_count = 3;
        code_point = first & 0x07;
    } else {
        throw std::runtime_error("User display name is not valid UTF-8");
    }
    if (continuation_count > text.size() - offset) {
        throw std::runtime_error("User display name is not valid UTF-8");
    }
    for (std::size_t index = 0; index < continuation_count; ++index) {
        const unsigned char continuation = byte(offset++);
        if ((continuation & 0xc0) != 0x80) {
            throw std::runtime_error("User display name is not valid UTF-8");
        }
        code_point = (code_point << 6) | (continuation & 0x3f);
    }
    return code_point;
}

bool is_unicode_whitespace(char32_t code_point) {
    return (code_point >= 0x0009 && code_point <= 0x000d)
        || code_point == 0x0020
        || code_point == 0x0085
        || code_point == 0x00a0
        || code_point == 0x1680
        || (code_point >= 0x2000 && code_point <= 0x200a)
        || code_point == 0x2028
        || code_point == 0x2029
        || code_point == 0x202f
        || code_point == 0x205f
        || code_point == 0x3000;
}

bool is_unicode_control(char32_t code_point) {
    return code_point <= 0x001f
        || (code_point >= 0x007f && code_point <= 0x009f);
}

void validate_user_id(std::string_view id, const std::filesystem::path& directory) {
    if (!is_user_id(id)) {
        throw std::runtime_error(
            "User ID '" + std::string(id) + "' in '" + utf8_path(directory)
            + "' must match [A-Za-z_][A-Za-z0-9_]*");
    }
    if (is_reserved_participant_name(id)) {
        throw std::runtime_error(
            "User ID '" + std::string(id) + "' in '" + utf8_path(directory)
            + "' is reserved");
    }
}

void validate_user_display_name(
    std::string_view name,
    const std::filesystem::path& path) {
    if (name.empty()) {
        throw std::runtime_error(
            "User config '" + utf8_path(path)
            + "' requires a non-empty string 'display_name'");
    }
    if (name.front() == '@' || name.front() == '/') {
        throw std::runtime_error("User display name cannot start with '@' or '/'");
    }
    char32_t first_code_point{};
    char32_t last_code_point{};
    std::size_t offset{};
    while (offset < name.size()) {
        const bool first = offset == 0;
        const char32_t code_point = next_utf8_code_point(name, offset);
        if (first) first_code_point = code_point;
        last_code_point = code_point;
        if (is_unicode_control(code_point)
            || code_point == 0x2028
            || code_point == 0x2029) {
            throw std::runtime_error(
                "User display name cannot contain control characters or line breaks");
        }
    }
    if (is_unicode_whitespace(first_code_point)
        || is_unicode_whitespace(last_code_point)) {
        throw std::runtime_error("User display name cannot start or end with whitespace");
    }
    if (is_reserved_participant_name(name)) {
        throw std::runtime_error(
            "User display name '" + std::string(name) + "' is reserved");
    }
}

User load_user(const std::filesystem::path& directory) {
    const std::string id = utf8_path(directory.filename());
    validate_user_id(id, directory.parent_path());
    const std::filesystem::path config_path = directory / "user.toml";
    std::ifstream config_file(config_path, std::ios::binary);
    if (!config_file) {
        throw std::runtime_error(
            "Failed to read user config '" + utf8_path(config_path) + "'");
    }
    const toml::table table = toml::parse(config_file, utf8_path(config_path));
    for (const auto& [key, value] : table) {
        (void)value;
        if (key.str() != "display_name") {
            throw std::runtime_error(
                "User config '" + utf8_path(config_path)
                + "' has unknown field '" + std::string(key.str()) + "'");
        }
    }
    const std::optional<std::string> display_name =
        table["display_name"].value<std::string>();
    validate_user_display_name(display_name.value_or(""), config_path);

    std::string prompt;
    const std::filesystem::path prompt_path = directory / "USER.md";
    if (std::filesystem::is_regular_file(prompt_path)) {
        std::ifstream prompt_file(prompt_path, std::ios::binary);
        if (!prompt_file) {
            throw std::runtime_error(
                "Failed to read user prompt '" + utf8_path(prompt_path) + "'");
        }
        std::ostringstream contents;
        contents << prompt_file.rdbuf();
        if (!prompt_file.good() && !prompt_file.eof()) {
            throw std::runtime_error(
                "Failed to read user prompt '" + utf8_path(prompt_path) + "'");
        }
        prompt = std::move(contents).str();
    } else if (std::filesystem::exists(prompt_path)) {
        throw std::runtime_error(
            "User prompt '" + utf8_path(prompt_path) + "' is not a regular file");
    }
    return {id, *display_name, std::move(prompt)};
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

UserRoster Workspace::load_users() const {
    const std::filesystem::path users_directory = root_ / "users";
    if (!std::filesystem::is_directory(users_directory)) {
        throw std::runtime_error(
            "Users directory '" + utf8_path(users_directory)
            + "' does not exist; create users/<id>/user.toml");
    }
    UserRoster users;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(users_directory)) {
        if (entry.is_directory()) users.push_back(load_user(entry.path()));
    }
    if (users.empty()) {
        throw std::runtime_error(
            "Users directory '" + utf8_path(users_directory)
            + "' does not contain an entry");
    }
    std::sort(users.begin(), users.end(), [](const User& left, const User& right) {
        return left.id < right.id;
    });
    std::unordered_map<std::string, std::string> display_names;
    for (const User& user : users) {
        const std::string folded = fold_ascii(user.display_name);
        const auto [existing, inserted] = display_names.emplace(folded, user.id);
        if (!inserted) {
            throw std::runtime_error(
                "User display name '" + user.display_name + "' is not unique: users '"
                + existing->second + "' and '" + user.id + "'");
        }
    }
    return users;
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
    const UserRoster users = load_users();
    const std::vector<AgentDefinition> definitions = load_definitions(
        forum, users, forum.directory / "personas" / "persona_defaults.toml");
    validate_forum_personas(definitions);
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
    return summarize(store_session(forum, std::move(label)));
}

CreatedSession Workspace::create_session(
    const std::string& forum_name,
    std::string label,
    WakeNotifier& notifier) const {
    Forum forum = load_forum(forum_name);
    UserRoster users = load_users();
    std::vector<AgentDefinition> definitions = load_definitions(
        forum, users, forum.directory / "personas" / "persona_defaults.toml");
    validate_forum_personas(definitions);

    const Session stored = store_session(forum, std::move(label));

    const SessionCatalog catalog(forum.directory / "sessions", forum.name);
    const std::filesystem::path database_path =
        catalog.open_database_path(stored.id);
    SessionLease lease = SessionLease::acquire(database_path);
    SessionRestore restored = load_session_state(database_path);
    std::unique_ptr<SessionController> controller =
        SessionController::from_definitions(
            std::move(definitions),
            std::move(users),
            database_path,
            std::move(lease),
            notifier,
            std::move(restored));
    log_info("Session opened");
    return {
        .controller = std::move(controller),
        .id = stored.id,
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
    UserRoster users = load_users();
    std::vector<AgentDefinition> definitions = load_definitions(
        forum, users, forum.directory / "personas" / "persona_defaults.toml");
    log_info("Session opened");
    return SessionController::from_definitions(
        std::move(definitions),
        std::move(users),
        database_path,
        std::move(lease),
        notifier,
        std::move(restored));
}

} // namespace cha
