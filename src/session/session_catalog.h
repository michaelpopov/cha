#pragma once

#include "session/not_found_error.h"
#include "session/stored_session.h"

#include <ctime>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace cha {

// Owns the session files of one forum, so no other component has to know how sessions are laid out
// on disk. It lists them, checking the identity embedded in each against the file it came from,
// creates new ones without ever overwriting an existing destination, and resolves a session ID to
// the database path used to open it. The clock is injectable to keep generated names testable.
// After construction, one catalog may receive concurrent calls when its injected Clock is itself
// safe for concurrent invocation. There is no directory-wide lock: concurrent create() calls take a
// SessionLease per candidate stem, publish atomically without replacing a destination, and retry
// the next suffix on a collision. Which creator receives the unsuffixed ID is therefore
// unspecified. The catalog holds no mutable cache.
class SessionCatalog {
public:
    using Clock = std::function<std::time_t()>;

    SessionCatalog(
        std::filesystem::path directory,
        std::string forum_name,
        Clock clock = {});

    // Tolerant: an identifiable database that fails its metadata check stays
    // listed under a fallback label with the failure in its error. Ordered by
    // session ID.
    std::vector<StoredSession> list() const;
    // Strict: reads and validates exactly one stored session without scanning
    // the catalog. Absence is reported separately from invalid metadata.
    [[nodiscard]] StoredSession inspect(const std::string& session_id) const;
    [[nodiscard]] StoredSession create(std::string label) const;
    // Safe path derivation only: the returned path names where a session's
    // database belongs, whether or not one is there.
    std::filesystem::path database_path(const std::string& session_id) const;

private:
    // The label a database claims, once its embedded identity has been checked
    // against the file it came from. Throws for anything else.
    [[nodiscard]] std::string validated_label(
        const std::filesystem::path& path,
        const std::string& session_id) const;
    std::filesystem::path directory_;
    std::string forum_name_;
    Clock clock_;
};

} // namespace cha
