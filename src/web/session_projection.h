#pragma once

#include "session/controller_view.h"
#include "chat/session_identity.h"
#include "web/protocol.h"

#include <optional>
#include <string>
#include <string_view>

namespace cha::web {

// Web-owned presentation data combined with core state only at the protocol
// boundary. It deliberately excludes controller, transcript, and identity
// state so the mapper remains a pure operation.
struct WebPresentationState {
    std::optional<std::string> notice;
    SessionLifecycle lifecycle{SessionLifecycle::starting};
    std::optional<ShutdownReason> shutdown_reason;
};

// Copies a borrowed controller view into an owning protocol snapshot, adding
// web-only identity, lifecycle, and notice fields. Borrowing ends here: the
// returned value stays valid after the controller mutates or is destroyed, so
// callers must invoke this synchronously on the controller's owner thread.
[[nodiscard]] SessionSnapshot to_snapshot(
    const FullSessionId& identity,
    std::string_view label,
    const ControllerView& controller,
    const WebPresentationState& presentation);

} // namespace cha::web
