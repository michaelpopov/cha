#include "workspace/workspace_config_store.h"

#include "storage/session_lease.h"
#include "storage/session_storage_layout.h"
#include "storage/sqlite_storage.h"
#include "storage/workspace_session_database.h"
#include "util/base64.h"
#include "util/curl.h"
#include "util/logging.h"
#include "util/path_name.h"
#include "util/picture.h"
#include "util/private_filesystem.h"
#include "workspace/builtins.h"
#include "workspace/workspace.h"
#include "workspace/workspace_config_editor.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
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
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace cha {
namespace {

using Database = storage::SqliteDatabase;

std::string allocate_character_id(
    const Workspace& workspace, const WorkspaceConfigEditor& editor) {
    for (std::size_t suffix = 1;; ++suffix) {
        const std::string candidate = "character_" + std::to_string(suffix);
        if (!workspace.find_character(candidate) && !workspace.find_persona(candidate)
            && !editor.exists(workspace.root() / "characters" / candidate)) {
            return candidate;
        }
    }
}

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
        && (name.ends_with(".toml") || name.ends_with(".md") || picture_format(name));
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

// Pictures belong only in character definition folders and the Assistant folder.
bool is_picture_directory(
    const std::filesystem::path& source,
    const std::filesystem::path& directory) {
    const std::string name = stored_name_from(source, directory);
    return name == "system/assistant"
        || (name.starts_with("characters/")
            && std::filesystem::is_regular_file(
                inspected_status(directory / "character.toml")));
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
        if (std::filesystem::is_directory(status)) {
            if (is_forum_member_directory(name)) {
                forum_member_directories.insert(name);
            }
            continue;
        }
        const bool image_extension =
            supported_picture_extension(utf8_path(file.extension()));
        const bool picture = image_extension && picture_format(name)
            && is_picture_directory(source, file.parent_path());
        if (image_extension && !picture) {
            log_warn("Ignoring picture file '" + name + "': unsupported name or location");
            continue;
        }
        if (std::filesystem::is_symlink(status)) {
            if (is_accepted_stored_name(name)) {
                fail_path(
                    "Configuration path '" + utf8_path(file)
                    + "' is a symbolic link");
            }
            continue;
        }
        if (!std::filesystem::is_regular_file(status)) continue;
        if (!is_accepted_stored_name(name)) continue;
        validate_stored_config_name(name);
        std::string content = read_file_bytes(file);
        if (picture) content = encode_base64(content);
        if (!files.emplace(name, std::move(content)).second) {
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
        std::string content = row.content;
        if (picture_format(row.name)) {
            try {
                content = decode_base64(row.content);
            } catch (const std::runtime_error&) {
                fail_path("Picture '" + row.name + "' has malformed base64");
            }
        }
        create_private_file(path, content);
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
    const auto validation_error = [](std::string message) {
        constexpr std::string_view virtual_root = "/workspace";
        std::size_t position{};
        while ((position = message.find(virtual_root, position))
            != std::string::npos) {
            const std::size_t end = position + virtual_root.size();
            if (end < message.size() && message[end] == '/') {
                message.erase(position, virtual_root.size() + 1);
            } else {
                message.replace(position, virtual_root.size(), "workspace");
                position += std::string_view("workspace").size();
            }
        }
        return WorkspaceConfigValidationError(std::move(message));
    };
    try {
        validate_config_rows(rows);
        TextFiles files;
        for (const auto& row : rows) files.emplace(row.name, row.content);
        // Validation reads these in-memory rows, never the real modify path.
        (void)Workspace::load("/workspace", files, nullptr);
    } catch (const toml::parse_error& error) {
        std::string message = error.source().path
            ? "Config file '" + *error.source().path + "': "
            : "Invalid TOML: ";
        message += error.description();
        message += " (line " + std::to_string(error.source().begin.line)
            + ", column " + std::to_string(error.source().begin.column) + ")";
        throw validation_error(std::move(message));
    } catch (const std::filesystem::filesystem_error&) {
        throw;
    } catch (const std::runtime_error& error) {
        throw validation_error(error.what());
    } catch (const std::invalid_argument& error) {
        throw validation_error(error.what());
    }
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

bool is_writable_config_root(std::string_view path) {
    static constexpr std::string_view roots[]{
        "characters/",
        "personas/",
        "forums/",
        "system/providers/",
        "system/styles/",
        "system/voices/",
        "system/jev/",
        "system/web-search/",
        "system/session/",
        "system/voice-input/",
        "system/voice-output/",
    };
    for (const std::string_view root : roots) {
        if (path.starts_with(root)) return true;
    }
    return false;
}

bool matches_list_prefix(std::string_view path, std::string_view prefix) {
    if (prefix.empty()) return true;
    if (!path.starts_with(prefix)) return false;
    if (path.size() == prefix.size() || prefix.ends_with('/')) return true;
    return path[prefix.size()] == '/';
}

bool is_assistant_member_path(std::string_view path) {
    constexpr std::string_view forums = "forums/";
    constexpr std::string_view members = "/members/";
    constexpr std::string_view member = "builtin-assistant";
    if (!path.starts_with(forums)) return false;
    path.remove_prefix(forums.size());
    const auto forum_end = path.find('/');
    if (forum_end == std::string_view::npos || forum_end == 0) return false;
    path.remove_prefix(forum_end);
    if (!path.starts_with(members)) return false;
    path.remove_prefix(members.size());
    return path == member || path.starts_with(std::string(member) + "/");
}

bool is_assistant_provider_path(
    std::string_view path, std::string_view provider_id) {
    if (provider_id.empty()) return false;
    const std::string prefix = "system/providers/" + std::string(provider_id);
    return path == prefix || path.starts_with(prefix + "/");
}

std::string assistant_provider_id(const Workspace& workspace) {
    const WorkspaceCharacter* const assistant =
        workspace.find_character(assistant_id);
    if (assistant && assistant->provider_id) return *assistant->provider_id;
    return {};
}

struct ConfigPathPolicy {
    bool readable{true};
    bool writable{false};
    // Assistant and credential files. Other read-only paths are unsupported.
    bool protected_file{false};
    std::string reason;
};

ConfigPathPolicy config_path_policy(
    std::string_view path, std::string_view assistant_provider) {
    if (picture_format(path)) {
        return {false, false, true, "Pictures are not available to configuration tools"};
    }
    if (path.starts_with("system/assistant/")) {
        return {true, false, true, "Assistant settings are read-only"};
    }
    if (is_assistant_member_path(path)) {
        return {true, false, true, "Assistant member files are read-only"};
    }
    if (is_assistant_provider_path(path, assistant_provider)) {
        return {true, false, true, "Assistant's selected provider is read-only"};
    }
    if (path.starts_with("system/keys/")) {
        return {true, false, true, "Credentials cannot be written"};
    }
    if (!is_writable_config_root(path)) {
        return {true, false, false, "Unsupported configuration path"};
    }
    return {true, true, false, {}};
}

std::string sanitize_config_error(
    std::string message, const std::filesystem::path& root) {
    const std::string prefixes[]{utf8_path(root), generic_utf8_path(root)};
    for (const std::string& prefix : prefixes) {
        if (prefix.empty()) continue;
        const std::string slash = prefix + '/';
        std::size_t position = 0;
        while ((position = message.find(slash)) != std::string::npos) {
            message.erase(position, slash.size());
        }
        position = 0;
        while ((position = message.find(prefix, position)) != std::string::npos) {
            message.replace(position, prefix.size(), "workspace");
            position += std::string_view("workspace").size();
        }
    }
    return message;
}

std::string resolve_key_reference(
    const Workspace& workspace,
    std::string_view api_key_id,
    std::string_view api_key_env) {
    if (!api_key_id.empty()) return std::string(api_key_id);
    if (api_key_env.empty()) return {};
    for (const SavedApiKey& key : workspace.api_keys()) {
        if (key.display_name == api_key_env) return key.id;
    }
    return std::string(api_key_env);
}

std::string provider_credential_destination(const ModelBackendConfig& config) {
    std::string host = config.host;
    if (host.find(':') != std::string::npos && !host.starts_with('[')) {
        host = '[' + host + ']';
    }
    return std::string(config.https ? "https://" : "http://") + host + ':'
        + std::to_string(config.port);
}

std::string url_credential_destination(std::string_view url) {
    std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(
        curl_url(), curl_url_cleanup);
    const std::string owned(url);
    if (!parsed
        || curl_url_set(parsed.get(), CURLUPART_URL, owned.c_str(), 0) != CURLUE_OK) {
        return owned;
    }
    char* scheme = nullptr;
    char* host = nullptr;
    char* port = nullptr;
    curl_url_get(parsed.get(), CURLUPART_SCHEME, &scheme, 0);
    curl_url_get(parsed.get(), CURLUPART_HOST, &host, 0);
    curl_url_get(parsed.get(), CURLUPART_PORT, &port, CURLU_DEFAULT_PORT);
    std::unique_ptr<char, decltype(&curl_free)> owned_scheme(scheme, curl_free);
    std::unique_ptr<char, decltype(&curl_free)> owned_host(host, curl_free);
    std::unique_ptr<char, decltype(&curl_free)> owned_port(port, curl_free);
    if (!scheme || !host) return owned;
    std::string destination = std::string(scheme) + "://" + host;
    if (port) {
        destination += ':';
        destination += port;
    }
    return destination;
}

using CredentialPair = std::pair<std::string, std::string>;

void add_credential_pair(
    std::set<CredentialPair>& pairs, std::string destination, std::string key) {
    if (destination.empty() || key.empty()) return;
    pairs.emplace(std::move(destination), std::move(key));
}

bool is_provider_config(std::string_view name) {
    constexpr std::string_view prefix = "system/providers/";
    constexpr std::string_view suffix = "/config.toml";
    if (!name.starts_with(prefix) || !name.ends_with(suffix)) return false;
    const std::string_view id =
        name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
    return !id.empty() && id.find('/') == std::string_view::npos;
}

// Pairs as written in the configuration rows, with the loader's defaults.
// Entries that the loader omits or ignores count too. Then a repair of a broken
// entry keeps its pair, and a broken new entry cannot add a pair for later.
// A file that does not parse adds no pair.
std::set<CredentialPair> credential_destination_pairs(
    const Workspace& workspace,
    const std::vector<ConfigFile>& rows) {
    std::set<CredentialPair> pairs;
    const auto text = [](const toml::table& table, std::string_view field) {
        return table[field].value_or(std::string{});
    };
    for (const ConfigFile& row : rows) {
        const bool provider = is_provider_config(row.name);
        if (!provider
            && row.name != "system/jev/config.toml"
            && row.name != "system/voice-input/config.toml"
            && row.name != "system/voice-output/config.toml"
            && row.name != "system/web-search/config.toml") {
            continue;
        }
        toml::table table;
        try {
            table = toml::parse(row.content);
        } catch (const toml::parse_error&) {
            continue;
        }
        const auto key = [&](const toml::table& source) {
            return resolve_key_reference(workspace, text(source, "api_key"), {});
        };
        if (provider) {
            ModelBackendConfig config;
            config.host = text(table, "host");
            config.port = table["port"].value_or(0);
            config.https = table["https"].value_or(false);
            add_credential_pair(
                pairs,
                provider_credential_destination(config),
                resolve_key_reference(
                    workspace, text(table, "api_key"), text(table, "api_key_env")));
        } else if (row.name == "system/voice-output/config.toml") {
            add_credential_pair(pairs, "fishaudio", key(table));
            if (const toml::table* eleven = table["elevenlabs"].as_table()) {
                add_credential_pair(pairs, "elevenlabs", key(*eleven));
            }
        } else if (row.name == "system/web-search/config.toml") {
            add_credential_pair(
                pairs, table["provider"].value_or(std::string("brave")), key(table));
            add_credential_pair(
                pairs, "firecrawl",
                resolve_key_reference(workspace, text(table, "firecrawl_api_key"), {}));
        } else {
            add_credential_pair(pairs, url_credential_destination(text(table, "url")), key(table));
        }
    }
    return pairs;
}

bool entities_preserved(const Workspace& base, const Workspace& candidate) {
    for (const WorkspaceCharacter& character : base.characters()) {
        if (candidate.find_character(character.character.id) == nullptr) {
            return false;
        }
    }
    for (const WorkspacePersona& persona : base.personas()) {
        if (candidate.find_persona(persona.id) == nullptr) return false;
    }
    for (const WorkspaceForum& forum : base.forums()) {
        if (candidate.find_forum(forum.id) == nullptr) return false;
        for (const WorkspaceForumMember& member : forum.members) {
            if (candidate.find_forum_member(forum.id, member.character_id)
                == nullptr) {
                return false;
            }
        }
    }
    for (const WorkspaceProvider& provider : base.providers()) {
        if (candidate.find_provider(provider.id) == nullptr) return false;
    }
    for (const WorkspaceStyle& style : base.styles()) {
        if (candidate.find_style(style.id) == nullptr) return false;
    }
    for (const WorkspaceVoice& voice : base.voices()) {
        if (candidate.find_voice(voice.id) == nullptr) return false;
    }
    return true;
}

std::optional<WorkspaceConfigKeyInfo> key_metadata_for(
    std::string_view path, std::string_view content) {
    constexpr std::string_view prefix = "system/keys/";
    if (!path.starts_with(prefix)) return std::nullopt;
    const std::string_view rest = path.substr(prefix.size());
    if (rest == "config.toml") return std::nullopt;
    const auto slash = rest.find('/');
    if (slash == std::string_view::npos || rest.substr(slash) != "/config.toml") {
        return WorkspaceConfigKeyInfo{
            .id = std::string(rest),
            .type = "unknown",
        };
    }
    WorkspaceConfigKeyInfo info;
    info.id = std::string(rest.substr(0, slash));
    try {
        const toml::table table = toml::parse(std::string(content));
        info.display_name = table["display_name"].value_or(std::string{});
        info.type = table["type"].value_or(std::string{});
        if (info.type == "models") {
            info.credential_present =
                !table["value"].value_or(std::string{}).empty();
        } else if (info.type == "R2") {
            info.credential_present =
                !table["secret_key"].value_or(std::string{}).empty();
        }
    } catch (const toml::parse_error&) {
    }
    return info;
}

bool cancelled(const WorkspaceConfigCancelCheck& check) {
    return check && check();
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
    WorkspaceConfigRevision revision{};
    struct UndoRecord {
        WorkspaceConfigRevision resulting_revision{};
        std::vector<std::pair<std::string, std::string>> old_files;
    };
    std::optional<UndoRecord> undo;

    enum class UndoUpdate { leave, clear, install };

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
        ++revision;
        undo.reset();
    }

    void require_open() const {
        if (restart_required) {
            throw WorkspaceRestartRequiredError(
                "Configuration is unavailable. Restart is required");
        }
    }

    bool undo_is_available() const {
        return undo && undo->resulting_revision == revision;
    }

    WorkspaceConfigApplyResult failed(
        WorkspaceConfigApplyError error, std::string message) const {
        return {
            .committed = false,
            .revision = revision,
            .undo_available = undo_is_available(),
            .error = error,
            .error_message = std::move(message),
        };
    }

    void commit_and_publish(
        const std::vector<ConfigFile>& committed_rows,
        const std::vector<ConfigFile>& rows,
        std::shared_ptr<const Workspace> candidate,
        std::string_view deleted_forum_id,
        UndoUpdate undo_update,
        std::optional<UndoRecord> new_undo = {}) {
        std::unique_lock publication_lock(snapshot_mutex);
        bool database_committed = false;
        bool config_changed = false;
        try {
            config_changed = committed_rows != rows;
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
            if (config_changed) {
                ++revision;
                if (undo_update == UndoUpdate::clear) undo.reset();
                else if (undo_update == UndoUpdate::install) {
                    undo = std::move(new_undo);
                }
            }
        } catch (const std::exception& error) {
            publication_lock.unlock();
            if (!database_committed) throw;
            if (config_changed) {
                ++revision;
                undo.reset();
            }
            require_restart(
                "Configuration was committed but could not be published: "
                + std::string(error.what()) + ". Restart is required");
        }
    }

    template<typename Writer>
    WorkspaceConfigEditResult edit(
        Writer&& writer,
        std::string_view deleted_forum_id = {}) {
        const std::lock_guard lock(mutex);
        require_open();
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
                return std::make_shared<const Workspace>(
                    Workspace::load(tree->workspace(), files));
            } catch (const std::runtime_error& error) {
                throw WorkspaceConfigValidationError(error.what());
            }
        }();
        if (consume_runtime_fault(WorkspaceConfigFault::validation)) {
            fail_path("Forced configuration validation failure");
        }

        commit_and_publish(
            committed_rows, rows, std::move(candidate), deleted_forum_id,
            UndoUpdate::clear);
        return {.affected_forum_ids = std::move(affected_forum_ids)};
    }

    WorkspaceConfigListResult list_config(std::string_view prefix) const {
        require_open();
        const std::shared_ptr<const Workspace> published = snapshot();
        const std::string provider = assistant_provider_id(*published);
        const auto rows = read_workspace_config_files(*database);
        WorkspaceConfigListResult result;
        result.revision = revision;
        for (const ConfigFile& row : rows) {
            if (!matches_list_prefix(row.name, prefix)) continue;
            const ConfigPathPolicy policy = config_path_policy(row.name, provider);
            if (!policy.readable) continue;
            if (result.entries.size() == workspace_config_list_limit) {
                result.truncated = true;
                break;
            }
            result.entries.push_back({
                .path = row.name,
                .bytes = row.content.size(),
                .readable = policy.readable,
                .writable = policy.writable,
                .protection_reason = policy.reason,
            });
        }
        return result;
    }

    WorkspaceConfigReadResult read_config(std::span<const std::string> paths) const {
        require_open();
        const auto rows = read_workspace_config_files(*database);
        TextFiles files;
        for (const ConfigFile& row : rows) files.emplace(row.name, row.content);
        WorkspaceConfigReadResult result;
        result.revision = revision;
        std::size_t result_bytes = 0;
        for (const std::string& path : paths) {
            WorkspaceConfigReadItem item{.path = path};
            const auto found = files.find(path);
            if (picture_format(path) || found == files.end()) {
                item.status = WorkspaceConfigReadStatus::missing;
                result.files.push_back(std::move(item));
                continue;
            }
            if (auto key = key_metadata_for(path, found->second)) {
                item.status = WorkspaceConfigReadStatus::key_metadata;
                item.key = std::move(key);
                result.files.push_back(std::move(item));
                continue;
            }
            if (found->second.size() > workspace_config_file_size_limit
                || result_bytes + found->second.size()
                    > workspace_config_call_size_limit) {
                item.status = WorkspaceConfigReadStatus::too_large;
                result.files.push_back(std::move(item));
                continue;
            }
            item.status = WorkspaceConfigReadStatus::ok;
            item.content = found->second;
            result_bytes += item.content.size();
            result.files.push_back(std::move(item));
        }
        return result;
    }

    // Checks shared by apply, undo, and creation before they read the committed rows.
    std::optional<WorkspaceConfigApplyResult> refuse_batch(
        WorkspaceConfigRevision version,
        const WorkspaceConfigCancelCheck& check,
        std::string_view action) const {
        if (cancelled(check)) {
            return failed(
                WorkspaceConfigApplyError::cancelled,
                "Configuration " + std::string(action) + " was cancelled");
        }
        // A restart error thrown after this check always means a committed write.
        if (restart_required) {
            return failed(
                WorkspaceConfigApplyError::restart_required,
                "Configuration is unavailable. Restart is required");
        }
        const std::shared_ptr<const Workspace> published = snapshot();
        if (!published || published->root() != tree->workspace()) {
            fail_path(
                "Runtime configuration store has no matching loaded workspace");
        }
        if (version != revision) {
            return failed(
                WorkspaceConfigApplyError::stale_version,
                "Configuration version is stale");
        }
        return std::nullopt;
    }

    struct Candidate {
        std::vector<ConfigFile> rows;
        std::shared_ptr<const Workspace> workspace;
        LoadWarningCollector warnings;
    };

    // Validates the row names and loads the candidate. Returns a failure, or
    // nothing when the candidate loaded. A bad row name is the caller's path
    // for apply, but stored data for undo, so each caller names its error.
    std::optional<WorkspaceConfigApplyResult> load_candidate(
        const TextFiles& files,
        WorkspaceConfigApplyError row_error,
        Candidate& candidate) const {
        for (const auto& [name, content] : files) {
            candidate.rows.push_back({name, content});
        }
        try {
            validate_config_rows(candidate.rows);
        } catch (const std::runtime_error& error) {
            return failed(
                row_error,
                sanitize_config_error(error.what(), tree->workspace()));
        }
        try {
            candidate.workspace = std::make_shared<const Workspace>(
                Workspace::load(tree->workspace(), files, &candidate.warnings));
        } catch (const std::runtime_error& error) {
            return failed(
                WorkspaceConfigApplyError::validation_failure,
                sanitize_config_error(error.what(), tree->workspace()));
        }
        if (consume_runtime_fault(WorkspaceConfigFault::validation)) {
            fail_path("Forced configuration validation failure");
        }
        return std::nullopt;
    }

    // Commits a loaded candidate. Reports only warnings from changed paths.
    WorkspaceConfigApplyResult commit_candidate(
        const std::vector<ConfigFile>& committed_rows,
        Candidate candidate,
        std::vector<WorkspaceConfigChangedPath> changed,
        const WorkspaceConfigCancelCheck& check,
        std::string_view action,
        UndoUpdate undo_update,
        std::optional<UndoRecord> new_undo = {}) {
        std::set<std::string> changed_paths;
        for (const auto& item : changed) changed_paths.insert(item.path);
        std::vector<LoadWarning> warnings;
        for (const LoadWarning& warning : candidate.warnings) {
            if (changed_paths.contains(warning.path)) warnings.push_back(warning);
        }
        if (cancelled(check)) {
            return failed(
                WorkspaceConfigApplyError::cancelled,
                "Configuration " + std::string(action) + " was cancelled");
        }
        commit_and_publish(
            committed_rows, candidate.rows, std::move(candidate.workspace), {},
            undo_update, std::move(new_undo));
        return {
            .committed = true,
            .revision = revision,
            .changed = std::move(changed),
            .warnings = std::move(warnings),
            .undo_available = undo_is_available(),
        };
    }

    // Only root scalar edits are supported. Equal typed values retain the source.
    static std::string set_config_value(
        const std::string& content, const WorkspaceConfigChange& change) {
        if (change.key.empty() || !std::ranges::all_of(change.key, [](unsigned char ch) {
                return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
                    || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-';
            })) {
            throw std::invalid_argument("set requires a nonempty ASCII root key");
        }
        toml::table table = toml::parse(content);
        const auto* node = table.get(change.key);
        if (node && (node->is_table() || node->is_array())) {
            throw std::invalid_argument("set cannot edit or remove a table or array");
        }
        if (!change.value) {
            if (!node) return content;
            table.erase(change.key);
        } else {
            const bool equal = std::visit([&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, double>) {
                    if (!std::isfinite(value)) {
                        throw std::invalid_argument("set requires a finite number");
                    }
                }
                const auto* typed = node ? node->as<T>() : nullptr;
                return typed && typed->get() == value;
            }, *change.value);
            if (equal) return content;
            std::visit([&](const auto& value) {
                table.insert_or_assign(change.key, value);
            }, *change.value);
        }
        std::ostringstream output;
        output << table << '\n';
        return output.str();
    }

    WorkspaceConfigApplyResult apply_config(
        WorkspaceConfigRevision version,
        std::span<const WorkspaceConfigChange> changes,
        const WorkspaceConfigCancelCheck& check) {
        if (auto refused = refuse_batch(version, check, "apply")) return *refused;
        if (changes.empty()) {
            return failed(
                WorkspaceConfigApplyError::invalid_argument,
                "Configuration apply requires at least one change");
        }

        const std::shared_ptr<const Workspace> published = snapshot();
        const std::string provider = assistant_provider_id(*published);
        std::set<std::string> seen;
        std::size_t total_bytes = 0;
        for (const WorkspaceConfigChange& change : changes) {
            if (!seen.insert(change.path).second) {
                return failed(
                    WorkspaceConfigApplyError::invalid_argument,
                    "Duplicate configuration path '" + change.path + "'");
            }
            if (change.content.size() > workspace_config_file_size_limit) {
                return failed(
                    WorkspaceConfigApplyError::too_large,
                    "Configuration file '" + change.path + "' exceeds 64 KiB");
            }
            total_bytes += change.content.size();
            try {
                validate_stored_config_name(change.path);
            } catch (const std::runtime_error& error) {
                return failed(
                    WorkspaceConfigApplyError::invalid_path, error.what());
            }
            const ConfigPathPolicy policy =
                config_path_policy(change.path, provider);
            if (policy.protected_file) {
                return failed(
                    WorkspaceConfigApplyError::protected_path, policy.reason);
            }
            if (!policy.writable) {
                return failed(
                    WorkspaceConfigApplyError::invalid_path,
                    "Configuration path '" + change.path + "' is not writable");
            }
        }
        if (total_bytes > workspace_config_call_size_limit) {
            return failed(
                WorkspaceConfigApplyError::too_large,
                "Configuration apply exceeds 256 KiB");
        }

        const auto committed_rows = read_workspace_config_files(*database);
        TextFiles files;
        for (const ConfigFile& row : committed_rows) {
            files.emplace(row.name, row.content);
        }
        bool created_file = false;
        std::vector<WorkspaceConfigChangedPath> changed;
        UndoRecord record;
        for (const WorkspaceConfigChange& change : changes) {
            const auto found = files.find(change.path);
            const bool exists = found != files.end();
            if (change.operation == WorkspaceConfigOperation::create) {
                if (exists) {
                    return failed(
                        WorkspaceConfigApplyError::create_replace_conflict,
                        "Configuration file '" + change.path + "' already exists");
                }
                created_file = true;
            } else if (!exists) {
                return failed(
                    WorkspaceConfigApplyError::create_replace_conflict,
                    "Configuration file '" + change.path + "' does not exist");
            }
            std::string content = change.content;
            if (change.operation == WorkspaceConfigOperation::set) {
                const std::string where = "Configuration file '" + change.path + "': ";
                if (std::filesystem::path(change.path).extension() != ".toml") {
                    return failed(WorkspaceConfigApplyError::invalid_argument,
                        where + "set requires an existing TOML file");
                }
                try {
                    content = set_config_value(found->second, change);
                } catch (const toml::parse_error& error) {
                    return failed(WorkspaceConfigApplyError::validation_failure,
                        where + sanitize_config_error(error.what(), tree->workspace()));
                } catch (const std::invalid_argument& error) {
                    return failed(WorkspaceConfigApplyError::invalid_argument, where + error.what());
                }
                if (content.size() > workspace_config_file_size_limit) {
                    return failed(WorkspaceConfigApplyError::too_large,
                        "Configuration file '" + change.path + "' exceeds 64 KiB");
                }
            }
            const std::optional<std::size_t> old_bytes =
                exists ? std::optional<std::size_t>(found->second.size())
                       : std::nullopt;
            if (!exists || found->second != content) {
                if (exists) {
                    record.old_files.emplace_back(change.path, found->second);
                }
                files[change.path] = content;
                changed.push_back({
                    .path = change.path,
                    .old_bytes = old_bytes,
                    .new_bytes = content.size(),
                });
            }
        }
        if (changed.empty()) {
            return {
                .committed = true,
                .revision = revision,
                .undo_available = undo_is_available(),
            };
        }

        Candidate candidate;
        if (auto failure = load_candidate(
                files, WorkspaceConfigApplyError::invalid_path, candidate)) {
            return *failure;
        }
        if (!entities_preserved(*published, *candidate.workspace)) {
            return failed(
                WorkspaceConfigApplyError::invalid_argument,
                "Configuration changes cannot remove existing entities or memberships");
        }
        const auto base_pairs = credential_destination_pairs(*published, committed_rows);
        const auto next_pairs =
            credential_destination_pairs(*candidate.workspace, candidate.rows);
        for (const auto& pair : next_pairs) {
            if (!base_pairs.contains(pair)) {
                return failed(
                    WorkspaceConfigApplyError::credential_destination_protected,
                    "Credential destinations cannot be added through Assistant");
            }
        }

        record.resulting_revision = revision + 1;
        if (created_file) {
            return commit_candidate(
                committed_rows, std::move(candidate), std::move(changed), check,
                "apply", UndoUpdate::clear);
        }
        return commit_candidate(
            committed_rows, std::move(candidate), std::move(changed), check,
            "apply", UndoUpdate::install, std::move(record));
    }

    WorkspaceConfigApplyResult undo_config(
        WorkspaceConfigRevision version, const WorkspaceConfigCancelCheck& check) {
        if (auto refused = refuse_batch(version, check, "undo")) return *refused;
        if (!undo_is_available()) {
            return failed(
                WorkspaceConfigApplyError::unavailable_undo,
                "Configuration undo is not available");
        }

        const auto committed_rows = read_workspace_config_files(*database);
        TextFiles files;
        for (const ConfigFile& row : committed_rows) {
            files.emplace(row.name, row.content);
        }
        std::vector<WorkspaceConfigChangedPath> changed;
        for (const auto& [path, content] : undo->old_files) {
            const auto found = files.find(path);
            const std::optional<std::size_t> old_bytes =
                found == files.end() ? std::nullopt
                                     : std::optional<std::size_t>(found->second.size());
            files[path] = content;
            changed.push_back({
                .path = path,
                .old_bytes = old_bytes,
                .new_bytes = content.size(),
            });
        }

        Candidate candidate;
        if (auto failure = load_candidate(
                files, WorkspaceConfigApplyError::validation_failure, candidate)) {
            return *failure;
        }
        return commit_candidate(
            committed_rows, std::move(candidate), std::move(changed), check,
            "undo", UndoUpdate::clear);
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
        ensure_entry_metadata_columns(*store.database);
        ensure_session_lifecycle_columns(*store.database);
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
    ensure_entry_metadata_columns(*impl->database);
    ensure_session_lifecycle_columns(*impl->database);
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

void WorkspaceConfigStore::apply_character_picture(
    std::string_view character_id,
    std::string_view filename,
    std::string_view content_base64) {
    (void)impl_->edit([&](const Workspace&, WorkspaceConfigEditor& editor) {
        editor.write_character_picture(character_id, filename, content_base64);
        return std::vector<std::string>{};
    });
}

WorkspaceConfigEditResult WorkspaceConfigStore::apply_character_settings(
    std::string_view character_id,
    std::string_view provider_id,
    std::optional<std::string_view> style_id,
    std::optional<std::string_view> voice_id,
    std::optional<std::string_view> reasoning_effort,
    std::optional<WebSearchMode> web_search,
    std::optional<bool> web_search_tool) {
    return impl_->edit([&](const Workspace& workspace, WorkspaceConfigEditor& editor) {
        std::vector<std::string> affected =
            forums_using_character(workspace, character_id);
        editor.write_character_settings(
            character_id, provider_id, style_id, voice_id,
            reasoning_effort, web_search, web_search_tool);
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
        character_id = allocate_character_id(workspace, editor);
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
    const VoiceSettings& settings,
    std::string_view provider) {
    return impl_->edit([&](const Workspace& workspace, WorkspaceConfigEditor& editor) {
        std::vector<std::string> affected =
            forums_using_voice(workspace, voice_id);
        const WorkspaceVoice* const previous = workspace.find_voice(voice_id);
        const bool default_voice = previous && workspace.voice_output()
            && workspace.voice_output()->default_voice == previous->label;
        editor.write_voice(
            voice_id, display_name, description, elevenlabs_voice_id, settings, provider);
        if (default_voice && previous->label != display_name) {
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
    std::string_view elevenlabs_voice_id,
    std::string_view provider) {
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
            voice_id, display_name, description, elevenlabs_voice_id, provider);
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

void WorkspaceConfigStore::apply_jev_update(const std::optional<WorkspaceJev>& settings) {
    (void)impl_->edit([&](const Workspace&, WorkspaceConfigEditor& editor) {
        editor.write_jev(settings);
        return std::vector<std::string>{};
    });
}

void WorkspaceConfigStore::apply_session_naming_update(const WorkspaceSessionNaming& settings) {
    (void)impl_->edit([&](const Workspace&, WorkspaceConfigEditor& editor) {
        editor.write_session_naming(settings);
        return std::vector<std::string>{};
    });
}

void WorkspaceConfigStore::apply_web_search_update(const WorkspaceWebSearch& settings) {
    (void)impl_->edit([&](const Workspace&, WorkspaceConfigEditor& editor) {
        editor.write_web_search(settings);
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

std::optional<WorkspaceCharacterPicture> WorkspaceConfigStore::get_character_picture(
    std::string_view character_id) const {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_open();
    const auto workspace = impl_->snapshot();
    const auto directory = workspace->character_directory(character_id);
    if (!directory) throw std::out_of_range("That character was not found.");
    const auto relative = directory->lexically_relative(workspace->root());
    for (const auto& format : picture_formats) {
        auto content = read_workspace_config_file(
            *impl_->database, generic_utf8_path(relative / format.filename));
        if (content) {
            return WorkspaceCharacterPicture{
                .filename = std::string(format.filename),
                .mime_type = std::string(format.mime_type),
                .content_base64 = std::move(*content),
            };
        }
    }
    return std::nullopt;
}

WorkspaceConfigRevision WorkspaceConfigStore::config_revision() const {
    const std::lock_guard lock(impl_->mutex);
    impl_->require_open();
    return impl_->revision;
}

WorkspaceConfigListResult WorkspaceConfigStore::list_config(
    std::string_view prefix) const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->list_config(prefix);
}

WorkspaceConfigReadResult WorkspaceConfigStore::read_config(
    std::span<const std::string> paths) const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->read_config(paths);
}

WorkspaceConfigApplyResult WorkspaceConfigStore::apply_config(
    WorkspaceConfigRevision version,
    std::span<const WorkspaceConfigChange> changes,
    WorkspaceConfigCancelCheck cancelled) {
    const std::lock_guard lock(impl_->mutex);
    return impl_->apply_config(version, changes, cancelled);
}

WorkspaceConfigApplyResult WorkspaceConfigStore::create_character_for_assistant(
    WorkspaceConfigRevision version,
    std::string_view name,
    std::string_view description,
    std::string_view provider_id,
    std::string_view profile,
    std::optional<std::string_view> forum_id,
    std::string& allocated_id,
    WorkspaceConfigCancelCheck cancelled) {
    const std::lock_guard lock(impl_->mutex);
    allocated_id.clear();
    if (auto refused = impl_->refuse_batch(version, cancelled, "creation")) return *refused;
    const auto workspace = impl_->snapshot();
    const auto committed = read_workspace_config_files(*impl_->database);
    TextFiles files;
    for (const auto& row : committed) files.emplace(row.name, row.content);
    const auto original = files;
    WorkspaceConfigEditor editor(*workspace, files);
    try {
        allocated_id = allocate_character_id(*workspace, editor);
        editor.create_character(allocated_id, name, description, provider_id, profile);
        if (forum_id) editor.add_prepared_character_to_forum(*forum_id, allocated_id);
    } catch (const std::invalid_argument& error) {
        return impl_->failed(WorkspaceConfigApplyError::invalid_argument,
            sanitize_config_error(error.what(), impl_->tree->workspace()));
    }
    std::vector<WorkspaceConfigChange> changes;
    for (const auto& [path, content] : original) {
        if (!files.contains(path)) throw std::logic_error("Character creation removed a row");
    }
    for (const auto& [path, content] : files) {
        const auto old = original.find(path);
        if (old == original.end() || old->second != content) {
            changes.push_back({.path = path,
                .operation = old == original.end() ? WorkspaceConfigOperation::create
                    : WorkspaceConfigOperation::replace,
                .content = content});
        }
    }
    return impl_->apply_config(version, changes, cancelled);
}

WorkspaceConfigApplyResult WorkspaceConfigStore::undo_config(
    WorkspaceConfigRevision version, WorkspaceConfigCancelCheck cancelled) {
    const std::lock_guard lock(impl_->mutex);
    return impl_->undo_config(version, cancelled);
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
