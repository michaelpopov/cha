#pragma once

#include "agents/character.h"
#include "agents/character_config.h"
#include "chat/persona.h"
#include "session/opened_session.h"
#include "session/session_identity.h"
#include "session/session_repository.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cha {

class WakeNotifier;
class SessionRepository;

// Provider and logging settings stored in the workspace's workspace.toml.
struct WorkspaceConfig {
    std::filesystem::path log_file;
    std::string log_level;
    ProviderConfig provider;
};

// Reads the workspace configuration without validating the rest of the
// workspace. Entry points use this to start logging before the model loads.
WorkspaceConfig load_workspace_config(
    const std::filesystem::path& root = ".");

// One forum as the browser sees it. It deliberately carries no filesystem
// path: routes should not learn the workspace layout.
struct ForumInfo {
    ForumId id;
    std::string display_name;
    std::optional<std::string> description;
    std::vector<CharacterId> member_ids;
    CharacterId default_character_id;
};

// The authoritative static workspace for one server process. It is loaded once
// at startup and performs no filesystem reads afterwards, so every discovery
// response and every newly opened session sees the same definitions. Session
// databases remain dynamic and are owned by SessionRepository.
class WorkspaceModel final {
public:
    static WorkspaceModel load(
        std::filesystem::path root,
        WorkspaceConfig config);

    const SharedPersonaRoster& personas() const noexcept { return personas_; }
    std::span<const CharacterMetadata> characters() const noexcept {
        return characters_;
    }
    std::span<const ForumInfo> forums() const noexcept { return forums_; }

    const CharacterMetadata* find_character(
        std::string_view id) const noexcept;
    const ForumInfo* find_forum(std::string_view id) const noexcept;
    std::string_view character_markdown(std::string_view id) const;

    // The only path-bearing values the model publishes, needed once by startup
    // to construct SessionRepository.
    std::vector<ForumSessionDirectory> session_directories() const;

private:
    WorkspaceModel() = default;

    // Full definitions may carry provider credentials, so they stay off the
    // general API. open_session() is the one production caller that needs them.
    friend OpenedSession open_session(
        const WorkspaceModel&,
        const SessionRepository&,
        const SessionIdentity&,
        WakeNotifier&);

    std::vector<CharacterDefinition> copy_definitions_for(
        std::string_view forum_id) const;

    WorkspaceConfig config_;
    SharedPersonaRoster personas_;
    std::vector<CharacterMetadata> characters_;
    std::vector<ForumInfo> forums_;
    std::unordered_map<std::string, std::size_t> character_index_;
    std::unordered_map<std::string, std::size_t> forum_index_;
    std::unordered_map<std::string, std::string> character_markdown_;
    std::unordered_map<std::string, std::vector<CharacterDefinition>> definitions_;
    std::vector<ForumSessionDirectory> session_directories_;
};

} // namespace cha
