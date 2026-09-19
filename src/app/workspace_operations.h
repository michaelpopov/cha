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
}

namespace cha::web {
class LiveSessionManager;
struct LiveSessionManagerSnapshot;
}

namespace cha::app::workspace {

[[nodiscard]] std::shared_ptr<const Workspace> published_workspace();
[[nodiscard]] bool is_welcome_session(
    std::string_view forum_id,
    std::string_view session_id) noexcept;

[[nodiscard]] cha::web::CharacterSummary character_summary(
    const Workspace& workspace,
    const WorkspaceCharacter& character);
[[nodiscard]] cha::web::CharacterDetail character_detail(
    const Workspace& workspace,
    const WorkspaceCharacter& character);
[[nodiscard]] cha::web::PersonaSummary persona_summary(
    const Workspace& workspace,
    const WorkspacePersona& persona);
[[nodiscard]] cha::web::PersonaDetail persona_detail(
    const Workspace& workspace,
    const WorkspacePersona& persona);
[[nodiscard]] cha::web::ForumSummary forum_summary(
    const WorkspaceForum& forum,
    const Workspace& workspace);
[[nodiscard]] cha::web::ForumDetail forum_detail(
    const Workspace& workspace,
    const WorkspaceForum& forum);
[[nodiscard]] cha::web::MarkdownFile markdown_file(
    std::string filename,
    std::string content,
    bool writable);
[[nodiscard]] std::vector<cha::web::SessionListing> sessions_for(
    const SessionRepository& sessions,
    const cha::web::LiveSessionManagerSnapshot& snapshot,
    std::string_view forum_id);

void invalidate_affected_sessions(
    cha::web::LiveSessionManager& live_sessions,
    std::span<const std::string> forum_ids);

[[nodiscard]] cha::web::CharacterDetail get_character(std::string_view id);
[[nodiscard]] cha::web::CharacterDetail create_character(
    WorkspaceConfigStore& store,
    std::string_view display_name,
    std::string_view description);
[[nodiscard]] cha::web::CharacterDetail update_character_settings(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    std::string_view id,
    const cha::web::CharacterSettingsUpdate& update);
[[nodiscard]] cha::web::CharacterDetail update_character_definition(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    std::string_view id,
    const cha::web::CharacterDefinitionUpdate& update);
void delete_character(WorkspaceConfigStore& store, std::string_view id);
[[nodiscard]] cha::web::MarkdownFile get_character_file(
    std::string_view id,
    std::string_view filename);
[[nodiscard]] cha::web::MarkdownFile create_character_file(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    std::string_view id,
    std::string filename,
    std::string content);
[[nodiscard]] cha::web::MarkdownFile update_character_file(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    std::string_view id,
    std::string filename,
    std::string content);
void delete_character_file(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    std::string_view id,
    std::string filename);

[[nodiscard]] cha::web::PersonaDetail get_persona(std::string_view id);
[[nodiscard]] cha::web::PersonaDetail create_persona(
    WorkspaceConfigStore& store,
    std::string_view display_name);
[[nodiscard]] cha::web::PersonaDetail update_persona(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    std::string_view id,
    const cha::web::PersonaUpdate& update);
void delete_persona(WorkspaceConfigStore& store, std::string_view id);

[[nodiscard]] cha::web::ForumDetail get_forum(std::string_view id);
[[nodiscard]] cha::web::ForumDetail create_forum(
    WorkspaceConfigStore& store,
    std::string_view display_name,
    std::string_view persona_id);
[[nodiscard]] cha::web::ForumDetail update_forum(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    std::string_view id,
    const cha::web::ForumUpdate& update);
void delete_forum(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    std::string_view id);
[[nodiscard]] cha::web::ForumDetail update_forum_members(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    std::string_view id,
    const cha::web::ForumMembersUpdate& update);
[[nodiscard]] cha::web::MarkdownFile get_forum_file(
    std::string_view id,
    std::string_view filename);
[[nodiscard]] cha::web::MarkdownFile create_forum_file(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    std::string_view id,
    std::string filename,
    std::string content);
[[nodiscard]] cha::web::MarkdownFile update_forum_file(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    std::string_view id,
    std::string filename,
    std::string content);
void delete_forum_file(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    std::string_view id,
    std::string filename);

} // namespace cha::app::workspace
