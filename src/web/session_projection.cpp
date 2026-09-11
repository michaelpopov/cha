#include "web/session_projection.h"

#include "util/text.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <stdexcept>

namespace cha::web {

std::optional<SpeechVoice> resolve_speech_voice(
    const Workspace& workspace,
    const WorkspaceCharacter& character) {
    if (!character.voice_id) return std::nullopt;
    const WorkspaceVoice* const voice = workspace.find_voice(*character.voice_id);
    if (voice == nullptr) {
        throw std::logic_error("Character voice is absent from the workspace");
    }
    return SpeechVoice{
        .id = voice->id,
        .display_name = voice->label,
        .elevenlabs_voice_id = voice->elevenlabs_voice_id,
        .settings = {
            .stability = voice->settings.stability,
            .similarity_boost = voice->settings.similarity_boost,
            .style = voice->settings.style,
            .use_speaker_boost = voice->settings.use_speaker_boost,
            .speed = voice->settings.speed,
        },
    };
}

SessionSnapshot to_snapshot(
    const FullSessionId& identity,
    std::string_view label,
    const ControllerView& controller,
    const WebPresentationState& presentation) {
    const std::shared_ptr<const Workspace> workspace = getws();
    if (!workspace) throw std::runtime_error("Workspace is not loaded");
    const WorkspaceForum* const workspace_forum =
        workspace->find_forum(identity.forum_id);
    const WorkspacePersona* const workspace_persona =
        workspace->find_persona(controller.default_persona_id);
    if (workspace_forum == nullptr || workspace_persona == nullptr) {
        throw std::runtime_error(
            "Session configuration is absent from the current workspace");
    }
    SessionSnapshot snapshot{
        .forum = {
            .id = identity.forum_id,
            .display_name = workspace_forum->display_name,
            .default_character_id = workspace_forum->default_character_id,
            .default_persona_id = std::string(controller.default_persona_id),
            .default_persona_display_name = workspace_persona->display_name,
        },
        .session_id = identity.session_id,
        .session_label = std::string(label),
        .default_character_id = std::string(controller.default_character_id),
        .transcript = {
            controller.transcript.entries.begin(),
            controller.transcript.entries.end(),
        },
        .covered_until = controller.transcript.covered_until,
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
    for (TranscriptEntry& entry : snapshot.transcript) {
        if (entry.kind == EntryKind::character) {
            entry.text = remove_source_references(entry.text);
        }
    }
    snapshot.characters.reserve(workspace_forum->members.size());
    for (const WorkspaceForumMember& member : workspace_forum->members) {
        const WorkspaceCharacter* const character =
            workspace->find_character(member.character_id);
        if (character == nullptr) {
            throw std::logic_error("Forum member has no workspace character");
        }
        snapshot.characters.push_back({
            .id = character->character.id,
            .display_name = character->character.display_name,
            .description = character->character.description,
            .appearance = character->character.appearance,
            .voice = resolve_speech_voice(*workspace, *character),
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
