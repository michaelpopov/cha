#pragma once

#include "session/session_database.h"
#include "session/session_identity.h"
#include "session/session_lease.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cha {

class SessionCatalog;

// Where one forum's stored sessions live. Application startup reads these off
// the loaded workspace and hands them to SessionRepository, so the repository
// never learns the workspace layout and the session layer never depends on an
// application type.
struct ForumSessionDirectory {
    std::string forum_id;
    std::filesystem::path directory;
};

// The one process-local session the repository creates and owns for itself.
// The application names it; this layer holds no built-in identifiers.
struct TemporarySessionSeed {
    SessionIdentity identity;
    std::string label;
};

// One stored session as offered for selection, with the modification time the
// lobby orders by. A non-empty error marks a database that is identifiable but
// failed its metadata check; it stays listed under a fallback label.
struct SessionEntry {
    SessionIdentity identity;
    std::string label;
    std::string error;
    std::filesystem::file_time_type updated_at;
};

// A session's storage, validated and leased, ready for a controller. Holding
// this value holds the lease: destroying it releases the session again.
struct PreparedSession {
    SessionIdentity identity;
    std::string label;
    std::filesystem::path database_path;
    SessionLease lease;
    SessionRestore restore;
};

// Owns every session-storage operation the application needs: the persistent
// per-forum catalogs and the one temporary session it creates at construction
// and removes at destruction. It holds only an immutable forum-to-directory
// map and constructs catalog helpers per operation, so a constructed
// repository serves concurrent const calls; exclusion comes from the existing
// catalog and session leases.
class SessionRepository final {
public:
    SessionRepository(
        std::vector<ForumSessionDirectory> persistent,
        TemporarySessionSeed temporary);
    ~SessionRepository();

    SessionRepository(const SessionRepository&) = delete;
    SessionRepository& operator=(const SessionRepository&) = delete;

    // Tolerant: an identifiable database with invalid metadata stays visible.
    // Results are ordered by session ID.
    [[nodiscard]] std::vector<SessionEntry> list(std::string_view forum_id) const;
    // Strict: throws for a missing or invalid selected session.
    void validate(const SessionIdentity& identity) const;
    // The temporary forum has no creation path and reports ForumNotFoundError.
    [[nodiscard]] SessionEntry create(std::string_view forum_id, std::string label) const;
    [[nodiscard]] PreparedSession prepare(const SessionIdentity& identity) const;

private:
    [[nodiscard]] SessionCatalog catalog_for(std::string_view forum_id) const;

    std::unordered_map<std::string, std::filesystem::path> persistent_;
    SessionIdentity temporary_identity_;
    std::string temporary_label_;
    std::filesystem::path temporary_directory_;
    std::filesystem::path temporary_database_path_;
};

} // namespace cha
