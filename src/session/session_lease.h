#pragma once

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace cha {

// Reports that another process owns the workspace database lease. Callers can
// handle this expected conflict without inspecting error text.
class SessionBusyError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Holds one non-blocking, exclusive operating-system lock on a database
// companion file. SessionRepository owns it for the workspace lifetime.
class SessionLease {
public:
    SessionLease() = delete;
    ~SessionLease();

    SessionLease(SessionLease&& other) noexcept;
    SessionLease& operator=(SessionLease&& other) noexcept;
    SessionLease(const SessionLease&) = delete;
    SessionLease& operator=(const SessionLease&) = delete;

    [[nodiscard]] static SessionLease acquire(
        const std::filesystem::path& database_path,
        std::string busy_message);
    [[nodiscard]] static std::filesystem::path companion_path(
        const std::filesystem::path& database_path);

    [[nodiscard]] bool active() const noexcept;

private:
    class Impl;

    explicit SessionLease(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace cha
