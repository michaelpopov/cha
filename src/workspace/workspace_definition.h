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
    std::string default_persona_id;
};

// The authoritative workspace for one server process. Definitions are loaded
// once at startup and never re-read, so every discovery response and every
// newly opened session sees the same characters, personas and prompts. A
// forum's default character is the one setting that stays live: it is read
// from config.toml when a session opens and written back there by /@. Session
// databases remain dynamic and are owned by SessionRepository.
class WorkspaceDefinition final {
public:
    static WorkspaceDefinition load(
        std::filesystem::path root,
        WorkspaceConfig config);

    const SharedPersonaRoster& personas() const noexcept { return personas_; }
    std::span<const CharacterMetadata> characters() const noexcept {
        return characters_;
    }
    std::span<const ForumInfo> forums() const noexcept { return forums_; }

    const CharacterMetadata* find_character(
        std::string_view id) const noexcept;
    const Persona* find_persona(std::string_view id) const noexcept;
    const ForumInfo* find_forum(std::string_view id) const noexcept;
    std::string_view character_markdown(std::string_view id) const;
    std::string_view forum_markdown(std::string_view id) const;

    // The forum's current default character, re-read from its config.toml so a
    // saved /@ change applies to the next session without a restart. Falls back
    // to the value loaded at startup when the file cannot be used.
    CharacterId forum_default_character(std::string_view forum_id) const;

    // The only path-bearing values the model publishes, needed once by startup
    // to construct SessionRepository.
    std::vector<ForumSessionDirectory> session_directories() const;

private:
    WorkspaceDefinition() = default;

    // Full definitions may carry provider credentials, so they stay off the
    // general API. open_session() is the one production caller that needs them.
    friend OpenedSession open_session(
        const WorkspaceDefinition&,
        const SessionRepository&,
        const SessionIdentity&,
        WakeNotifier&);

    std::vector<CharacterDefinition> copy_definitions_for(
        std::string_view forum_id) const;
    void persist_forum_default_character(
        std::string_view forum_id,
        std::string_view character_id) const;

    WorkspaceConfig config_;
    SharedPersonaRoster personas_;
    std::vector<CharacterMetadata> characters_;
    std::vector<ForumInfo> forums_;
    std::unordered_map<std::string, std::size_t> character_index_;
    std::unordered_map<std::string, std::size_t> forum_index_;
    std::unordered_map<std::string, std::string> character_markdown_;
    std::unordered_map<std::string, std::string> forum_markdown_;
    std::unordered_map<std::string, std::filesystem::path> forum_config_paths_;
    std::unordered_map<std::string, std::vector<CharacterDefinition>> definitions_;
    std::vector<ForumSessionDirectory> session_directories_;
};

} // namespace cha
