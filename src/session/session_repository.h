#pragma once

#include "session/session_database.h"
#include "session/session_identity.h"
#include "session/session_lease.h"
#include "session/stored_session.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

class SessionCatalog;

// The one process-local session the repository creates and owns for itself.
// The application names it; this layer holds no built-in identifiers.
struct TemporarySessionSeed {
    SessionIdentity identity;
    std::string label;
};

// A session's storage, validated and leased, ready for a controller. Holding
// this value holds the lease: destroying it releases the session again. This is
// the only storage value that proves anything about the session it names; a
// StoredSession observed before the lease does not.
struct PreparedSession {
    SessionIdentity identity;
    std::string label;
    std::filesystem::path database_path;
    SessionLease lease;
    SessionRestore restore;
};

// Owns every session-storage operation the application needs: the persistent
// per-forum catalogs and the one temporary session it creates at construction
// and removes at destruction. It constructs catalog helpers per operation, so
// a repository serves concurrent const calls; exclusion comes from session
// leases. Persistent forum existence and paths come from the currently
// published Workspace; it caches no workspace configuration or session rows.
class SessionRepository final {
public:
    SessionRepository(
        std::filesystem::path workspace_root,
        TemporarySessionSeed temporary);
    ~SessionRepository();

    SessionRepository(const SessionRepository&) = delete;
    SessionRepository& operator=(const SessionRepository&) = delete;

    // Tolerant: an identifiable database with invalid metadata stays visible.
    // Results are ordered by session ID.
    [[nodiscard]] std::vector<StoredSession> list(std::string_view forum_id) const;
    // Strict: throws for a missing or invalid selected session.
    void validate(const SessionIdentity& identity) const;
    // The temporary forum has no creation path and reports ForumNotFoundError.
    [[nodiscard]] StoredSession create(std::string_view forum_id, std::string label) const;
    [[nodiscard]] StoredSession rename(
        const SessionIdentity& identity,
        std::string label) const;
    // Recoverable deletion moves the database into the forum's deleted/
    // directory. The companion lease file deliberately stays in place.
    void move_to_deleted(const SessionIdentity& identity) const;
    [[nodiscard]] PreparedSession prepare(const SessionIdentity& identity) const;

private:
    [[nodiscard]] SessionCatalog catalog_for(std::string_view forum_id) const;

    std::filesystem::path workspace_root_;
    SessionIdentity temporary_identity_;
    std::string temporary_label_;
    std::filesystem::path temporary_directory_;
    std::filesystem::path temporary_database_path_;
};

} // namespace cha
