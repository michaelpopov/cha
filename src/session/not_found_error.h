#pragma once

#include <stdexcept>
#include <string>

namespace cha {

class ForumNotFoundError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class SessionNotFoundError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Absence reads the same wherever it is discovered, so a caller cannot tell
// which layer looked for the database.
inline SessionNotFoundError missing_session_error(const std::string& session_id) {
    return SessionNotFoundError(
        "Session '" + session_id + "' does not have a database");
}

} // namespace cha
