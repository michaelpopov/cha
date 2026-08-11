#include "web/session_projection.h"

#include "util/text.h"

#include <algorithm>

namespace cha::web {

SessionSnapshot to_snapshot(
    const SessionDescriptor& descriptor,
    const ControllerView& controller,
    const WebPresentationState& presentation) {
    SessionSnapshot snapshot{
        .forum = {
            .id = descriptor.identity.forum_id,
            .display_name = descriptor.forum_display_name,
            .default_character_id = descriptor.forum_default_character_id,
            .default_persona_id = descriptor.forum_default_persona_id,
            .default_persona_display_name = descriptor.forum_default_persona_display_name,
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
    snapshot.characters.reserve(controller.characters.size());
    for (const CharacterMetadata& character : controller.characters) {
        snapshot.characters.push_back({
            .id = character.id,
            .display_name = character.display_name,
            .description = character.description,
            .appearance = character.appearance,
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
