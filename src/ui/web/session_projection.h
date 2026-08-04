#pragma once

#include "session/session_identity.h"
#include "session/session_state.h"
#include "ui/web/protocol.h"

#include <optional>
#include <string>

namespace cha::web {

// Web-owned presentation data combined with core state only at the protocol
// boundary. It deliberately excludes controller, transcript, and identity
// state so the mapper remains a pure operation.
struct WebPresentationState {
    std::optional<std::string> notice;
    SessionLifecycle lifecycle{SessionLifecycle::starting};
    std::optional<ShutdownReason> shutdown_reason;
};

// Converts one consumed core state value into the stable web DTO. Moving the
// strings out of state preserves the one-full-transcript-copy publication path.
[[nodiscard]] SessionSnapshot to_snapshot(
    const SessionDescriptor& descriptor,
    SessionState&& state,
    const WebPresentationState& presentation);

} // namespace cha::web
