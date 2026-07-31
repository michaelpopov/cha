#pragma once

#include "session/not_found_error.h"

#include <ctime>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace cha {

// One stored session as offered for selection: its ID, the label shown to the user, and an error
// message when the file exists but failed its identity check.
struct Session {
    std::string id;
    std::string label;
    std::string error;

    bool operator==(const Session&) const = default;
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
    [[nodiscard]] Session create(std::string label) const;
    std::filesystem::path database_path(const std::string& session_id) const;
    // Revalidates the embedded identity before returning the selected database.
    // Absence is reported separately from storage or metadata failures.
    std::filesystem::path open_database_path(const std::string& session_id) const;

private:
    std::filesystem::path directory_;
    std::string forum_name_;
    Clock clock_;
};

} // namespace cha
