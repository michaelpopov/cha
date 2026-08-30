#pragma once

#include <cstdint>
#include <filesystem>

namespace cha {

class Workspace;

struct SessionMigrationSummary {
    std::filesystem::path database_path;
    std::uint64_t forums{};
    std::uint64_t active_sessions{};
    std::uint64_t archived_sessions{};
    std::uint64_t turns{};
    std::uint64_t entries{};
};

// Copies every legacy per-session database in a validated workspace into one
// newly published workspace database. Legacy files are opened read-only and
// are never moved or removed.
SessionMigrationSummary migrate_sessions(const Workspace& workspace);

} // namespace cha
