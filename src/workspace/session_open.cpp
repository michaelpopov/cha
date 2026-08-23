#include "workspace/session_open.h"

#include "workspace/workspace.h"
#include "session/not_found_error.h"
#include "session/session_controller.h"
#include "session/session_repository.h"
#include "providers/providers.h"
#include "util/logging.h"

#include <utility>
#include <stdexcept>

namespace cha {
namespace {

std::shared_ptr<const Workspace> current_workspace() {
    std::shared_ptr<const Workspace> workspace = getws();
    if (!workspace) throw std::runtime_error("Workspace is not loaded");
    return workspace;
}

void persist_default_character(
    std::string forum_id,
    std::string_view character_id) {
    const std::shared_ptr<const Workspace> workspace = current_workspace();
    workspace->write_forum_default_character(forum_id, character_id);
    loadws(workspace->root());
}

void persist_default_persona(
    std::string forum_id,
    std::string_view persona_id) {
    const std::shared_ptr<const Workspace> workspace = current_workspace();
    workspace->write_forum_default_persona(forum_id, persona_id);
    loadws(workspace->root());
}

} // namespace

OpenedSession open_session(
    const SessionRepository& sessions,
    const FullSessionId& identity,
    Providers& providers,
    std::shared_ptr<WakeNotifier> notifier) {
    const std::shared_ptr<const Workspace> workspace = current_workspace();
    const WorkspaceForum* const forum = workspace->find_forum(identity.forum_id);
    if (forum == nullptr) {
        throw ForumNotFoundError("Forum '" + identity.forum_id + "' does not exist");
    }
    PreparedSession prepared = sessions.prepare(identity);
    log_info("Session opened");
    return {
        .label = prepared.label,
        .controller = SessionController::from_workspace(
            forum->default_character_id,
            forum->default_persona_id,
            prepared.database_path,
            std::move(prepared.lease),
            providers,
            std::move(notifier),
            std::move(prepared.restore),
            prepared.identity),
        .persist_default_character = [forum_id = forum->id](
                                         std::string_view character_id) {
            persist_default_character(forum_id, character_id);
        },
        .persist_default_persona = [forum_id = forum->id](
                                       std::string_view persona_id) {
            persist_default_persona(forum_id, persona_id);
        },
    };
}

} // namespace cha
