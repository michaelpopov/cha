#pragma once

#include "workspace/workspace_definition.h"
#include "session/opened_session.h"
#include "session/session_identity.h"

#include <memory>

namespace cha {

class SessionRepository;
class WakeNotifier;
class Providers;

// The one production path that creates a SessionController. It re-resolves
// the forum's character definitions from disk and combines them with storage
// the repository prepares now. A definition that no longer loads falls back to
// the generation's copy and reports that on the opened session. The controller
// borrows `model` for the session's whole life, so the caller must keep it
// alive at least that long.
// Entrance and Welcome need no special case: Entrance is an ordinary forum in
// the model and Welcome an ordinary prepared session in the repository.
//
// Domain exceptions propagate unchanged: ForumNotFoundError,
// SessionNotFoundError, SessionBusyError, and storage failures reach the
// caller, which maps them to registry or HTTP results.
OpenedSession open_session(
    const WorkspaceDefinition& model,
    const SessionRepository& sessions,
    const SessionIdentity& identity,
    Providers& providers,
    std::shared_ptr<WakeNotifier> notifier);


} // namespace cha
