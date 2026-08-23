#pragma once

#include "session/opened_session.h"
#include "session/session_identity.h"

#include <memory>

namespace cha {

class SessionRepository;
class WakeNotifier;
class Providers;

// The one production path that creates a SessionController. It acquires the
// current Workspace, validates the forum, and combines its stable IDs with
// storage the repository prepares now. The controller later acquires Workspace
// for configuration-dependent operations; it retains no workspace lifetime.
// Entrance and Welcome need no special case: Entrance is an ordinary forum in
// the model and Welcome an ordinary prepared session in the repository.
//
// Domain exceptions propagate unchanged: ForumNotFoundError,
// SessionNotFoundError, SessionBusyError, and storage failures reach the
// caller, which maps them to registry or HTTP results.
OpenedSession open_session(
    const SessionRepository& sessions,
    const SessionIdentity& identity,
    Providers& providers,
    std::shared_ptr<WakeNotifier> notifier);
} // namespace cha
