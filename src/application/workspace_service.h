#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace cha {

class ChatCoordinator;

// Describes one selectable session without exposing its storage representation.
struct SessionSummary {
    std::string id;
    std::string label;
    std::string error;

    bool operator==(const SessionSummary&) const = default;
};

// Exposes workspace and session use cases independently of a user interface.
class WorkspaceService {
public:
    explicit WorkspaceService(std::filesystem::path root = ".");
    ~WorkspaceService();

    WorkspaceService(const WorkspaceService&) = delete;
    WorkspaceService& operator=(const WorkspaceService&) = delete;

    std::vector<std::string> rooms() const;
    std::vector<SessionSummary> sessions(
        const std::string& room_name) const;
    [[nodiscard]] std::unique_ptr<ChatCoordinator> create_session(
        const std::string& room_name,
        std::string label) const;
    [[nodiscard]] std::unique_ptr<ChatCoordinator> open_session(
        const std::string& room_name,
        const std::string& session_id) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cha
