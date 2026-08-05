#pragma once

#include "session/session_lease.h"
#include "session/not_found_error.h"

#include <ctime>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace cha {

// One stored session as offered for selection: its ID, the label shown to the persona, and an error
// message when the file exists but failed its identity check.
struct Session {
    std::string id;
    std::string label;
    std::string error;
    bool ambiguous{false};

    bool operator==(const Session&) const = default;
};

// The storage result of a public-name creation. It keeps the new session's
// lease alive from atomic publication through controller construction.
struct PreparedSession {
    Session session;
    std::filesystem::path database_path;
    SessionLease lease;

    PreparedSession(Session stored, std::filesystem::path path, SessionLease held_lease)
        : session(std::move(stored)), database_path(std::move(path)), lease(std::move(held_lease)) {}
    PreparedSession(PreparedSession&&) noexcept = default;
    PreparedSession& operator=(PreparedSession&&) noexcept = default;
    PreparedSession(const PreparedSession&) = delete;
    PreparedSession& operator=(const PreparedSession&) = delete;
};

class SessionNameAmbiguousError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class SessionNameExistsError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class InvalidSessionNameError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Owns the session files of one forum, so no other component has to know how sessions are laid out
// on disk. It lists them, checking the identity embedded in each against the file it came from,
// creates new ones without ever overwriting an existing destination, and resolves a session ID to
// the database path used to open it. The clock is injectable to keep generated names testable.
// After construction, one catalog may receive concurrent calls when its injected Clock is itself
// safe for concurrent invocation. Concurrent create() calls publish atomically and retry filename
// collisions; the catalog holds no mutable cache.
class SessionCatalog {
public:
    using Clock = std::function<std::time_t()>;

    SessionCatalog(
        std::filesystem::path directory,
        std::string forum_name,
        Clock clock = {});

    std::vector<Session> list() const;
    // Lists public metadata names. Corrupt rows deliberately have no private
    // ID-derived label, and colliding healthy public names are annotated.
    std::vector<Session> list_by_name() const;
    // Reads and validates exactly one stored session without scanning the
    // catalog. Absence is reported separately from invalid metadata.
    [[nodiscard]] Session session(const std::string& session_id) const;
    [[nodiscard]] Session create(std::string label) const;
    // Resolves one public name by scanning metadata. It never chooses an
    // ambiguous folded match.
    [[nodiscard]] Session session_by_name(const std::string& name) const;
    [[nodiscard]] PreparedSession create_by_name(std::string name) const;
    std::filesystem::path database_path(const std::string& session_id) const;
    // Revalidates the embedded identity before returning the selected database.
    // Absence is reported separately from storage or metadata failures.
    std::filesystem::path open_database_path(const std::string& session_id) const;

private:
    [[nodiscard]] Session validated_session(
        const std::filesystem::path& path,
        const std::string& session_id) const;
    std::filesystem::path directory_;
    std::string forum_name_;
    Clock clock_;
};

} // namespace cha
