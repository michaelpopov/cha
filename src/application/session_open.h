#pragma once

#include "application/workspace_definition.h"
#include "session/opened_session.h"
#include "session/session_identity.h"

namespace cha {

class SessionRepository;
class WakeNotifier;

// The one production path that creates a SessionController. It combines the
// definitions the model loaded at startup with storage the repository prepares
// now, so a session always opens with the values discovery already showed.
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
    WakeNotifier& notifier);

} // namespace cha
