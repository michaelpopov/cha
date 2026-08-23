#include "web/session_projection.h"

#include "util/text.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <stdexcept>

namespace cha::web {

SessionSnapshot to_snapshot(
    const SessionDescriptor& descriptor,
    const ControllerView& controller,
    const WebPresentationState& presentation) {
    const std::shared_ptr<const Workspace> workspace = getws();
    if (!workspace) throw std::runtime_error("Workspace is not loaded");
    const WorkspaceForum* const workspace_forum =
        workspace->find_forum(descriptor.identity.forum_id);
    const WorkspacePersona* const workspace_persona =
        workspace->find_persona(controller.default_persona_id);
    if (workspace_forum == nullptr || workspace_persona == nullptr) {
        throw std::runtime_error(
            "Session configuration is absent from the current workspace");
    }
    SessionSnapshot snapshot{
        .forum = {
            .id = descriptor.identity.forum_id,
            .display_name = descriptor.forum_display_name,
            .default_character_id = descriptor.forum_default_character_id,
            .default_persona_id = std::string(controller.default_persona_id),
            .default_persona_display_name = workspace_persona->display_name,
        },
        .session_id = descriptor.identity.session_id,
        .session_label = descriptor.session_label,
        .default_character_id = std::string(controller.default_character_id),
        .transcript = {
            controller.transcript.entries.begin(),
            controller.transcript.entries.end(),
        },
        .generation = {
            .active = controller.generation.active,
            .request_id = controller.generation.request_id,
            .character_id = std::string(controller.generation.character_id),
            .character_display_name = std::string(controller.generation.character_display_name),
            .phase = controller.generation.phase,
            .reasoning_text = std::string(controller.generation.reasoning_text),
        },
        .notice = presentation.notice,
        .lifecycle = presentation.lifecycle,
        .shutdown_reason = presentation.shutdown_reason,
    };
    snapshot.characters.reserve(workspace_forum->members.size());
    for (const WorkspaceForumMember& member : workspace_forum->members) {
        const WorkspaceCharacter* const character =
            workspace->find_character(member.character_id);
        if (character == nullptr) {
            throw std::logic_error("Forum member has no workspace character");
        }
        CharacterAppearance appearance = character->character.appearance;
        if (controller.style_overrides != nullptr) {
            const auto selected = controller.style_overrides->find(
                character->character.id);
            if (selected != controller.style_overrides->end()) {
                const WorkspaceStyle* const style =
                    workspace->find_style(selected->second);
                if (style != nullptr) {
                    appearance = style->appearance;
                }
            }
        }
        snapshot.characters.push_back({
            .id = character->character.id,
            .display_name = character->character.display_name,
            .description = character->character.description,
            .appearance = appearance,
        });
    }
    snapshot.forum.members = snapshot.characters;
    std::sort(snapshot.forum.members.begin(), snapshot.forum.members.end(),
        [](const CharacterSummary& left, const CharacterSummary& right) {
            return fold_ascii(left.display_name) < fold_ascii(right.display_name);
        });
    return snapshot;
}

} // namespace cha::web
