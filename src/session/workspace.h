#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

class SessionController;
class WakeNotifier;

// A forum resolved from the workspace: its directory name, user-facing display
// name, ordered personas, and the directory holding its instructions and sessions.
struct Forum {
    std::string name;
    std::string display_name;
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

// Process-wide settings stored in the workspace's app.toml.
struct ApplicationConfig {
    std::filesystem::path log_file;
    std::string log_level;
};

// Reads the workspace-level application configuration without validating the
// forums directory. Entry points use this to start logging before Workspace.
ApplicationConfig load_application_config(
    const std::filesystem::path& root = ".");

// The way into a workspace directory and the place where a chat session is assembled. It resolves
// the layout (personas, forums), lists forums and their sessions, and on create or open loads the
// forum's AgentDefinition values, resolves the session file through SessionCatalog, restores the
// stored transcript, and returns a SessionController ready to use. Front ends call it instead of
// touching persona files or session storage themselves.
class Workspace {
public:
    explicit Workspace(std::filesystem::path root = ".");
    Workspace(std::filesystem::path root, ApplicationConfig app_config);

    const ApplicationConfig& app_config() const;
    std::vector<std::string> forums() const;
    Forum load_forum(const std::string& name) const;
    // Fully validates one forum without creating a session or initializing
    // completion providers. Returns its resolved metadata on success.
    Forum check_forum(const std::string& name) const;
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
    std::filesystem::path root_;
    ApplicationConfig app_config_;
};

} // namespace cha
