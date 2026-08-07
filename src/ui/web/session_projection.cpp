#include "ui/web/session_projection.h"

#include "util/text.h"

#include <algorithm>
#include <utility>

namespace cha::web {

SessionSnapshot to_snapshot(
    const SessionDescriptor& descriptor,
    SessionState&& state,
    const WebPresentationState& presentation) {
    SessionSnapshot snapshot{
        .forum = {
            .id = descriptor.identity.forum_id,
            .display_name = descriptor.forum_display_name,
            .default_character_id = descriptor.forum_default_character_id,
        },
        .session_id = descriptor.identity.session_id,
        .session_label = descriptor.session_label,
        .default_character_id = std::move(state.default_agent_id),
        .transcript = std::move(state.transcript),
        .generation = std::move(state.generation),
        .notice = presentation.notice,
        .lifecycle = presentation.lifecycle,
        .shutdown_reason = presentation.shutdown_reason,
    };
    snapshot.characters.reserve(state.characters.size());
    snapshot.forum.members.reserve(state.characters.size());
    for (CharacterInfo& character : state.characters) {
        snapshot.forum.members.push_back({
            .id = character.id,
            .display_name = character.name,
            .description = character.description,
            .appearance = character.appearance,
        });
        snapshot.characters.push_back({
            .id = std::move(character.id),
            .display_name = std::move(character.name),
            .description = std::move(character.description),
            .appearance = character.appearance,
        });
    }
    std::sort(snapshot.forum.members.begin(), snapshot.forum.members.end(),
        [](const CharacterSummary& left, const CharacterSummary& right) {
            return fold_ascii(left.display_name) < fold_ascii(right.display_name);
        });
    return snapshot;
}

} // namespace cha::web
