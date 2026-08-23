#pragma once

#include "chat/session_identity.h"

#include <filesystem>
#include <string>

namespace cha {

// One observation of a stored session, taken while the catalog inspected it:
// where it lives, what it calls itself, and when it last changed. It owns every
// field it reports and holds no database handle and no lease, so it says
// nothing about the session's state after it is returned — another process may
// already have leased, rewritten, or removed the file. Never pass one on as
// proof that opening will succeed; only PreparedSession, which holds the lease,
// carries that.
//
// An empty error means the database's embedded metadata matched the file it
// came from. Listing keeps an identifiable but invalid database visible under a
// fallback label with the validation failure in error; strict operations reject
// it instead.
struct StoredSession {
    FullSessionId identity;
    std::string label;
    std::filesystem::path database_path;
    std::filesystem::file_time_type updated_at;
    std::string error;

    bool operator==(const StoredSession&) const = default;
};

} // namespace cha
