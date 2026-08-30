#pragma once

#include "session/sqlite_storage.h"

#include <cstdint>
#include <filesystem>

namespace cha {

inline constexpr std::int64_t workspace_session_application_id =
    0x43484157; // "CHAW"
inline constexpr std::int64_t workspace_session_database_version = 1;

struct WorkspaceSessionDatabaseCounts {
    std::uint64_t forums{};
    std::uint64_t active_sessions{};
    std::uint64_t archived_sessions{};
    std::uint64_t turns{};
    std::uint64_t entries{};
};

// These are shared by the one-time importer and the final workspace runtime so
// there is only one authoritative schema definition and validator.
void create_workspace_session_schema(storage::SqliteDatabase& database);
void set_workspace_session_database_identity(
    storage::SqliteDatabase& database);
void validate_workspace_session_contents(storage::SqliteDatabase& database);
WorkspaceSessionDatabaseCounts validate_workspace_session_database(
    const std::filesystem::path& path);

} // namespace cha
