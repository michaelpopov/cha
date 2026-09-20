#pragma once

#include "web/protocol.h"
#include "workspace/workspace.h"

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cha {
class WorkspaceConfigStore;
class SessionRepository;
class LiveSessionManager;
struct LiveSessionManagerSnapshot;
}

namespace cha::app::workspace {

[[nodiscard]] bool is_welcome_session(
    std::string_view forum_id,
    std::string_view session_id) noexcept;

[[nodiscard]] CharacterSummary character_summary(
    const Workspace& workspace,
    const WorkspaceCharacter& character);
[[nodiscard]] CharacterDetail character_detail(
    const Workspace& workspace,
    const WorkspaceCharacter& character);
[[nodiscard]] PersonaSummary persona_summary(
    const Workspace& workspace,
    const WorkspacePersona& persona);
[[nodiscard]] PersonaDetail persona_detail(
    const Workspace& workspace,
    const WorkspacePersona& persona);
[[nodiscard]] ForumSummary forum_summary(
    const WorkspaceForum& forum,
    const Workspace& workspace);
[[nodiscard]] ForumDetail forum_detail(
    const Workspace& workspace,
    const WorkspaceForum& forum);
[[nodiscard]] MarkdownFile markdown_file(
    std::string filename,
    std::string content,
    bool writable);
[[nodiscard]] std::vector<SessionListing> sessions_for(
    const SessionRepository& sessions,
    const LiveSessionManagerSnapshot& snapshot,
    std::string_view forum_id);

void invalidate_affected_sessions(
    LiveSessionManager& live_sessions,
    std::span<const std::string> forum_ids);
void refresh_affected_sessions(
    LiveSessionManager& live_sessions,
    std::span<const std::string> forum_ids);

[[nodiscard]] CharacterDetail get_character(
    const Workspace& workspace,
    std::string_view id);
[[nodiscard]] CharacterDetail create_character(
    WorkspaceConfigStore& store,
    std::string_view display_name,
    std::string_view description);
[[nodiscard]] CharacterDetail update_character_settings(
    WorkspaceConfigStore& store,
    LiveSessionManager& live_sessions,
    std::string_view id,
    const CharacterSettingsUpdate& update);
[[nodiscard]] CharacterDetail update_character_definition(
    WorkspaceConfigStore& store,
    LiveSessionManager& live_sessions,
    std::string_view id,
    const CharacterDefinitionUpdate& update);
void delete_character(WorkspaceConfigStore& store, std::string_view id);
[[nodiscard]] MarkdownFile get_character_file(
    const Workspace& workspace,
    std::string_view id,
    std::string_view filename);
[[nodiscard]] MarkdownFile create_character_file(
    WorkspaceConfigStore& store,
    LiveSessionManager& live_sessions,
    std::string_view id,
    std::string filename,
    std::string content);
[[nodiscard]] MarkdownFile update_character_file(
    WorkspaceConfigStore& store,
    LiveSessionManager& live_sessions,
    std::string_view id,
    std::string filename,
    std::string content);
void delete_character_file(
    WorkspaceConfigStore& store,
    LiveSessionManager& live_sessions,
    std::string_view id,
    std::string filename);

[[nodiscard]] PersonaDetail get_persona(
    const Workspace& workspace,
    std::string_view id);
[[nodiscard]] PersonaDetail create_persona(
    WorkspaceConfigStore& store,
    std::string_view display_name);
[[nodiscard]] PersonaDetail update_persona(
    WorkspaceConfigStore& store,
    LiveSessionManager& live_sessions,
    std::string_view id,
    const PersonaUpdate& update);
void delete_persona(WorkspaceConfigStore& store, std::string_view id);

[[nodiscard]] ForumDetail get_forum(
    const Workspace& workspace,
    std::string_view id);
[[nodiscard]] ForumDetail create_forum(
    WorkspaceConfigStore& store,
    std::string_view display_name,
    std::string_view persona_id);
[[nodiscard]] ForumDetail update_forum(
    WorkspaceConfigStore& store,
    LiveSessionManager& live_sessions,
    std::string_view id,
    const ForumUpdate& update);
void delete_forum(
    WorkspaceConfigStore& store,
    LiveSessionManager& live_sessions,
    std::string_view id);
[[nodiscard]] ForumDetail update_forum_members(
    WorkspaceConfigStore& store,
    LiveSessionManager& live_sessions,
    std::string_view id,
    const ForumMembersUpdate& update);
[[nodiscard]] MarkdownFile get_forum_file(
    const Workspace& workspace,
    std::string_view id,
    std::string_view filename);
[[nodiscard]] MarkdownFile create_forum_file(
    WorkspaceConfigStore& store,
    LiveSessionManager& live_sessions,
    std::string_view id,
    std::string filename,
    std::string content);
[[nodiscard]] MarkdownFile update_forum_file(
    WorkspaceConfigStore& store,
    LiveSessionManager& live_sessions,
    std::string_view id,
    std::string filename,
    std::string content);
void delete_forum_file(
    WorkspaceConfigStore& store,
    LiveSessionManager& live_sessions,
    std::string_view id,
    std::string filename);

} // namespace cha::app::workspace
