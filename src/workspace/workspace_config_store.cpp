#include "workspace/workspace_config_store.h"

#include "session/session_lease.h"
#include "session/session_storage_layout.h"
#include "session/sqlite_storage.h"
#include "session/workspace_session_database.h"
#include "util/environment.h"
#include "util/path_name.h"
#include "util/private_filesystem.h"
#include "workspace/workspace.h"

#include <toml++/toml.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace cha {
namespace {

using Database = storage::SqliteDatabase;

constexpr std::array sidecar_suffixes{
    std::string_view("-journal"),
    std::string_view("-wal"),
    std::string_view("-shm"),
};

constexpr std::array skeleton_directories{
    std::string_view("system/providers"),
    std::string_view("personas"),
    std::string_view("characters"),
    std::string_view("forums"),
};

constexpr std::string_view forum_member_placeholder =
    "# Required placeholder\n";

std::atomic_uint64_t next_validation_serial{};

[[noreturn]] void fail_path(std::string message) {
    throw std::runtime_error(std::move(message));
}

std::filesystem::path normalize_path(const std::filesystem::path& path) {
    return std::filesystem::weakly_canonical(std::filesystem::absolute(path));
}

std::filesystem::file_status inspected_status(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(path, error);
    if (status.type() == std::filesystem::file_type::not_found) {
        return status;
    }
    if (error) {
        fail_path(
            "Failed to inspect '" + utf8_path(path) + "': " + error.message());
    }
    return status;
}

bool path_is_under(
    const std::filesystem::path& root,
    const std::filesystem::path& candidate) {
    auto next_nonempty = [](std::filesystem::path::const_iterator it,
                            std::filesystem::path::const_iterator end) {
        while (it != end && it->empty()) ++it;
        return it;
    };
    auto root_it = next_nonempty(root.begin(), root.end());
    auto candidate_it = next_nonempty(candidate.begin(), candidate.end());
    const auto root_end = root.end();
    const auto candidate_end = candidate.end();
    while (root_it != root_end) {
        if (candidate_it == candidate_end || *root_it != *candidate_it) {
            return false;
        }
        root_it = next_nonempty(std::next(root_it), root_end);
        candidate_it = next_nonempty(std::next(candidate_it), candidate_end);
    }
    return true;
}

std::filesystem::path require_existing_directory(
    const std::filesystem::path& path,
    std::string_view role) {
    const std::filesystem::path normalized = normalize_path(path);
    const std::filesystem::file_status status = inspected_status(normalized);
    if (!std::filesystem::exists(status)) {
        fail_path(
            std::string(role) + " '" + utf8_path(path) + "' does not exist");
    }
    if (!std::filesystem::is_directory(status)) {
        fail_path(
            std::string(role) + " '" + utf8_path(normalized)
            + "' is not a directory");
    }
    return normalized;
}

bool is_accepted_stored_name(std::string_view name) {
    return name != "app.toml" && name != "workspace.toml"
        && (name.ends_with(".toml") || name.ends_with(".md"));
}

bool is_forum_member_directory(std::string_view name) {
    constexpr std::string_view forums_prefix = "forums/";
    constexpr std::string_view members_prefix = "members/";
    if (!name.starts_with(forums_prefix)) return false;
    name.remove_prefix(forums_prefix.size());

    const std::size_t forum_end = name.find('/');
    if (forum_end == std::string_view::npos || forum_end == 0) return false;
    name.remove_prefix(forum_end + 1);
    if (!name.starts_with(members_prefix)) return false;
    name.remove_prefix(members_prefix.size());
    return !name.empty() && name.find('/') == std::string_view::npos;
}

std::string stored_name_from(
    const std::filesystem::path& source,
    const std::filesystem::path& file) {
    return file.lexically_relative(source).generic_string();
}

std::filesystem::path join_stored_name(
    const std::filesystem::path& destination,
    std::string_view name) {
    std::filesystem::path joined = destination;
    std::string_view rest = name;
    for (;;) {
        const auto slash = rest.find('/');
        joined /= path_from_utf8(rest.substr(0, slash));
        if (slash == std::string_view::npos) break;
        rest = rest.substr(slash + 1);
    }
    return joined;
}

void require_contained(
    const std::filesystem::path& destination,
    const std::filesystem::path& candidate) {
    const std::filesystem::path root =
        std::filesystem::weakly_canonical(std::filesystem::absolute(destination));
    const std::filesystem::path joined =
        std::filesystem::weakly_canonical(std::filesystem::absolute(candidate));
    if (joined == root || !path_is_under(root, joined)) {
        fail_path(
            "Configuration path '" + utf8_path(candidate)
            + "' escapes '" + utf8_path(destination) + "'");
    }
}

void validate_config_rows(const std::vector<ConfigFile>& rows) {
    std::set<std::string> names;
    for (const ConfigFile& row : rows) {
        validate_stored_config_name(row.name);
        if (!names.insert(row.name).second) {
            fail_path("Configuration name '" + row.name + "' is duplicated");
        }
    }
    for (const ConfigFile& row : rows) {
        for (std::size_t index = 0; index < row.name.size(); ++index) {
            if (row.name[index] != '/') continue;
            const std::string parent = row.name.substr(0, index);
            if (names.contains(parent)) {
                fail_path(
                    "Configuration name '" + row.name
                    + "' collides with '" + parent + "'");
            }
        }
    }
}

std::string read_file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        fail_path("Failed to read '" + utf8_path(path) + "'");
    }
    std::string content{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    if (input.bad()) {
        fail_path("Failed to read '" + utf8_path(path) + "'");
    }
    return content;
}

std::vector<ConfigFile> collect_config_rows(const std::filesystem::path& source) {
    std::map<std::string, std::string> files;
    std::set<std::string> forum_member_directories;
    std::error_code error;
    const std::filesystem::recursive_directory_iterator end;
    std::filesystem::recursive_directory_iterator iterator(
        source,
        std::filesystem::directory_options::none,
        error);
    if (error) {
        fail_path(
            "Failed to read '" + utf8_path(source) + "': " + error.message());
    }
    for (; iterator != end; iterator.increment(error)) {
        if (error) {
            fail_path(
                "Failed to read '" + utf8_path(source) + "': "
                + error.message());
        }
        const std::filesystem::path file = iterator->path();
        const std::string name = stored_name_from(source, file);
        if (name.empty() || name == ".") continue;

        const std::filesystem::file_status status = inspected_status(file);
        if (std::filesystem::is_symlink(status)) {
            if (is_accepted_stored_name(name)) {
                fail_path(
                    "Configuration path '" + utf8_path(file)
                    + "' is a symbolic link");
            }
            continue;
        }
        if (std::filesystem::is_directory(status)) {
            if (is_forum_member_directory(name)) {
                forum_member_directories.insert(name);
            }
            continue;
        }
        if (!std::filesystem::is_regular_file(status)) continue;
        if (!is_accepted_stored_name(name)) continue;
        validate_stored_config_name(name);
        if (!files.emplace(name, read_file_bytes(file)).second) {
            fail_path("Configuration name '" + name + "' is duplicated");
        }
    }

    for (const std::string& directory : forum_member_directories) {
        files.try_emplace(
            directory + "/character.toml",
            forum_member_placeholder);
    }

    std::vector<ConfigFile> rows;
    rows.reserve(files.size());
    for (auto& [name, content] : files) {
        rows.push_back({std::move(name), std::move(content)});
    }
    return rows;
}

void ensure_private_directory(const std::filesystem::path& path) {
    const std::filesystem::file_status status = inspected_status(path);
    if (!std::filesystem::exists(status)) {
        create_private_directory(path);
        return;
    }
    if (!std::filesystem::is_directory(status)) {
        fail_path(
            "Path '" + utf8_path(path) + "' is not a directory");
    }
}

void create_parent_directories(
    const std::filesystem::path& destination,
    std::string_view name) {
    std::string_view rest = name;
    std::filesystem::path current = destination;
    for (;;) {
        const auto slash = rest.find('/');
        if (slash == std::string_view::npos) break;
        current /= path_from_utf8(rest.substr(0, slash));
        require_contained(destination, current);
        ensure_private_directory(current);
        rest = rest.substr(slash + 1);
    }
}

void materialize_config_files(
    const std::filesystem::path& destination,
    const std::vector<ConfigFile>& rows) {
    validate_config_rows(rows);
    require_directory(destination);
    for (const ConfigFile& row : rows) {
        require_contained(destination, join_stored_name(destination, row.name));
    }

    for (const std::string_view directory : skeleton_directories) {
        create_parent_directories(destination, directory);
        const std::filesystem::path path =
            join_stored_name(destination, directory);
        require_contained(destination, path);
        ensure_private_directory(path);
    }

    for (const ConfigFile& row : rows) {
        const std::filesystem::path path =
            join_stored_name(destination, row.name);
        require_contained(destination, path);
        create_parent_directories(destination, row.name);
        if (std::filesystem::exists(inspected_status(path))) {
            fail_path("Path '" + utf8_path(path) + "' already exists");
        }
        create_private_file(path, row.content);
    }
}

class TemporaryPrivateRoot {
public:
    TemporaryPrivateRoot() {
        const std::filesystem::path parent =
            std::filesystem::temp_directory_path();
        root_ = parent
            / ("cha-import-"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count())
               + "-"
               + std::to_string(++next_validation_serial));
        create_private_directory(root_);
        try {
            create_private_directory(root_ / "workspace");
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove_all(root_, ignored);
            throw;
        }
    }

    TemporaryPrivateRoot(const TemporaryPrivateRoot&) = delete;
    TemporaryPrivateRoot& operator=(const TemporaryPrivateRoot&) = delete;

    ~TemporaryPrivateRoot() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }
    [[nodiscard]] std::filesystem::path workspace() const {
        return root_ / "workspace";
    }

private:
    std::filesystem::path root_;
};

// Previous processes may leave an orphaned cha-runtime-* tree in the system
// temporary directory. It is a copy of already-committed configuration plus a
// non-durable Welcome database. Runtime never reuses an orphan; each startup
// creates a fresh root and ordinary temporary-directory cleanup can remove the
// leftover tree.
class RuntimePrivateRoot {
public:
    RuntimePrivateRoot() {
        const std::filesystem::path parent =
            std::filesystem::temp_directory_path();
        std::exception_ptr failure;
        for (int attempt = 0; attempt != 8; ++attempt) {
            root_ = parent
                / ("cha-runtime-"
                   + std::to_string(
                       std::chrono::steady_clock::now().time_since_epoch().count())
                   + "-"
                   + std::to_string(++next_validation_serial));
            try {
                create_private_directory(root_);
                workspace_ = root_ / "workspace";
                welcome_ = root_ / "welcome";
                create_private_directory(workspace_);
                try {
                    create_private_directory(welcome_);
                } catch (...) {
                    std::error_code ignored;
                    std::filesystem::remove_all(root_, ignored);
                    throw;
                }
                return;
            } catch (...) {
                failure = std::current_exception();
                std::error_code ignored;
                std::filesystem::remove_all(root_, ignored);
                root_.clear();
                workspace_.clear();
                welcome_.clear();
            }
        }
        std::rethrow_exception(failure);
    }

    RuntimePrivateRoot(const RuntimePrivateRoot&) = delete;
    RuntimePrivateRoot& operator=(const RuntimePrivateRoot&) = delete;

    ~RuntimePrivateRoot() {
        std::error_code ignored;
        if (!root_.empty()) std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }
    [[nodiscard]] const std::filesystem::path& workspace() const noexcept {
        return workspace_;
    }
    [[nodiscard]] const std::filesystem::path& welcome() const noexcept {
        return welcome_;
    }

private:
    std::filesystem::path root_;
    std::filesystem::path workspace_;
    std::filesystem::path welcome_;
};

[[noreturn]] void fail_database_state(
    const std::filesystem::path& database,
    WorkspaceDatabaseState state);

void remove_directory_contents(const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::directory_iterator iterator(
        directory,
        std::filesystem::directory_options::none,
        error);
    if (error) {
        fail_path(
            "Failed to read '" + utf8_path(directory) + "': " + error.message());
    }
    std::vector<std::filesystem::path> children;
    const std::filesystem::directory_iterator end;
    for (; iterator != end; iterator.increment(error)) {
        if (error) {
            fail_path(
                "Failed to read '" + utf8_path(directory) + "': "
                + error.message());
        }
        children.push_back(iterator->path());
    }
    for (const std::filesystem::path& child : children) {
        std::filesystem::remove_all(child, error);
        if (error) {
            fail_path(
                "Failed to remove '" + utf8_path(child) + "': "
                + error.message());
        }
    }
}

[[noreturn]] void fail_runtime_database_state(
    const std::filesystem::path& database,
    WorkspaceDatabaseState state) {
    if (state == WorkspaceDatabaseState::missing) {
        fail_path(
            "Workspace session database '" + utf8_path(database)
            + "' does not exist. Stop CHA and run:\n"
              "chaweb --config=CONFIG --import WORKSPACE");
    }
    fail_database_state(database, state);
}

std::vector<std::string> forums_using_character(
    const Workspace& workspace,
    std::string_view character_id) {
    std::vector<std::string> result;
    for (const WorkspaceForum& forum : workspace.forums()) {
        for (const WorkspaceForumMember& member : forum.members) {
            if (member.character_id == character_id) {
                result.push_back(forum.id);
                break;
            }
        }
    }
    return result;
}

void remove_created_database(const std::filesystem::path& path) noexcept {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    for (const std::string_view suffix : sidecar_suffixes) {
        std::filesystem::path sidecar = path;
        sidecar += suffix;
        std::filesystem::remove(sidecar, ignored);
    }
}

bool directory_is_empty(const std::filesystem::path& path) {
    return std::filesystem::directory_iterator(path)
        == std::filesystem::directory_iterator();
}

std::string busy_message(const std::filesystem::path& database) {
    return "Database already in use: '" + utf8_path(database) + "'";
}

[[noreturn]] void fail_legacy(
    const std::filesystem::path& source,
    bool database_exists) {
    if (database_exists) {
        fail_path(
            "Legacy session databases remain in workspace '"
            + utf8_path(source)
            + "'. Verify 'workspace.sqlite3', then remove the legacy "
              "session files before starting this build");
    }
    fail_path(
        "Legacy session databases were found in workspace '"
        + utf8_path(source)
        + "'. This build cannot migrate them. Use an archived "
          "migration-capable CHA build to run 'chaweb --migration "
          "--workspace <workspace>', verify 'workspace.sqlite3', then "
          "remove the legacy session files before starting this build");
}

[[noreturn]] void fail_database_state(
    const std::filesystem::path& database,
    WorkspaceDatabaseState state) {
    switch (state) {
    case WorkspaceDatabaseState::missing:
        fail_path(
            "Workspace session database '" + utf8_path(database)
            + "' does not exist");
    case WorkspaceDatabaseState::valid_v1:
        fail_path(
            "Workspace session database '" + utf8_path(database)
            + "' is a valid CHA schema-1 database. Stop CHA and run:\n"
              "chaweb --config=CONFIG --import WORKSPACE");
    case WorkspaceDatabaseState::wrong_application_id:
    case WorkspaceDatabaseState::unsupported_version:
        fail_path(
            "Workspace session database '" + utf8_path(database)
            + "' has an unsupported schema");
    case WorkspaceDatabaseState::valid_v2:
    case WorkspaceDatabaseState::corrupt:
        break;
    }
    fail_path(
        "Workspace session database '" + utf8_path(database)
        + "' is not a valid CHA database");
}

void validate_materialized_source(
    const std::vector<ConfigFile>& rows,
    const std::filesystem::path& source) {
    TemporaryPrivateRoot root;
    materialize_config_files(root.workspace(), rows);
    std::vector<DotenvEntry> entries;
    const std::filesystem::path dotenv = source / ".env";
    if (std::filesystem::exists(inspected_status(dotenv))) {
        entries = parse_dotenv(dotenv);
    }
    ScopedEnvironmentOverlay overlay(entries);
    (void)Workspace::load(root.workspace());
}

struct PrunedImport {
    std::vector<ConfigFile> rows;
    std::set<std::string> forums;
};

std::string_view trimmed_line_start(std::string_view line) {
    while (!line.empty()
           && (line.front() == ' ' || line.front() == '\t'
               || line.front() == '\r')) {
        line.remove_prefix(1);
    }
    return line;
}

std::optional<std::string> forum_config_default(std::string_view content) {
    toml::table table;
    try {
        table = toml::parse(content);
    } catch (const toml::parse_error&) {
        return std::nullopt;
    }
    if (table.contains("default_character") == table.contains("default_agent")) {
        return std::nullopt;
    }
    const auto node = table.contains("default_character")
        ? table["default_character"] : table["default_agent"];
    return node.value<std::string>();
}

std::string without_forum_default_lines(std::string_view content) {
    std::string result;
    std::size_t position{};
    while (position <= content.size()) {
        const std::size_t end = content.find('\n', position);
        const bool last = end == std::string_view::npos;
        const std::string_view line = last
            ? content.substr(position)
            : content.substr(position, end - position + 1);
        const std::string_view body = trimmed_line_start(line);
        if (!body.starts_with("default_character")
            && !body.starts_with("default_agent")) {
            result.append(line);
        }
        if (last) break;
        position = end + 1;
    }
    return result;
}

PrunedImport prune_imported_rows(std::vector<ConfigFile> rows) {
    std::set<std::string> characters;
    for (const ConfigFile& row : rows) {
        constexpr std::string_view prefix = "characters/";
        constexpr std::string_view suffix = "/character.toml";
        if (!row.name.starts_with(prefix) || !row.name.ends_with(suffix)
            || row.name.size() < prefix.size() + suffix.size()) {
            continue;
        }
        std::string_view middle = std::string_view(row.name).substr(prefix.size());
        middle.remove_suffix(suffix.size());
        const std::size_t slash = middle.rfind('/');
        const std::string_view id =
            slash == std::string_view::npos ? middle : middle.substr(slash + 1);
        if (!id.empty()) characters.emplace(id);
    }

    constexpr std::string_view forums_prefix = "forums/";
    constexpr std::string_view members_infix = "/members/";
    std::set<std::string> forum_ids;
    std::map<std::string, std::set<std::string>> forum_members;
    for (const ConfigFile& row : rows) {
        if (!row.name.starts_with(forums_prefix)) continue;
        const std::string_view rest =
            std::string_view(row.name).substr(forums_prefix.size());
        const std::size_t slash = rest.find('/');
        if (slash == std::string_view::npos) continue;
        const std::string forum(rest.substr(0, slash));
        forum_ids.insert(forum);
        const std::string prefix =
            std::string(forums_prefix) + forum + std::string(members_infix);
        if (!row.name.starts_with(prefix)) continue;
        const std::string_view member_rest =
            std::string_view(row.name).substr(prefix.size());
        const std::size_t member_end = member_rest.find('/');
        if (member_end == std::string_view::npos) continue;
        const std::string member(member_rest.substr(0, member_end));
        if (!member.empty()) forum_members[forum].insert(member);
    }

    std::map<std::string, std::set<std::string>> removed_members;
    for (const auto& [forum, members] : forum_members) {
        for (const std::string& member : members) {
            if (!characters.contains(member)) removed_members[forum].insert(member);
        }
    }
    if (!removed_members.empty()) {
        std::vector<ConfigFile> kept;
        kept.reserve(rows.size());
        for (ConfigFile& row : rows) {
            bool drop = false;
            for (const auto& [forum, members] : removed_members) {
                const std::string prefix =
                    std::string(forums_prefix) + forum + std::string(members_infix);
                if (!row.name.starts_with(prefix)) continue;
                const std::string_view rest =
                    std::string_view(row.name).substr(prefix.size());
                const std::size_t end = rest.find('/');
                if (end != std::string_view::npos
                    && members.contains(std::string(rest.substr(0, end)))) {
                    drop = true;
                    break;
                }
            }
            if (!drop) kept.push_back(std::move(row));
        }
        rows = std::move(kept);
        for (const auto& [forum, members] : removed_members) {
            for (const std::string& member : members) forum_members[forum].erase(member);
        }
    }

    std::set<std::string> empty_forums;
    for (const std::string& forum : forum_ids) {
        const auto found = forum_members.find(forum);
        if (found == forum_members.end() || found->second.empty()) empty_forums.insert(forum);
    }
    if (!empty_forums.empty()) {
        std::vector<ConfigFile> kept;
        kept.reserve(rows.size());
        for (ConfigFile& row : rows) {
            bool drop = false;
            for (const std::string& forum : empty_forums) {
                if (row.name == "forums/" + forum
                    || row.name.starts_with("forums/" + forum + "/")) {
                    drop = true;
                    break;
                }
            }
            if (!drop) kept.push_back(std::move(row));
        }
        rows = std::move(kept);
        for (const std::string& forum : empty_forums) {
            forum_ids.erase(forum);
            forum_members.erase(forum);
        }
    }

    for (ConfigFile& row : rows) {
        if (!row.name.starts_with(forums_prefix)) continue;
        const std::string_view rest =
            std::string_view(row.name).substr(forums_prefix.size());
        const std::size_t slash = rest.find('/');
        if (slash == std::string_view::npos || rest.substr(slash + 1) != "config.toml") {
            continue;
        }
        const auto members = forum_members.find(std::string(rest.substr(0, slash)));
        if (members == forum_members.end()) continue;
        const std::optional<std::string> configured = forum_config_default(row.content);
        if (!configured || members->second.contains(*configured)) continue;
        row.content = without_forum_default_lines(row.content);
    }

    std::set<std::string> surviving = forum_ids;
    surviving.emplace(workspace_entrance_id);
    return {.rows = std::move(rows), .forums = std::move(surviving)};
}

void prune_orphan_sessions(Database& database, const std::set<std::string>& valid_forums) {
    storage::SqliteStatement forums = database.prepare("SELECT forum_id FROM forums");
    std::vector<std::string> orphans;
    while (forums.step()) {
        const std::string forum_id = forums.text(0);
        if (!valid_forums.contains(forum_id)) orphans.push_back(forum_id);
    }
    for (const std::string& forum_id : orphans) {
        storage::SqliteStatement delete_sessions = database.prepare(
            "DELETE FROM sessions WHERE forum_key IN ("
            "SELECT forum_key FROM forums WHERE forum_id = ?1)",
            std::string_view(forum_id));
        delete_sessions.run();
        storage::SqliteStatement delete_forum = database.prepare(
            "DELETE FROM forums WHERE forum_id = ?1", std::string_view(forum_id));
        delete_forum.run();
    }
}

void commit_imported_rows(
    const std::filesystem::path& database,
    const std::vector<ConfigFile>& rows,
    const std::set<std::string>& valid_forums) {
    const WorkspaceDatabaseState state =
        inspect_workspace_session_database(database);
    switch (state) {
    case WorkspaceDatabaseState::missing: {
        create_empty_workspace_session_database(database);
        try {
            Database handle(database, Database::Mode::read_write);
            storage::SqliteTransaction transaction(handle);
            replace_workspace_config_files(handle, rows);
            transaction.commit();
        } catch (...) {
            remove_created_database(database);
            throw;
        }
        break;
    }
    case WorkspaceDatabaseState::valid_v1: {
        Database handle(database, Database::Mode::read_write);
        upgrade_workspace_session_database_from_v1(handle, rows);
        storage::SqliteTransaction transaction(handle);
        prune_orphan_sessions(handle, valid_forums);
        transaction.commit();
        break;
    }
    case WorkspaceDatabaseState::valid_v2: {
        Database handle(database, Database::Mode::read_write);
        storage::SqliteTransaction transaction(handle);
        replace_workspace_config_files(handle, rows);
        prune_orphan_sessions(handle, valid_forums);
        transaction.commit();
        break;
    }
    default:
        fail_database_state(database, state);
    }
    secure_workspace_session_database_files(database);
}

} // namespace

WorkspaceConfigTransfer import_workspace_configuration(
    const std::filesystem::path& source_directory,
    const std::filesystem::path& database_path) {
    const std::filesystem::path source =
        require_existing_directory(source_directory, "Import source");
    const std::filesystem::path database = normalize_path(database_path);
    (void)require_existing_directory(database.parent_path(), "Database parent");

    SessionLease lease = SessionLease::acquire(database, busy_message(database));
    (void)lease;

    const bool database_exists =
        std::filesystem::is_regular_file(inspected_status(database));
    if (has_legacy_session_databases(source)) {
        fail_legacy(source, database_exists);
    }

    PrunedImport pruned = prune_imported_rows(collect_config_rows(source));
    validate_materialized_source(pruned.rows, source);

    secure_workspace_session_database_files(database);
    commit_imported_rows(database, pruned.rows, pruned.forums);
    return {.file_count = pruned.rows.size()};
}

WorkspaceConfigTransfer export_workspace_configuration(
    const std::filesystem::path& database_path,
    const std::filesystem::path& destination_directory) {
    const std::filesystem::path database = normalize_path(database_path);
    (void)require_existing_directory(database.parent_path(), "Database parent");
    const std::filesystem::path destination =
        normalize_path(destination_directory);

    SessionLease lease = SessionLease::acquire(database, busy_message(database));
    (void)lease;

    secure_workspace_session_database_files(database);
    const WorkspaceDatabaseState state =
        inspect_workspace_session_database(database);
    if (state != WorkspaceDatabaseState::valid_v2) {
        fail_database_state(database, state);
    }

    Database handle(database, Database::Mode::read_only);
    validate_workspace_session_database_identity(handle);
    validate_workspace_session_contents(handle);
    const std::vector<ConfigFile> rows = read_workspace_config_files(handle);
    validate_config_rows(rows);

    const std::filesystem::file_status destination_status =
        inspected_status(destination);
    if (std::filesystem::exists(destination_status)) {
        if (!std::filesystem::is_directory(destination_status)) {
            fail_path(
                "Export destination '" + utf8_path(destination)
                + "' is not a directory");
        }
        if (!directory_is_empty(destination)) {
            fail_path(
                "Export destination '" + utf8_path(destination)
                + "' is not empty");
        }
    }

    bool output_began = false;
    try {
        if (!std::filesystem::exists(destination_status)) {
            create_private_directory(destination);
        }
        output_began = true;
        materialize_config_files(destination, rows);
    } catch (const std::exception& error) {
        if (output_began) {
            fail_path(
                std::string(error.what()) + ". Destination '"
                + utf8_path(destination)
                + "' may be incomplete and must be emptied before retrying");
        }
        throw;
    }
    return {.file_count = rows.size()};
}

std::atomic<WorkspaceConfigFault> forced_runtime_fault{
    WorkspaceConfigFault::none};

bool consume_runtime_fault(WorkspaceConfigFault fault) {
    WorkspaceConfigFault expected = fault;
    return forced_runtime_fault.compare_exchange_strong(
        expected, WorkspaceConfigFault::none);
}

void force_next_workspace_config_fault(WorkspaceConfigFault fault) {
    forced_runtime_fault.store(fault);
}

struct WorkspaceConfigStore::Impl {
    std::filesystem::path database_path;
    std::optional<SessionLease> lease;
    std::unique_ptr<Database> database;
    std::optional<RuntimePrivateRoot> tree;
    std::mutex mutex;

    void rematerialize_workspace() {
        if (consume_runtime_fault(WorkspaceConfigFault::restore)) {
            fail_path("Forced restoration failure");
        }
        remove_directory_contents(tree->workspace());
        const std::vector<ConfigFile> rows =
            read_workspace_config_files(*database);
        materialize_config_files(tree->workspace(), rows);
    }

    [[noreturn]] void restore_after_pre_commit_failure(
        std::exception_ptr failure) {
        try {
            rematerialize_workspace();
        } catch (const std::exception& restore_error) {
            std::string original = "Configuration edit failed";
            try {
                std::rethrow_exception(std::move(failure));
            } catch (const std::exception& error) {
                original = error.what();
            }
            throw WorkspaceRestartRequiredError(
                original + ". Failed to restore the materialized workspace: "
                + restore_error.what() + ". Restart is required");
        }
        std::rethrow_exception(std::move(failure));
    }

    template<typename Writer>
    WorkspaceConfigEditResult edit(Writer&& writer) {
        const std::lock_guard lock(mutex);
        const std::shared_ptr<const Workspace> published = getws();
        if (!published || published->root() != tree->workspace()) {
            fail_path(
                "Runtime configuration store has no matching loaded workspace");
        }

        std::exception_ptr failure;
        bool committed = false;
        std::vector<std::string> affected_forum_ids;
        try {
            affected_forum_ids = writer(*published);
            Workspace candidate = Workspace::load(tree->workspace());
            if (consume_runtime_fault(WorkspaceConfigFault::collect_rows)) {
                fail_path("Forced configuration row-collection failure");
            }
            std::vector<ConfigFile> rows =
                collect_config_rows(tree->workspace());
            if (consume_runtime_fault(WorkspaceConfigFault::sqlite_begin)) {
                fail_path("Forced SQLite begin failure");
            }
            storage::SqliteTransaction transaction(*database);
            if (consume_runtime_fault(WorkspaceConfigFault::sqlite_write)) {
                fail_path("Forced SQLite write failure");
            }
            replace_workspace_config_files(*database, rows);
            if (consume_runtime_fault(WorkspaceConfigFault::sqlite_commit)) {
                fail_path("Forced SQLite commit failure");
            }
            transaction.commit();
            committed = true;
            if (consume_runtime_fault(WorkspaceConfigFault::publication)) {
                fail_path("Forced workspace publication failure");
            }
            loadws(std::move(candidate));
        } catch (...) {
            failure = std::current_exception();
        }

        if (!committed) {
            restore_after_pre_commit_failure(std::move(failure));
        }
        if (failure) {
            try {
                std::rethrow_exception(std::move(failure));
            } catch (const std::exception& error) {
                throw WorkspaceRestartRequiredError(
                    std::string(
                        "Configuration was committed but could not be "
                        "published: ")
                    + error.what() + ". Restart is required");
            }
        }
        return {.affected_forum_ids = std::move(affected_forum_ids)};
    }
};

WorkspaceConfigStore::WorkspaceConfigStore(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {
}

WorkspaceConfigStore::~WorkspaceConfigStore() = default;

std::unique_ptr<WorkspaceConfigStore> WorkspaceConfigStore::open(
    const std::filesystem::path& database_path) {
    auto impl = std::make_unique<Impl>();
    impl->database_path = normalize_path(database_path);
    (void)require_existing_directory(
        impl->database_path.parent_path(), "Database parent");

    impl->lease.emplace(
        SessionLease::acquire(
            impl->database_path, busy_message(impl->database_path)));

    secure_workspace_session_database_files(impl->database_path);
    const WorkspaceDatabaseState state =
        inspect_workspace_session_database(impl->database_path);
    if (state != WorkspaceDatabaseState::valid_v2) {
        fail_runtime_database_state(impl->database_path, state);
    }

    impl->database = std::make_unique<Database>(
        impl->database_path, Database::Mode::read_write);
    validate_workspace_session_database_identity(*impl->database);
    validate_workspace_session_contents(*impl->database);
    impl->database->execute("PRAGMA journal_mode = WAL");
    secure_workspace_session_database_files(impl->database_path);

    impl->tree.emplace();
    const std::vector<ConfigFile> rows =
        read_workspace_config_files(*impl->database);
    validate_config_rows(rows);
    materialize_config_files(impl->tree->workspace(), rows);

    load_dotenv(impl->database_path.parent_path() / ".env");
    loadws(Workspace::load(impl->tree->workspace()));
    return std::unique_ptr<WorkspaceConfigStore>(
        new WorkspaceConfigStore(std::move(impl)));
}

const std::filesystem::path& WorkspaceConfigStore::private_root()
    const noexcept {
    return impl_->tree->root();
}

const std::filesystem::path& WorkspaceConfigStore::workspace_path()
    const noexcept {
    return impl_->tree->workspace();
}

const std::filesystem::path& WorkspaceConfigStore::welcome_path()
    const noexcept {
    return impl_->tree->welcome();
}

const std::filesystem::path& WorkspaceConfigStore::database_path()
    const noexcept {
    return impl_->database_path;
}

WorkspaceConfigEditResult WorkspaceConfigStore::apply_character_settings(
    std::string_view character_id,
    std::string_view provider_id,
    std::optional<std::string_view> style_id,
    std::optional<std::string_view> reasoning_effort,
    std::optional<WebSearchMode> web_search) {
    return impl_->edit([&](const Workspace& workspace) {
        std::vector<std::string> affected =
            forums_using_character(workspace, character_id);
        workspace.write_character_settings(
            character_id, provider_id, style_id, reasoning_effort, web_search);
        return affected;
    });
}

WorkspaceConfigEditResult WorkspaceConfigStore::apply_forum_default_character(
    std::string_view forum_id,
    std::string_view character_id) {
    return impl_->edit([&](const Workspace& workspace) {
        workspace.write_forum_default_character(forum_id, character_id);
        return std::vector<std::string>{std::string(forum_id)};
    });
}

WorkspaceConfigEditResult WorkspaceConfigStore::apply_forum_default_persona(
    std::string_view forum_id,
    std::string_view persona_id) {
    return impl_->edit([&](const Workspace& workspace) {
        workspace.write_forum_default_persona(forum_id, persona_id);
        return std::vector<std::string>{std::string(forum_id)};
    });
}

} // namespace cha
