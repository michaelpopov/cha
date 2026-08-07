#pragma once

#include "session/session_lease.h"

#include <filesystem>
#include <utility>

namespace cha {

struct PreparedWelcomeSession {
    std::filesystem::path database_path;
    SessionLease lease;

    PreparedWelcomeSession(
        std::filesystem::path path,
        SessionLease held_lease)
        : database_path(std::move(path)), lease(std::move(held_lease)) {}
    PreparedWelcomeSession(PreparedWelcomeSession&&) noexcept = default;
    PreparedWelcomeSession& operator=(PreparedWelcomeSession&&) noexcept = default;
    PreparedWelcomeSession(const PreparedWelcomeSession&) = delete;
    PreparedWelcomeSession& operator=(const PreparedWelcomeSession&) = delete;
};

class WelcomeStorage {
public:
    WelcomeStorage();
    ~WelcomeStorage();
    WelcomeStorage(const WelcomeStorage&) = delete;
    WelcomeStorage& operator=(const WelcomeStorage&) = delete;
    const std::filesystem::path& directory() const noexcept { return directory_; }
    const std::filesystem::path& database_path() const noexcept { return database_path_; }
    std::filesystem::file_time_type last_write_time() const;
    PreparedWelcomeSession prepare();

private:
    std::filesystem::path directory_;
    std::filesystem::path database_path_;
};
} // namespace cha
