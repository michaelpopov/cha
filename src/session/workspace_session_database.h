#pragma once

#include "session/sqlite_storage.h"

#include <cstdint>
#include <filesystem>

namespace cha {

inline constexpr std::int64_t workspace_session_application_id =
    0x43484157; // "CHAW"
inline constexpr std::int64_t workspace_session_database_version = 1;

// The workspace runtime has one authoritative schema definition and validator.
void create_workspace_session_schema(storage::SqliteDatabase& database);
void set_workspace_session_database_identity(
    storage::SqliteDatabase& database);
void validate_workspace_session_database_identity(
    storage::SqliteDatabase& database);
void validate_workspace_session_contents(storage::SqliteDatabase& database);

// Runtime lifecycle helpers. Creation is for a genuinely new workspace only
// and refuses an existing path. Runtime initialization validates the complete
// database before enabling WAL; it recreates only an empty, unidentified file
// left by an interrupted first creation.
void create_empty_workspace_session_database(
    const std::filesystem::path& path);
void initialize_workspace_session_database_runtime(
    const std::filesystem::path& path);
void checkpoint_workspace_session_database(
    const std::filesystem::path& path);

} // namespace cha
