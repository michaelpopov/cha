#pragma once

#include "chat/session_identity.h"
#include "session/opened_session.h"

#include <cstdint>
#include <memory>
#include <string>

namespace cha {

class SessionRepository;
class WakeNotifier;
class Providers;
class WorkspaceConfigStore;

// The one production path that creates a SessionController. It acquires the
// current Workspace, validates the forum, and combines its stable IDs with
// storage the repository prepares now. The controller later acquires Workspace
// for configuration-dependent operations; it retains no workspace lifetime.
// Entrance and Welcome need no special case: Entrance is an ordinary forum in
// the model and Welcome an ordinary prepared session in the repository.
//
// Domain exceptions propagate unchanged: ForumNotFoundError,
// SessionNotFoundError, and storage failures reach the caller, which maps them
// to registry or HTTP results.
OpenedSession open_session(
    const SessionRepository& sessions,
    const FullSessionId& identity,
    Providers& providers,
    std::shared_ptr<WakeNotifier> notifier,
    WorkspaceConfigStore& config,
    std::uint64_t context_epoch = 0,
    std::shared_ptr<const std::string> maintenance_prompt = {});
} // namespace cha
