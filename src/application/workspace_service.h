#pragma once

#include "application/session_summary.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace cha {

class ChatCoordinator;
class WorkspaceService;

// Holds one validated room and its loaded agent definitions for a selection run.
class PreparedRoom {
public:
    ~PreparedRoom();

    PreparedRoom(PreparedRoom&&) noexcept;
    PreparedRoom& operator=(PreparedRoom&&) noexcept;
    PreparedRoom(const PreparedRoom&) = delete;
    PreparedRoom& operator=(const PreparedRoom&) = delete;

    [[nodiscard]] std::vector<SessionSummary> sessions() const;
    [[nodiscard]] std::unique_ptr<ChatCoordinator> create_session(
        std::string label) const;
    [[nodiscard]] std::unique_ptr<ChatCoordinator> open_session(
        const std::string& session_id) const;

private:
    friend class WorkspaceService;
    class Impl;

    explicit PreparedRoom(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

// Exposes workspace and session use cases independently of a user interface.
class WorkspaceService {
public:
    explicit WorkspaceService(std::filesystem::path root = ".");
    ~WorkspaceService();

    WorkspaceService(const WorkspaceService&) = delete;
    WorkspaceService& operator=(const WorkspaceService&) = delete;

    [[nodiscard]] std::vector<std::string> rooms() const;
    [[nodiscard]] PreparedRoom prepare_room(
        const std::string& room_name) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cha
