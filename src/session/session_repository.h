#pragma once

#include "chat/session_identity.h"
#include "session/session_database.h"
#include "session/stored_session.h"

#include <filesystem>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

class Workspace;

struct TemporarySessionSeed {
    FullSessionId identity;
    std::string label;
};

struct PreparedSession {
    FullSessionId identity;
    std::string label;
    std::filesystem::path database_path;
    SessionKey session_key{};
    SessionRestore restore;
};

// Uses the already-locked unified database and the store-owned welcome
// directory. Repository operations use short-lived connections; every live
// actor opens its own journal connection.
class SessionRepository final {
public:
    class MaintenanceGuard {
    public:
        ~MaintenanceGuard() = default;
        MaintenanceGuard(MaintenanceGuard&&) = delete;
        MaintenanceGuard& operator=(MaintenanceGuard&&) = delete;
        MaintenanceGuard(const MaintenanceGuard&) = delete;
        MaintenanceGuard& operator=(const MaintenanceGuard&) = delete;

        void checkpoint() const;

    private:
        friend class SessionRepository;
        explicit MaintenanceGuard(const SessionRepository& repository);

        const SessionRepository* repository_;
        std::unique_lock<std::shared_mutex> lock_;
    };

    SessionRepository(
        std::filesystem::path database_path,
        std::filesystem::path workspace_root,
        std::filesystem::path welcome_directory,
        TemporarySessionSeed temporary);
    ~SessionRepository();

    SessionRepository(const SessionRepository&) = delete;
    SessionRepository& operator=(const SessionRepository&) = delete;

    [[nodiscard]] std::vector<StoredSession> list(
        std::string_view forum_id) const;
    [[nodiscard]] std::vector<StoredSession> recent() const;
    void validate(const FullSessionId& identity) const;
    [[nodiscard]] StoredSession create(
        std::string_view forum_id,
        std::string label) const;
    [[nodiscard]] StoredSession rename(
        const FullSessionId& identity,
        std::string label) const;
    void archive(const FullSessionId& identity) const;
    [[nodiscard]] PreparedSession prepare(
        const FullSessionId& identity) const;
    [[nodiscard]] std::vector<TranscriptEntry> history(
        const FullSessionId& identity) const;

    // Fences new repository operations and waits for existing ones. Live
    // actors write through SessionJournal and must be stopped separately.
    [[nodiscard]] MaintenanceGuard reserve_maintenance() const;
    void synchronize_forums() const;
    void synchronize_forums(const Workspace& workspace) const;

    [[nodiscard]] const std::filesystem::path& database_path() const noexcept {
        return database_path_;
    }

private:
    void require_persistent_forum(std::string_view forum_id) const;
    void synchronize_forums_unlocked(const Workspace& workspace) const;

    mutable std::shared_mutex operation_mutex_;
    std::filesystem::path workspace_root_;
    std::filesystem::path database_path_;
    FullSessionId temporary_identity_;
    std::string temporary_label_;
    std::filesystem::path temporary_database_path_;
};

} // namespace cha
