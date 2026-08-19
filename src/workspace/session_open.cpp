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
    // Both branches of forum_default_persona() return a persona the workspace
    // resolved, and the controller rejects one that is not in the roster.
    const std::string default_persona = model.forum_default_persona(forum->id);
    auto copied = model.copy_definitions_for(forum->id);
    const CharacterId default_character = model.forum_default_character(forum->id);
    PreparedSession prepared = sessions.prepare(identity);
    log_info("Session opened");
    return {
        .descriptor = {
            .identity = prepared.identity,
            .forum_display_name = forum->display_name,
            .session_label = prepared.label,
            .forum_default_character_id = default_character,
        },
        .controller = SessionController::from_shared_definitions(
            std::move(copied.definitions),
            model.personas(),
            default_character,
            default_persona,
            prepared.database_path,
            std::move(prepared.lease),
            notifier,
            std::move(prepared.restore),
            // Borrowed for the session's life, the same borrow the persist
            // callbacks below take. The caller must keep the model alive for at
            // least that long: a caller that can replace it publishes each one
            // as a generation and retains it in OpenedSession::lifetime.
            [&model](std::string_view name) {
                return model.resolve_session_provider(name);
            },
            // Same borrow as the provider resolver above.
            [&model](std::string_view name) {
                return model.resolve_session_style(name);
            },
            prepared.identity),
        .persist_default_character = [&model, forum_id = forum->id](
                                         std::string_view character_id) {
            model.persist_forum_default_character(forum_id, character_id);
        },
        .persist_default_persona = [&model, forum_id = forum->id](
                                       std::string_view persona_id) {
            model.persist_forum_default_persona(forum_id, persona_id);
        },
        .notice = std::move(copied.fallback_notice),
    };
}

} // namespace cha
