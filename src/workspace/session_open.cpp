#include "workspace/session_open.h"

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
    const Persona* const persona = model.find_persona(forum->default_persona_id);
    if (persona == nullptr) {
        throw std::logic_error("Forum default persona is absent from the workspace model");
    }
    std::vector<CharacterDefinition> definitions =
        model.copy_definitions_for(forum->id);
    const CharacterId default_character = model.forum_default_character(forum->id);
    PreparedSession prepared = sessions.prepare(identity);
    log_info("Session opened");
    return {
        .descriptor = {
            .identity = prepared.identity,
            .forum_display_name = forum->display_name,
            .session_label = prepared.label,
            .forum_default_character_id = default_character,
            .forum_default_persona_id = forum->default_persona_id,
            .forum_default_persona_display_name = persona->display_name,
        },
        .controller = SessionController::from_shared_definitions(
            std::move(definitions),
            std::make_shared<const PersonaRoster>(PersonaRoster{*persona}),
            default_character,
            prepared.database_path,
            std::move(prepared.lease),
            notifier,
            std::move(prepared.restore)),
        .persist_default_character = [&model, forum_id = forum->id](
                                         std::string_view character_id) {
            model.persist_forum_default_character(forum_id, character_id);
        },
    };
}

} // namespace cha
