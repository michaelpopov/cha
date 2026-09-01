#pragma once

#include "session/sqlite_storage.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

inline constexpr std::int64_t workspace_session_application_id =
    0x43484157; // "CHAW"
inline constexpr std::int64_t workspace_session_database_version = 2;
inline constexpr std::int64_t workspace_session_database_version_v1 = 1;

struct ConfigFile {
    std::string name;
    std::string content;
};

enum class WorkspaceDatabaseState {
    missing,
    valid_v1,
    valid_v2,
    wrong_application_id,
    unsupported_version,
    corrupt,
};

// The workspace runtime has one authoritative schema definition and validator.
void create_workspace_session_schema(storage::SqliteDatabase& database);
void set_workspace_session_database_identity(
    storage::SqliteDatabase& database);
void validate_workspace_session_database_identity(
    storage::SqliteDatabase& database);
void validate_workspace_session_contents(storage::SqliteDatabase& database);

[[nodiscard]] WorkspaceDatabaseState inspect_workspace_session_database(
    const std::filesystem::path& path);

void validate_stored_config_name(std::string_view name);
[[nodiscard]] std::vector<ConfigFile> read_workspace_config_files(
    storage::SqliteDatabase& database);
void replace_workspace_config_files(
    storage::SqliteDatabase& database,
    const std::vector<ConfigFile>& rows);
void upgrade_workspace_session_database_from_v1(
    storage::SqliteDatabase& database,
    const std::vector<ConfigFile>& rows);

// Runtime lifecycle helpers. Creation is for a genuinely new workspace only
// and refuses an existing path. Runtime initialization secures existing
// durable files, validates a complete v2 database before enabling WAL, and
// recreates only an empty, unidentified file left by an interrupted first
// creation. A valid v1 database is never upgraded or deleted at runtime.
void create_empty_workspace_session_database(
    const std::filesystem::path& path);
void secure_workspace_session_database_files(
    const std::filesystem::path& path);
void initialize_workspace_session_database_runtime(
    const std::filesystem::path& path);
void checkpoint_workspace_session_database(
    const std::filesystem::path& path);

} // namespace cha
