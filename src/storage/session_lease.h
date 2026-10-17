#pragma once

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cha {

inline constexpr char vault_in_use_message[] =
    "Close other CHA processes using this vault, then try again.";
inline constexpr char session_in_use_message[] =
    "This session is open in another CHA process.";

// Reports that another process owns the workspace database lease. Callers can
// handle this expected conflict without inspecting error text.
class SessionBusyError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Holds one non-blocking OS file lock. Vault users share a companion lock;
// maintenance, main roles, live sessions and credential refresh use exclusivity.
class SessionLease {
public:
    enum class Mode { shared, exclusive };
    SessionLease() = delete;
    ~SessionLease();

    SessionLease(SessionLease&& other) noexcept;
    SessionLease& operator=(SessionLease&& other) noexcept;
    SessionLease(const SessionLease&) = delete;
    SessionLease& operator=(const SessionLease&) = delete;

    [[nodiscard]] static SessionLease acquire(
        const std::filesystem::path& database_path,
        std::string busy_message,
        Mode mode = Mode::exclusive);
    [[nodiscard]] static SessionLease acquire_file(
        const std::filesystem::path& lock_path, std::string busy_message,
        Mode mode = Mode::exclusive);
    [[nodiscard]] static SessionLease acquire_main(const std::filesystem::path& database_path);
    [[nodiscard]] static std::filesystem::path session_path(
        const std::filesystem::path& database_path,
        std::string_view forum_id, std::string_view session_id);
    // Drops the old lock before attempting the new mode. On failure this
    // handle is unlocked; callers must have closed protected database handles.
    void change_mode(Mode mode);
    [[nodiscard]] Mode mode() const noexcept;
    [[nodiscard]] static std::filesystem::path companion_path(
        const std::filesystem::path& database_path);

    [[nodiscard]] bool active() const noexcept;

private:
    class Impl;

    explicit SessionLease(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace cha
