#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

class SessionController;

// A room resolved from the workspace: its name, its ordered personas, and the
// directory holding its instructions and sessions.
struct Room {
    std::string name;
    std::vector<std::string> persona_names;
    std::filesystem::path directory;
};

// A session as presented to a front end for selection: identity, label, and an optional error.
// It deliberately mirrors Session without exposing where or how the session is stored.
struct SessionSummary {
    std::string id;
    std::string label;
    std::string error;

    bool operator==(const SessionSummary&) const = default;
};

// The way into a workspace directory and the place where a chat session is assembled. It resolves
// the layout (personas, rooms), lists rooms and their sessions, and on create or open loads the
// room's AgentDefinition values, resolves the session file through SessionCatalog, restores the
// stored transcript, and returns a SessionController ready to use. Front ends call it instead of
// touching persona files or session storage themselves.
class Workspace {
public:
    explicit Workspace(std::filesystem::path root = ".");

    std::vector<std::string> rooms() const;
    Room load_room(const std::string& name) const;
    // Resolves the selected persona directory without loading its agent configuration.
    std::filesystem::path persona_directory(std::string_view persona_name) const;

    std::vector<SessionSummary> sessions(const std::string& room_name) const;
    [[nodiscard]] std::unique_ptr<SessionController> create_session(
        const std::string& room_name,
        std::string label) const;
    [[nodiscard]] std::unique_ptr<SessionController> open_session(
        const std::string& room_name,
        const std::string& session_id) const;

private:
    std::filesystem::path room_directory(const std::string& name) const;
    static std::vector<std::string> read_name_list(const std::filesystem::path& path);

    std::filesystem::path root_;
};

} // namespace cha
