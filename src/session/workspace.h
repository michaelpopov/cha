#pragma once

#include "agents/persona.h"
#include "agents/config.h"
#include "session/not_found_error.h"
#include "session/opened_session.h"
#include "session/session_catalog.h"

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

class WakeNotifier;

// A forum resolved from the workspace: its directory name, persona-facing display
// name, ordered characters, and the directory holding its instructions and sessions.
struct Forum {
    std::string name;
    std::string display_name;
    std::optional<std::string> description;
    std::vector<std::string> character_names;
    std::string default_agent_id;
    std::filesystem::path directory;
};

// A session as presented to a front end for selection: identity, label, and an optional error.
// It deliberately mirrors Session without exposing where or how the session is stored.
struct SessionSummary {
    std::string id;
    std::string label;
    std::string error;
    bool ambiguous{false};

    bool operator==(const SessionSummary&) const = default;
};

// Provider and logging settings stored in the workspace's workspace.toml.
struct WorkspaceConfig {
    std::filesystem::path log_file;
    std::string log_level;
    ProviderConfig provider;
};

// Reads the workspace configuration without validating the
// forums directory. Entry points use this to start logging before Workspace.
WorkspaceConfig load_workspace_config(
    const std::filesystem::path& root = ".");

// The way into a workspace directory and the place where a chat session is assembled. It resolves
// the layout (characters, forums), lists forums and their sessions, and on create or open loads the
// forum's AgentDefinition values, resolves the session file through SessionCatalog, restores the
// stored transcript, and returns a SessionController ready to use. Front ends call it instead of
// touching character files or session storage themselves. A Workspace is immutable
// after construction and has no lazy caches, so one instance may be shared by
// concurrent callers.
class Workspace {
public:
    explicit Workspace(std::filesystem::path root = ".");
    Workspace(std::filesystem::path root, WorkspaceConfig workspace_config);

    const WorkspaceConfig& workspace_config() const;
    const std::filesystem::path& root() const noexcept { return root_; }
    std::vector<CharacterDefinitionMetadata> character_definitions() const;
    std::string character_definition_markdown(
        const std::string& character_id) const;
    std::vector<std::string> forums() const;
    // Loads and validates the current workspace roster. Raises when personas/
    // is missing; an existing empty directory is valid and it re-reads on every call.
    PersonaRoster load_personas() const;
    Forum load_forum(const std::string& name) const;
    // Fully validates one forum without creating a session or initializing
    // completion providers. Returns its resolved metadata on success.
    Forum check_forum(const std::string& name) const;
    std::vector<SessionSummary> sessions(const std::string& forum_name) const;
    std::filesystem::file_time_type session_last_write_time(
        const std::string& forum_name,
        const std::string& session_id) const;
    // Name-facing counterparts for terminal/application callers. They resolve
    // public forum and session names while the established key APIs above stay
    // available to the web protocol.
    std::vector<SessionSummary> sessions_by_name(const std::string& forum_name) const;
    [[nodiscard]] Forum load_forum_by_name(std::string_view name) const;
    // Reads one stored session's validated metadata directly, without listing
    // or reading any other session in the forum.
    [[nodiscard]] SessionSummary session_summary(
        const std::string& forum_name,
        const std::string& session_id) const;
    [[nodiscard]] SessionSummary session_summary_by_name(
        std::string_view forum_name,
        const std::string& session_name) const;
    // Validates a stored session identity and its on-disk metadata without
    // acquiring a lease, constructing a controller, or restoring a session.
    // The web lobby uses this before asking its live-session registry to open.
    void check_session(
        const std::string& forum_name,
        const std::string& session_id) const;
    // Validates the forum and atomically publishes a stored session database.
    // It deliberately does not acquire a SessionLease, initialize providers,
    // or construct a SessionController; callers open the returned identity
    // separately through open_session().
    [[nodiscard]] SessionSummary create_stored_session(
        const std::string& forum_name,
        std::string label) const;
    [[nodiscard]] SessionSummary create_stored_session_by_name(
        std::string_view forum_name,
        std::string session_name) const;
    // Name-based callers that will immediately construct a controller retain
    // this prepared lease instead of opening a publication race.
    [[nodiscard]] PreparedSession prepare_stored_session_by_name(
        std::string_view forum_name,
        std::string session_name) const;
    [[nodiscard]] OpenedSession create_session(
        const std::string& forum_name,
        std::string label,
        WakeNotifier& notifier) const;
    [[nodiscard]] OpenedSession open_session(
        const SessionIdentity& identity,
        WakeNotifier& notifier,
        SharedPersonaRoster personas = {}) const;
    [[nodiscard]] OpenedSession open_session_by_name(
        std::string_view forum_name,
        const std::string& session_name,
        WakeNotifier& notifier) const;

private:
    std::filesystem::path forum_directory(const std::string& name) const;
    std::filesystem::path root_;
    WorkspaceConfig workspace_config_;
};

} // namespace cha
