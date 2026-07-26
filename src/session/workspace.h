#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

class SessionController;
class WakeNotifier;

// A forum resolved from the workspace: its name, its ordered personas, and the
// directory holding its instructions and sessions.
struct Forum {
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

// A session just created on disk: the controller ready to run, and the ID the session was given so
// a front end can show it or store it to reopen the session later.
struct CreatedSession {
    std::unique_ptr<SessionController> controller;
    std::string id;
};

// The way into a workspace directory and the place where a chat session is assembled. It resolves
// the layout (personas, forums), lists forums and their sessions, and on create or open loads the
// forum's AgentDefinition values, resolves the session file through SessionCatalog, restores the
// stored transcript, and returns a SessionController ready to use. Front ends call it instead of
// touching persona files or session storage themselves.
class Workspace {
public:
    explicit Workspace(std::filesystem::path root = ".");

    std::vector<std::string> forums() const;
    Forum load_forum(const std::string& name) const;
    // Resolves the selected persona directory without loading its agent configuration.
    std::filesystem::path persona_directory(std::string_view persona_name) const;

    std::vector<SessionSummary> sessions(const std::string& forum_name) const;
    [[nodiscard]] CreatedSession create_session(
        const std::string& forum_name,
        std::string label,
        WakeNotifier& notifier) const;
    [[nodiscard]] std::unique_ptr<SessionController> open_session(
        const std::string& forum_name,
        const std::string& session_id,
        WakeNotifier& notifier) const;

private:
    std::filesystem::path forum_directory(const std::string& name) const;
    static std::vector<std::string> read_name_list(const std::filesystem::path& path);

    std::filesystem::path root_;
};

} // namespace cha
