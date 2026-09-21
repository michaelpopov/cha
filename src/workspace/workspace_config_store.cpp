#include "workspace/workspace_config_store.h"

#include "storage/session_lease.h"
#include "storage/session_storage_layout.h"
#include "storage/sqlite_storage.h"
#include "storage/workspace_session_database.h"
#include "util/path_name.h"
#include "util/private_filesystem.h"
#include "workspace/workspace.h"
#include "workspace/workspace_config_editor.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
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
    std::string_view("system/keys"),
    std::string_view("system/providers"),
    std::string_view("system/styles"),
    std::string_view("system/voices"),
    std::string_view("system/voice-input"),
    std::string_view("system/voice-output"),
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
    return generic_utf8_path(file.lexically_relative(source));
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

void require_one_config_change(
    const Database& database,
    std::string_view operation,
    std::string_view name) {
    if (database.changes() == 1) return;
    throw std::runtime_error(
        "Failed to " + std::string(operation) + " configuration row '"
        + std::string(name) + "'");
}

void apply_config_changes(
    Database& database,
    const std::vector<ConfigFile>& committed,
    const std::vector<ConfigFile>& candidate) {
    // Both inputs use bytewise name order: SQLite ORDER BY name (BINARY) for
    // committed rows and std::map iteration in collect_config_rows().
    std::size_t old_index = 0;
    std::size_t new_index = 0;
    while (old_index < committed.size() || new_index < candidate.size()) {
        if (new_index == candidate.size()
            || (old_index < committed.size()
                && committed[old_index].name < candidate[new_index].name)) {
            const std::string_view name = committed[old_index].name;
            storage::SqliteStatement remove = database.prepare(
                "DELETE FROM config WHERE name = ?1", name);
            remove.run();
            require_one_config_change(database, "delete", name);
            ++old_index;
            continue;
        }

        if (old_index == committed.size()
            || candidate[new_index].name < committed[old_index].name) {
            const ConfigFile& row = candidate[new_index];
            storage::SqliteStatement insert = database.prepare(
                "INSERT INTO config (name, content) VALUES (?1, ?2)",
                std::string_view(row.name),
                std::string_view(row.content));
            insert.run();
            require_one_config_change(database, "insert", row.name);
            ++new_index;
            continue;
        }

        const ConfigFile& old_row = committed[old_index];
        const ConfigFile& new_row = candidate[new_index];
        if (old_row.content != new_row.content) {
            storage::SqliteStatement update = database.prepare(
                "UPDATE config SET content = ?2 WHERE name = ?1",
                std::string_view(new_row.name),
                std::string_view(new_row.content));
            update.run();
            require_one_config_change(database, "update", new_row.name);
        }
        ++old_index;
        ++new_index;
    }
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

// Only ephemeral session storage lives here; configuration stays in SQLite.
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


[[noreturn]] void fail_runtime_database_state(
    const std::filesystem::path& database,
    WorkspaceDatabaseState state) {
    if (state == WorkspaceDatabaseState::missing) {
        fail_path(
            "Workspace session database '" + utf8_path(database)
            + "' does not exist. Import the workspace configuration into "
              "this vault to create it.");
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

std::vector<std::string> forums_using_persona(
    const Workspace& workspace,
    std::string_view persona_id) {
    std::vector<std::string> result;
    for (const WorkspaceForum& forum : workspace.forums()) {
        if (forum.default_persona_id == persona_id) result.push_back(forum.id);
    }
    return result;
}

std::vector<std::string> forums_using_provider(
    const Workspace& workspace,
    std::string_view provider_id) {
    std::vector<std::string> result;
    for (const WorkspaceForum& forum : workspace.forums()) {
        const bool used = std::ranges::any_of(
            forum.members,
            [&](const WorkspaceForumMember& member) {
                const WorkspaceCharacter* character =
                    workspace.find_character(member.character_id);
                return character != nullptr && character->provider_id
                    && *character->provider_id == provider_id;
            });
        if (used) result.push_back(forum.id);
    }
    return result;
}

std::vector<std::string> forums_using_style(
    const Workspace& workspace,
    std::string_view style_id) {
    std::vector<std::string> result;
    for (const WorkspaceForum& forum : workspace.forums()) {
        const bool used = std::ranges::any_of(
            forum.members,
            [&](const WorkspaceForumMember& member) {
                const WorkspaceCharacter* character =
                    workspace.find_character(member.character_id);
                return character != nullptr && character->style_id
                    && *character->style_id == style_id;
            });
        const WorkspacePersona* persona =
            workspace.find_persona(forum.default_persona_id);
        if (used || (persona && persona->style_id == style_id)) {
            result.push_back(forum.id);
        }
    }
    return result;
}

std::vector<std::string> forums_using_voice(
    const Workspace& workspace,
    std::string_view voice_id) {
    std::vector<std::string> result;
    for (const WorkspaceForum& forum : workspace.forums()) {
        const bool used = std::ranges::any_of(
            forum.members,
            [&](const WorkspaceForumMember& member) {
                const WorkspaceCharacter* character =
                    workspace.find_character(member.character_id);
                return character != nullptr && character->voice_id
                    && *character->voice_id == voice_id;
            });
        const WorkspacePersona* persona =
            workspace.find_persona(forum.default_persona_id);
        if (used || (persona && persona->voice_id == voice_id)) {
            result.push_back(forum.id);
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
          "migration-capable CHA build to run 'CHA --migration "
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
            + "' is a valid CHA schema-1 database. Import the workspace "
              "configuration into this vault to upgrade it to schema 2.");
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

void validate_configuration(
    const std::vector<ConfigFile>& rows) {
    validate_config_rows(rows);
    TextFiles files;
    for (const auto& row : rows) files.emplace(row.name, row.content);
    (void)Workspace::load("/workspace", files);
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

std::string_view stored_directory_id(
    std::string_view name,
    std::string_view prefix) {
    if (!name.starts_with(prefix)) return {};
    name.remove_prefix(prefix.size());
    const auto slash = name.find('/');
    if (slash == std::string_view::npos || slash == 0) return {};
    return name.substr(0, slash);
}

void commit_imported_rows(
    const std::filesystem::path& database,
    const std::vector<ConfigFile>& rows,
    const std::set<std::string>& valid_forums,
    std::string_view database_password) {
    const WorkspaceDatabaseState state =
        inspect_workspace_session_database(database, database_password);
    switch (state) {
    case WorkspaceDatabaseState::missing: {
        create_empty_workspace_session_database(database, database_password);
        try {
            Database handle(
                database, Database::Mode::read_write, database_password);
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
        Database handle(database, Database::Mode::read_write, database_password);
        upgrade_workspace_session_database_from_v1(handle, rows);
        storage::SqliteTransaction transaction(handle);
        prune_orphan_sessions(handle, valid_forums);
        transaction.commit();
        break;
    }
    case WorkspaceDatabaseState::valid_v2: {
        Database handle(database, Database::Mode::read_write, database_password);
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
    const std::filesystem::path& database_path,
    WorkspaceConfigLease lease_mode,
    std::string_view database_password) {
    const std::filesystem::path source =
        require_existing_directory(source_directory, "Import source");
    const std::filesystem::path database = normalize_path(database_path);
    (void)require_existing_directory(database.parent_path(), "Database parent");

    std::optional<SessionLease> lease;
    if (lease_mode == WorkspaceConfigLease::acquire) {
        lease.emplace(SessionLease::acquire(database, busy_message(database)));
    }

    const bool database_exists =
        std::filesystem::is_regular_file(inspected_status(database));
    if (has_legacy_session_databases(source)) {
        fail_legacy(source, database_exists);
    }

    PrunedImport pruned = prune_imported_rows(collect_config_rows(source));
    validate_configuration(pruned.rows);

    secure_workspace_session_database_files(database);
    commit_imported_rows(
        database, pruned.rows, pruned.forums, database_password);
    return {.file_count = pruned.rows.size()};
}

WorkspaceConfigTransfer export_workspace_configuration(
    const std::filesystem::path& database_path,
    const std::filesystem::path& destination_directory,
    WorkspaceConfigLease lease_mode,
    std::string_view database_password) {
    const std::filesystem::path database = normalize_path(database_path);
    (void)require_existing_directory(database.parent_path(), "Database parent");
    const std::filesystem::path destination =
        normalize_path(destination_directory);

    std::optional<SessionLease> lease;
    if (lease_mode == WorkspaceConfigLease::acquire) {
        lease.emplace(SessionLease::acquire(database, busy_message(database)));
    }

    secure_workspace_session_database_files(database);
    const WorkspaceDatabaseState state =
        inspect_workspace_session_database(database, database_password);
    if (state != WorkspaceDatabaseState::valid_v2) {
        fail_database_state(database, state);
    }

    Database handle(database, Database::Mode::read_only, database_password);
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
    std::string database_password;
    std::optional<SessionLease> lease;
    std::unique_ptr<Database> database;
    std::optional<RuntimePrivateRoot> tree;
    mutable std::mutex mutex;
    mutable std::mutex snapshot_mutex;
    std::shared_ptr<const Workspace> published_workspace;
    bool restart_required{};
    std::function<void()> on_restart_required;

    [[noreturn]] void require_restart(std::string message) {
        restart_required = true;
        if (on_restart_required) {
            try {
                on_restart_required();
            } catch (...) {
                // Preserve the configuration failure even if stopping other
                // resources fails. Admission must already have been closed.
            }
        }
        throw WorkspaceRestartRequiredError(std::move(message));
    }

    std::shared_ptr<const Workspace> snapshot() const {
        const std::lock_guard lock(snapshot_mutex);
        return published_workspace;
    }

    void publish(Workspace workspace) {
        auto published = std::make_shared<const Workspace>(std::move(workspace));
        const std::lock_guard lock(snapshot_mutex);
        published_workspace = std::move(published);
    }

    void load_committed_workspace() {
        const auto rows = read_workspace_config_files(*database);
        validate_config_rows(rows);
        TextFiles files;
        for (const auto& row : rows) files.emplace(row.name, row.content);
        publish(Workspace::load(tree->workspace(), files));
    }

    template<typename Writer>
    WorkspaceConfigEditResult edit(
        Writer&& writer,
        std::string_view deleted_forum_id = {}) {
        const std::lock_guard lock(mutex);
        if (restart_required) {
            throw WorkspaceRestartRequiredError("Configuration is unavailable. Restart is required");
        }
        const std::shared_ptr<const Workspace> published = snapshot();
        if (!published || published->root() != tree->workspace()) {
            fail_path(
                "Runtime configuration store has no matching loaded workspace");
        }

        const auto committed_rows = read_workspace_config_files(*database);
        TextFiles files;
        for (const auto& row : committed_rows) files.emplace(row.name, row.content);
        WorkspaceConfigEditor editor(*published, files);
        auto affected_forum_ids = writer(*published, editor);
        std::vector<ConfigFile> rows;
        for (const auto& [name, content] : files) rows.push_back({name, content});
        validate_config_rows(rows);
        auto candidate = [&] {
            try {
                return std::make_shared<const Workspace>(Workspace::load(tree->workspace(), files));
            } catch (const std::runtime_error& error) {
                throw WorkspaceConfigValidationError(error.what());
            }
        }();
        if (consume_runtime_fault(WorkspaceConfigFault::validation)) {
            fail_path("Forced configuration validation failure");
        }

        // Allocate and validate before committing. A rejected candidate has no
        // side effects to restore. Hold the publication lock across commit so
        // the only remaining real operation is a noexcept pointer swap.
        std::unique_lock publication_lock(snapshot_mutex);
        bool database_committed = false;
        try {
            const bool config_changed = committed_rows != rows;
            if (config_changed || !deleted_forum_id.empty()) {
                if (consume_runtime_fault(WorkspaceConfigFault::sqlite_begin)) {
                    fail_path("Forced SQLite begin failure");
                }
                storage::SqliteTransaction transaction(*database);
                if (consume_runtime_fault(WorkspaceConfigFault::sqlite_write)) {
                    fail_path("Forced SQLite write failure");
                }
                if (config_changed) {
                    apply_config_changes(*database, committed_rows, rows);
                }
                if (!deleted_forum_id.empty()) {
                    storage::SqliteStatement delete_sessions = database->prepare(
                        "DELETE FROM sessions WHERE forum_key IN ("
                        "SELECT forum_key FROM forums WHERE forum_id = ?1)",
                        deleted_forum_id);
                    delete_sessions.run();
                    storage::SqliteStatement delete_forum = database->prepare(
                        "DELETE FROM forums WHERE forum_id = ?1",
                        deleted_forum_id);
                    delete_forum.run();
                }
                if (consume_runtime_fault(WorkspaceConfigFault::sqlite_commit)) {
                    fail_path("Forced SQLite commit failure");
                }
                transaction.commit();
                database_committed = true;
            }
            if (consume_runtime_fault(WorkspaceConfigFault::publication)) {
                fail_path("Forced workspace publication failure");
            }
            published_workspace.swap(candidate);
        } catch (const std::exception& error) {
            publication_lock.unlock();
            if (!database_committed) throw;
            require_restart(
                "Configuration was committed but could not be published: "
                + std::string(error.what()) + ". Restart is required");
        }
        return {.affected_forum_ids = std::move(affected_forum_ids)};
    }
};

WorkspaceConfigStore::WorkspaceConfigStore(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {
}

WorkspaceConfigStore::~WorkspaceConfigStore() = default;

struct WorkspaceConfigStore::MaintenanceGuard::Impl {
    explicit Impl(WorkspaceConfigStore::Impl& store)
        : store(&store), lock(store.mutex) {}

    WorkspaceConfigStore::Impl* store;
    std::unique_lock<std::mutex> lock;
    bool closed{};
};

WorkspaceConfigStore::MaintenanceGuard::MaintenanceGuard(
    std::unique_ptr<MaintenanceGuard::Impl> impl)
    : impl_(std::move(impl)) {}

WorkspaceConfigStore::MaintenanceGuard::~MaintenanceGuard() = default;

WorkspaceConfigStore::MaintenanceGuard::MaintenanceGuard(
    MaintenanceGuard&&) noexcept = default;

WorkspaceConfigStore::MaintenanceGuard&
WorkspaceConfigStore::MaintenanceGuard::operator=(
    MaintenanceGuard&&) noexcept = default;

void WorkspaceConfigStore::MaintenanceGuard::close() {
    if (!impl_ || impl_->closed) {
        throw std::logic_error("Workspace database is already closed");
    }
    impl_->store->database.reset();
    impl_->closed = true;
}

void WorkspaceConfigStore::MaintenanceGuard::set_password(
    std::string database_password) {
    if (!impl_ || !impl_->closed) {
        throw std::logic_error("Workspace database is not closed");
    }
    impl_->store->database_password = std::move(database_password);
}

void WorkspaceConfigStore::MaintenanceGuard::retarget(
    std::filesystem::path database_path,
    SessionLease lease,
    std::string database_password) {
    if (!impl_ || !impl_->closed) {
        throw std::logic_error("Workspace database is not closed");
    }
    WorkspaceConfigStore::Impl& store = *impl_->store;
    store.lease = std::move(lease);
    store.database_path = std::move(database_path);
    store.database_password = std::move(database_password);
}

void WorkspaceConfigStore::MaintenanceGuard::reopen() {
    if (!impl_ || !impl_->closed) {
        throw std::logic_error("Workspace database is not closed");
    }
    WorkspaceConfigStore::Impl& store = *impl_->store;
    try {
        secure_workspace_session_database_files(store.database_path);
        const WorkspaceDatabaseState state =
            inspect_workspace_session_database(
                store.database_path, store.database_password);
        if (state != WorkspaceDatabaseState::valid_v2) {
            fail_runtime_database_state(store.database_path, state);
        }

        store.database = std::make_unique<Database>(
            store.database_path,
            Database::Mode::read_write,
            store.database_password);
        validate_workspace_session_database_identity(*store.database);
        validate_workspace_session_contents(*store.database);
        store.database->execute("PRAGMA journal_mode = WAL");
        secure_workspace_session_database_files(store.database_path);

        store.load_committed_workspace();
        impl_->closed = false;
    } catch (const std::exception& error) {
        // Leave the store closed; the application handles maintenance recovery.
        store.database.reset();
        throw WorkspaceRestartRequiredError(
            "Failed to reopen the workspace database: "
            + std::string(error.what()) + ". Restart is required");
    }
}

std::unique_ptr<WorkspaceConfigStore> WorkspaceConfigStore::open(
    const std::filesystem::path& database_path,
    std::string database_password) {
    auto impl = std::make_unique<Impl>();
    impl->database_path = normalize_path(database_path);
    impl->database_password = std::move(database_password);
    (void)require_existing_directory(
        impl->database_path.parent_path(), "Database parent");

    impl->lease.emplace(
        SessionLease::acquire(
            impl->database_path, busy_message(impl->database_path)));

    secure_workspace_session_database_files(impl->database_path);
    const WorkspaceDatabaseState state =
        inspect_workspace_session_database(
            impl->database_path, impl->database_password);
    if (state != WorkspaceDatabaseState::valid_v2) {
        fail_runtime_database_state(impl->database_path, state);
    }

    impl->database = std::make_unique<Database>(
        impl->database_path,
        Database::Mode::read_write,
        impl->database_password);
    validate_workspace_session_database_identity(*impl->database);
    validate_workspace_session_contents(*impl->database);
    impl->database->execute("PRAGMA journal_mode = WAL");
    secure_workspace_session_database_files(impl->database_path);

    impl->tree.emplace();
    impl->load_committed_workspace();
    return std::unique_ptr<WorkspaceConfigStore>(
        new WorkspaceConfigStore(std::move(impl)));
}

std::shared_ptr<const Workspace> WorkspaceConfigStore::snapshot() const {
    return impl_->snapshot();
}

void WorkspaceConfigStore::set_restart_required_handler(std::function<void()> handler) {
    const std::lock_guard lock(impl_->mutex);
    impl_->on_restart_required = std::move(handler);
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

std::filesystem::path WorkspaceConfigStore::database_path() const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->database_path;
}

WorkspaceConfigStore::MaintenanceGuard
WorkspaceConfigStore::reserve_maintenance() {
    return MaintenanceGuard(
        std::make_unique<MaintenanceGuard::Impl>(*impl_));
}

WorkspaceConfigEditResult WorkspaceConfigStore::apply_character_settings(
    std::string_view character_id,
    std::string_view provider_id,
    std::optional<std::string_view> style_id,
    std::optional<std::string_view> voice_id,
    std::optional<std::string_view> reasoning_effort,
    std::optional<WebSearchMode> web_search) {
    return impl_->edit([&](const Workspace& workspace, WorkspaceConfigEditor& editor) {
        std::vector<std::string> affected =
            forums_using_character(workspace, character_id);
        editor.write_character_settings(
            character_id, provider_id, style_id, voice_id,
            reasoning_effort, web_search);
        return affected;
    });
}

WorkspaceConfigEditResult WorkspaceConfigStore::apply_character_definition(
    std::string_view character_id,
    std::string_view display_name,
    std::optional<std::string_view> markdown) {
    return impl_->edit([&](const Workspace& workspace, WorkspaceConfigEditor& editor) {
        std::vector<std::string> affected =
            forums_using_character(workspace, character_id);
        editor.write_character_definition(
            character_id, display_name, markdown);
        return affected;
    });
}

WorkspaceConfigEditResult WorkspaceConfigStore::apply_character_file(
    std::string_view character_id,
    std::string_view filename,
    std::optional<std::string_view> content,
    bool create) {
    return impl_->edit([&](const Workspace& workspace, WorkspaceConfigEditor& editor) {
        auto affected = forums_using_character(workspace, character_id);
        editor.write_character_file(character_id, filename, content, create);
        return affected;
    });
}

WorkspaceConfigEditResult WorkspaceConfigStore::apply_character_delete(
    std::string_view character_id) {
    return impl_->edit([&](const Workspace&, WorkspaceConfigEditor& editor) {
        editor.delete_character(character_id);
        return std::vector<std::string>{};
    });
}

WorkspaceConfigEditResult WorkspaceConfigStore::apply_persona_update(
    std::string_view persona_id,
    std::string_view display_name,
    std::string_view markdown,
    std::optional<std::string_view> style_id,
    std::optional<std::string_view> voice_id) {
    return impl_->edit([&](const Workspace& workspace, WorkspaceConfigEditor& editor) {
        std::vector<std::string> affected =
            forums_using_persona(workspace, persona_id);
        editor.write_persona(
            persona_id, display_name, markdown, style_id, voice_id);
        return affected;
    });
}

std::string WorkspaceConfigStore::create_persona(
    std::string_view display_name) {
    std::string persona_id;
    (void)impl_->edit([&](const Workspace& workspace, WorkspaceConfigEditor& editor) {
        for (std::size_t suffix = 1;; ++suffix) {
            const std::string candidate = "persona_" + std::to_string(suffix);
            const std::filesystem::path directory =
                workspace.root() / "personas" / candidate;
            if (workspace.find_persona(candidate) == nullptr
                && !editor.exists(directory)) {
                persona_id = candidate;
                break;
            }
        }
        editor.create_persona(persona_id, display_name);
        return std::vector<std::string>{};
    });
    return persona_id;
}

WorkspaceConfigEditResult WorkspaceConfigStore::apply_persona_delete(
    std::string_view persona_id) {
    return impl_->edit([&](const Workspace&, WorkspaceConfigEditor& editor) {
        editor.delete_persona(persona_id);
        return std::vector<std::string>{};
    });
}

std::string WorkspaceConfigStore::create_character(
    std::string_view display_name,
    std::string_view description) {
    std::string character_id;
    (void)impl_->edit([&](const Workspace& workspace, WorkspaceConfigEditor& editor) {
        for (std::size_t suffix = 1;; ++suffix) {
            const std::string candidate = "character_" + std::to_string(suffix);
            const std::filesystem::path directory =
                workspace.root() / "characters" / candidate;
            if (workspace.find_character(candidate) == nullptr
                && workspace.find_persona(candidate) == nullptr
                && !editor.exists(directory)) {
                character_id = candidate;
                break;
            }
        }
        editor.create_character(character_id, display_name, description);
        return std::vector<std::string>{};
    });
    return character_id;
}

std::string WorkspaceConfigStore::create_forum(
    std::string_view display_name,
    std::string_view persona_id) {
    std::string forum_id;
    (void)impl_->edit([&](const Workspace& workspace, WorkspaceConfigEditor& editor) {
        for (std::size_t suffix = 1;; ++suffix) {
            const std::string candidate = "forum_" + std::to_string(suffix);
            const std::filesystem::path directory =
                workspace.root() / "forums" / candidate;
            if (workspace.find_forum(candidate) == nullptr
                && !editor.exists(directory)) {
                forum_id = candidate;
                break;
            }
        }
        editor.create_forum(forum_id, display_name, persona_id);
        return std::vector<std::string>{};
    });
    return forum_id;
}

WorkspaceConfigEditResult WorkspaceConfigStore::apply_forum_update(
    std::string_view forum_id,
    std::string_view display_name,
    std::string_view markdown) {
    return impl_->edit([&](const Workspace&, WorkspaceConfigEditor& editor) {
        editor.write_forum(forum_id, display_name, markdown);
        return std::vector<std::string>{std::string(forum_id)};
    });
}

WorkspaceConfigEditResult WorkspaceConfigStore::apply_forum_delete(
    std::string_view forum_id) {
    return impl_->edit([&](const Workspace&, WorkspaceConfigEditor& editor) {
        editor.delete_forum(forum_id);
        return std::vector<std::string>{std::string(forum_id)};
    }, forum_id);
}

WorkspaceConfigEditResult WorkspaceConfigStore::apply_forum_file(
    std::string_view forum_id,
    std::string_view filename,
    std::optional<std::string_view> content,
    bool create) {
    return impl_->edit([&](const Workspace&, WorkspaceConfigEditor& editor) {
        editor.write_forum_file(forum_id, filename, content, create);
        return std::vector<std::string>{std::string(forum_id)};
    });
}

WorkspaceConfigEditResult WorkspaceConfigStore::apply_forum_members_and_persona(
    std::string_view forum_id,
    std::span<const std::string> character_ids,
    std::string_view persona_id) {
    return impl_->edit([&](const Workspace& workspace, WorkspaceConfigEditor& editor) {
        if (workspace.find_persona(persona_id) == nullptr) {
            throw std::invalid_argument("Invalid persona");
        }
        editor.write_forum_members(forum_id, character_ids);
        editor.write_forum_default_persona(forum_id, persona_id);
        return std::vector<std::string>{std::string(forum_id)};
    });
}

WorkspaceConfigEditResult WorkspaceConfigStore::apply_forum_default_character(
    std::string_view forum_id,
    std::string_view character_id) {
    return impl_->edit([&](const Workspace&, WorkspaceConfigEditor& editor) {
        editor.write_forum_default_character(forum_id, character_id);
        return std::vector<std::string>{std::string(forum_id)};
    });
}

WorkspaceConfigEditResult WorkspaceConfigStore::apply_provider_update(
    std::string_view provider_id,
    std::string_view display_name,
    const ModelBackendConfig& config) {
    return impl_->edit([&](const Workspace& workspace, WorkspaceConfigEditor& editor) {
        std::vector<std::string> affected =
            forums_using_provider(workspace, provider_id);
        editor.write_provider(provider_id, display_name, config);
        return affected;
    });
}

std::string WorkspaceConfigStore::create_provider(
    std::string_view display_name,
    std::string_view copy_from) {
    std::string provider_id;
    (void)impl_->edit([&](const Workspace& workspace, WorkspaceConfigEditor& editor) {
        for (std::size_t suffix = 1;; ++suffix) {
            const std::string candidate = "provider_" + std::to_string(suffix);
            const std::filesystem::path directory =
                workspace.root() / "system" / "providers" / candidate;
            if (workspace.find_provider(candidate) == nullptr
                && !editor.exists(directory)) {
                provider_id = candidate;
                break;
            }
        }
        editor.create_provider(provider_id, display_name, copy_from);
        return std::vector<std::string>{};
    });
    return provider_id;
}

WorkspaceConfigEditResult WorkspaceConfigStore::apply_provider_delete(
    std::string_view provider_id) {
    return impl_->edit([&](const Workspace&, WorkspaceConfigEditor& editor) {
        editor.delete_provider(provider_id);
        return std::vector<std::string>{};
    });
}

WorkspaceConfigEditResult WorkspaceConfigStore::apply_style_update(
    std::string_view style_id,
    std::string_view display_name,
    const CharacterAppearance& appearance) {
    return impl_->edit([&](const Workspace& workspace, WorkspaceConfigEditor& editor) {
        std::vector<std::string> affected =
            forums_using_style(workspace, style_id);
        editor.write_style(style_id, display_name, appearance);
        return affected;
    });
}

std::string WorkspaceConfigStore::create_style(
    std::string_view display_name) {
    std::string style_id;
    (void)impl_->edit([&](const Workspace& workspace, WorkspaceConfigEditor& editor) {
        for (std::size_t suffix = 1;; ++suffix) {
            const std::string candidate = "style_" + std::to_string(suffix);
            const std::filesystem::path directory =
                workspace.root() / "system" / "styles" / candidate;
            if (workspace.find_style(candidate) == nullptr
                && !editor.exists(directory)) {
                style_id = candidate;
                break;
            }
        }
        editor.create_style(style_id, display_name);
        return std::vector<std::string>{};
    });
    return style_id;
}

WorkspaceConfigEditResult WorkspaceConfigStore::apply_style_delete(
    std::string_view style_id) {
    return impl_->edit([&](const Workspace&, WorkspaceConfigEditor& editor) {
        editor.delete_style(style_id);
        return std::vector<std::string>{};
    });
}

WorkspaceConfigEditResult WorkspaceConfigStore::apply_voice_update(
    std::string_view voice_id,
    std::string_view display_name,
    std::string_view description,
    std::string_view elevenlabs_voice_id,
    const VoiceSettings& settings) {
    return impl_->edit([&](const Workspace& workspace, WorkspaceConfigEditor& editor) {
        std::vector<std::string> affected =
            forums_using_voice(workspace, voice_id);
        const WorkspaceVoice* const previous = workspace.find_voice(voice_id);
        const bool default_voice = previous && workspace.voice_output()
            && workspace.voice_output()->default_voice == previous->label;
        editor.write_voice(
            voice_id, display_name, description, elevenlabs_voice_id, settings);
        if (default_voice) {
            WorkspaceVoiceOutput output = *workspace.voice_output();
            output.default_voice = std::string(display_name);
            editor.write_voice_output(output);
        }
        return affected;
    });
}

std::string WorkspaceConfigStore::create_voice(
    std::string_view display_name,
    std::string_view description,
    std::string_view elevenlabs_voice_id) {
    std::string voice_id;
    (void)impl_->edit([&](const Workspace& workspace, WorkspaceConfigEditor& editor) {
        for (std::size_t suffix = 1;; ++suffix) {
            const std::string candidate = "voice_" + std::to_string(suffix);
            const std::filesystem::path directory =
                workspace.root() / "system" / "voices" / candidate;
            if (workspace.find_voice(candidate) == nullptr
                && !editor.exists(directory)) {
                voice_id = candidate;
                break;
            }
        }
        editor.create_voice(
            voice_id, display_name, description, elevenlabs_voice_id);
        return std::vector<std::string>{};
    });
    return voice_id;
}

WorkspaceConfigEditResult WorkspaceConfigStore::apply_voice_delete(
    std::string_view voice_id) {
    return impl_->edit([&](const Workspace&, WorkspaceConfigEditor& editor) {
        editor.delete_voice(voice_id);
        return std::vector<std::string>{};
    });
}

void WorkspaceConfigStore::apply_voice_input_update(
    const WorkspaceVoiceInput& settings) {
    (void)impl_->edit([&](const Workspace&, WorkspaceConfigEditor& editor) {
        editor.write_voice_input(settings);
        return std::vector<std::string>{};
    });
}

void WorkspaceConfigStore::apply_voice_output_update(
    const WorkspaceVoiceOutput& settings) {
    (void)impl_->edit([&](const Workspace&, WorkspaceConfigEditor& editor) {
        editor.write_voice_output(settings);
        return std::vector<std::string>{};
    });
}

void WorkspaceConfigStore::apply_api_key_create(
    std::string_view id,
    std::string_view display_name,
    std::string_view value) {
    (void)impl_->edit([&](const Workspace&, WorkspaceConfigEditor& editor) {
        editor.create_api_key(id, display_name, value);
        return std::vector<std::string>{};
    });
}

void WorkspaceConfigStore::apply_api_key_update(
    std::string_view id,
    std::string_view display_name,
    std::string_view value) {
    (void)impl_->edit([&](const Workspace&, WorkspaceConfigEditor& editor) {
        editor.write_api_key(id, display_name, value);
        return std::vector<std::string>{};
    });
}

void WorkspaceConfigStore::apply_api_key_delete(std::string_view id) {
    (void)impl_->edit([&](const Workspace&, WorkspaceConfigEditor& editor) {
        editor.delete_api_key(id);
        return std::vector<std::string>{};
    });
}

void WorkspaceConfigStore::apply_r2_storage_create(
    const R2StorageKey& key) {
    (void)impl_->edit([&](const Workspace&, WorkspaceConfigEditor& editor) {
        editor.create_r2_storage(key);
        return std::vector<std::string>{};
    });
}

void WorkspaceConfigStore::apply_r2_storage_update(
    const R2StorageKey& key) {
    (void)impl_->edit([&](const Workspace&, WorkspaceConfigEditor& editor) {
        editor.write_r2_storage(key);
        return std::vector<std::string>{};
    });
}

void WorkspaceConfigStore::apply_r2_storage_delete() {
    (void)impl_->edit([](const Workspace&, WorkspaceConfigEditor& editor) {
        editor.delete_r2_storage();
        return std::vector<std::string>{};
    });
}

void WorkspaceConfigStore::apply_key_migration(
    std::span<const SavedApiKey> api_keys,
    const std::optional<R2StorageKey>& r2_storage,
    std::uint64_t next_id) {
    (void)impl_->edit([&](const Workspace&, WorkspaceConfigEditor& editor) {
        for (const SavedApiKey& key : api_keys) {
            editor.create_api_key(key.id, key.display_name, key.value);
        }
        if (r2_storage) editor.create_r2_storage(*r2_storage);
        editor.write_next_api_key_id(next_id);
        return std::vector<std::string>{};
    });
}

void WorkspaceConfigStore::merge(
    const std::filesystem::path& source_database_path,
    const SessionLease& source_lease,
    std::string_view source_password) {
    const std::filesystem::path source = normalize_path(source_database_path);
    if (source == database_path()) {
        fail_path(
            "Source database '" + utf8_path(source)
            + "' is the active destination database");
    }
    if (!source_lease.active()) {
        fail_path("Source database lease is not active");
    }

    const WorkspaceDatabaseState state =
        inspect_workspace_session_database(source, source_password);
    if (state != WorkspaceDatabaseState::valid_v2) {
        fail_database_state(source, state);
    }

    std::vector<ConfigFile> source_rows;
    {
        Database handle(source, Database::Mode::read_write, source_password);
        validate_workspace_session_database_identity(handle);
        validate_workspace_session_contents(handle);
        source_rows = read_workspace_config_files(handle);
        validate_config_rows(source_rows);
    }

    Workspace source_workspace = [&] {
        TextFiles files;
        for (const auto& row : source_rows) files.emplace(row.name, row.content);
        return Workspace::load(source, files);
    }();
    for (const ConfigFile& row : source_rows) {
        if (const std::string_view id =
                stored_directory_id(row.name, "system/providers/");
            !id.empty() && source_workspace.find_provider(id) == nullptr) {
            fail_path(
                "Source directory 'system/providers/" + std::string(id)
                + "' is invalid");
        }
        if (const std::string_view id =
                stored_directory_id(row.name, "system/styles/");
            !id.empty() && source_workspace.find_style(id) == nullptr) {
            fail_path(
                "Source directory 'system/styles/" + std::string(id)
                + "' is invalid");
        }
    }

    (void)impl_->edit([&](const Workspace& published, WorkspaceConfigEditor& editor) {
        TextFiles& candidate = editor.files();

        // S's R2 singleton replaces D's even when the saved-key IDs differ.
        if (source_workspace.r2_storage() && published.r2_storage()) {
            const std::string prefix =
                "system/keys/" + published.r2_storage()->id + "/";
            std::erase_if(candidate, [&](const auto& row) {
                return row.first.starts_with(prefix);
            });
        }

        for (const ConfigFile& row : source_rows) {
            candidate[row.name] = row.content;
        }

        // Every merged key comes from S or D, and each loaded counter already
        // exceeds its own highest key ID, so the larger counter is safe.
        const std::uint64_t next_id = std::max(
            source_workspace.next_api_key_id(), published.next_api_key_id());
        if (next_id > static_cast<std::uint64_t>(
                std::numeric_limits<std::int64_t>::max())) {
            fail_path("API key ID space is exhausted");
        }
        candidate["system/keys/config.toml"] =
            "next_id = " + std::to_string(next_id) + "\n";

        return std::vector<std::string>{};
    });
}

} // namespace cha
