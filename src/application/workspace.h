#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

class ChatCoordinator;

// Describes a resolved room, including its ordered persona roster and storage directory.
struct Room {
    std::string name;
    std::vector<std::string> persona_names;
    std::filesystem::path directory;
};

// Describes one selectable session without exposing its storage representation.
struct SessionSummary {
    std::string id;
    std::string label;
    std::string error;

    bool operator==(const SessionSummary&) const = default;
};

// Exposes workspace layout, room/session use cases, and coordinator construction.
class Workspace {
public:
    explicit Workspace(std::filesystem::path root = ".");

    std::vector<std::string> rooms() const;
    Room load_room(const std::string& name) const;
    // Resolves the selected persona directory without loading its agent configuration.
    std::filesystem::path persona_directory(std::string_view persona_name) const;

    std::vector<SessionSummary> sessions(const std::string& room_name) const;
    [[nodiscard]] std::unique_ptr<ChatCoordinator> create_session(
        const std::string& room_name,
        std::string label) const;
    [[nodiscard]] std::unique_ptr<ChatCoordinator> open_session(
        const std::string& room_name,
        const std::string& session_id) const;

private:
    std::filesystem::path room_directory(const std::string& name) const;
    static std::vector<std::string> read_name_list(const std::filesystem::path& path);

    std::filesystem::path root_;
};

} // namespace cha
