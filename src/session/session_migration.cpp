#include "session/session_migration.h"

#include "session/legacy_session_import.h"
#include "session/session_archive.h"
#include "session/session_delete_conflict.h"
#include "session/session_lease.h"
#include "session/session_storage_layout.h"
#include "session/sqlite_storage.h"
#include "session/workspace_session_database.h"
#include "util/path_name.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace cha {
namespace {

using Database = storage::SqliteDatabase;
using Statement = storage::SqliteStatement;
using Transaction = storage::SqliteTransaction;

std::int64_t scalar(Database& database, std::string_view sql) {
    Statement statement = database.prepare(sql);
    if (!statement.step()) {
        throw std::runtime_error(
            "SQLite query returned no value for '" + database.path() + "'");
    }
    return statement.integer(0);
}

std::map<std::string, std::int64_t> insert_forums(
    Database& target,
    const Workspace& workspace) {
    std::map<std::string, std::int64_t> result;
    for (const WorkspaceForum& forum : workspace.forums()) {
        if (!workspace.forum_session_directory(forum.id)) continue;
        Statement insert = target.prepare(
            "INSERT INTO forums (forum_id) VALUES (?1)",
            std::string_view(forum.id));
        insert.run();
        result.emplace(forum.id, target.last_insert_rowid());
    }
    return result;
}

void require_expected_counts(
    const WorkspaceSessionDatabaseCounts& counts,
    std::uint64_t expected_forums,
    std::uint64_t expected_sessions,
    std::uint64_t expected_turns,
    std::uint64_t expected_entries) {
    if (counts.forums != expected_forums
        || counts.active_sessions + counts.archived_sessions != expected_sessions
        || counts.turns != expected_turns
        || counts.entries != expected_entries) {
        throw std::runtime_error(
            "Workspace session database counts do not match legacy sources");
    }
}

std::filesystem::path sidecar(
    const std::filesystem::path& database,
    std::string_view suffix) {
    std::filesystem::path result = database;
    result += suffix;
    return result;
}

void remove_generated_target(const std::filesystem::path& path) noexcept {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    std::filesystem::remove(sidecar(path, "-journal"), ignored);
    std::filesystem::remove(sidecar(path, "-wal"), ignored);
    std::filesystem::remove(sidecar(path, "-shm"), ignored);
}

SessionMigrationSummary make_summary(
    const std::filesystem::path& path,
    const WorkspaceSessionDatabaseCounts& counts) {
    return {
        .database_path = path,
        .forums = counts.forums,
        .active_sessions = counts.active_sessions,
        .archived_sessions = counts.archived_sessions,
        .turns = counts.turns,
        .entries = counts.entries,
    };
}

} // namespace

SessionMigrationSummary migrate_sessions(const Workspace& workspace) {
    const std::filesystem::path target =
        workspace_session_database_path(workspace);
    const std::filesystem::path temporary =
        workspace_session_migration_path(workspace);
    SessionLease lease = SessionLease::acquire(
        target,
        "Workspace already in use: '" + utf8_path(workspace.root()) + "'");

    if (std::filesystem::exists(target)) {
        throw std::runtime_error(
            "Workspace session database already exists at '"
            + utf8_path(target) + "'");
    }
    if (std::filesystem::exists(temporary)) {
        throw std::runtime_error(
            "Unfinished migration output already exists at '"
            + utf8_path(temporary)
            + "'; remove it explicitly before retrying");
    }

    std::vector<LegacySessionSource> discovered =
        discover_legacy_session_sources(workspace);
    if (discovered.empty()) {
        throw std::runtime_error(
            "No legacy session databases were found in workspace '"
            + utf8_path(workspace.root()) + "'");
    }

    std::vector<ValidatedLegacySession> sources;
    sources.reserve(discovered.size());
    std::map<FullSessionId, std::filesystem::path> identities;
    std::uint64_t expected_turns{};
    std::uint64_t expected_entries{};
    for (LegacySessionSource& source : discovered) {
        ValidatedLegacySession validated =
            validate_legacy_session_source(std::move(source));
        const auto [existing, inserted] = identities.emplace(
            validated.source.expected_identity, validated.source.path);
        if (!inserted) {
            throw std::runtime_error(
                "Duplicate legacy session identity '"
                + validated.source.expected_identity.forum_id + '/'
                + validated.source.expected_identity.session_id + "' in '"
                + utf8_path(existing->second) + "' and '"
                + utf8_path(validated.source.path) + "'");
        }
        expected_turns += validated.turns;
        expected_entries += validated.entries;
        sources.push_back(std::move(validated));
    }

    std::uint64_t expected_forums{};
    for (const WorkspaceForum& forum : workspace.forums()) {
        if (workspace.forum_session_directory(forum.id)) ++expected_forums;
    }
    const std::int64_t archived_at = std::max<std::int64_t>(0,
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    try {
        {
            Database database(temporary, Database::Mode::read_write_create);
            create_workspace_session_schema(database);
            Transaction transaction(database);
            const auto forums = insert_forums(database, workspace);
            for (const ValidatedLegacySession& source : sources) {
                const auto forum =
                    forums.find(source.source.expected_identity.forum_id);
                if (forum == forums.end()) {
                    throw std::logic_error(
                        "Validated source belongs to an unknown forum");
                }
                import_legacy_session(
                    database, source, forum->second, archived_at);
            }

            if (static_cast<std::uint64_t>(scalar(
                    database, "SELECT COUNT(*) FROM sessions")) != sources.size()
                || static_cast<std::uint64_t>(scalar(
                    database, "SELECT COUNT(*) FROM turns")) != expected_turns
                || static_cast<std::uint64_t>(scalar(
                    database, "SELECT COUNT(*) FROM entries")) != expected_entries) {
                throw std::runtime_error(
                    "Imported workspace session counts do not match legacy sources");
            }
            validate_workspace_session_contents(database);
            set_workspace_session_database_identity(database);
            transaction.commit();
        }

        WorkspaceSessionDatabaseCounts counts =
            validate_workspace_session_database(temporary);
        require_expected_counts(
            counts,
            expected_forums,
            sources.size(),
            expected_turns,
            expected_entries);
        try {
            archive_without_replacement(temporary, target);
        } catch (const SessionDeleteConflictError&) {
            throw std::runtime_error(
                "Workspace session database already exists at '"
                + utf8_path(target) + "'");
        }
        counts = validate_workspace_session_database(target);
        require_expected_counts(
            counts,
            expected_forums,
            sources.size(),
            expected_turns,
            expected_entries);
        return make_summary(target, counts);
    } catch (...) {
        remove_generated_target(temporary);
        throw;
    }
}

} // namespace cha
