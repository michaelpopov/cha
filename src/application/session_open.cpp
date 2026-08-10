#include "application/session_open.h"

#include "session/not_found_error.h"
#include "session/session_controller.h"
#include "session/session_repository.h"
#include "util/logging.h"

#include <utility>

namespace cha {

OpenedSession open_session(
    const WorkspaceDefinition& model,
    const SessionRepository& sessions,
    const SessionIdentity& identity,
    WakeNotifier& notifier) {
    const ForumInfo* const forum = model.find_forum(identity.forum_id);
    if (forum == nullptr) {
        throw ForumNotFoundError("Forum '" + identity.forum_id + "' does not exist");
    }
    std::vector<CharacterDefinition> definitions =
        model.copy_definitions_for(forum->id);
    PreparedSession prepared = sessions.prepare(identity);
    log_info("Session opened");
    return {
        .descriptor = {
            .identity = prepared.identity,
            .forum_display_name = forum->display_name,
            .session_label = prepared.label,
            .forum_default_character_id = forum->default_character_id,
        },
        .controller = SessionController::from_shared_definitions(
            std::move(definitions),
            model.personas(),
            forum->default_character_id,
            prepared.database_path,
            std::move(prepared.lease),
            notifier,
            std::move(prepared.restore)),
    };
}

} // namespace cha
