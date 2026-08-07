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

// Combines one consumed core state value with web-only identity and lifecycle
// fields. Core transcript, generation, and append types remain unchanged at
// the JSON boundary.
[[nodiscard]] SessionSnapshot to_snapshot(
    const SessionDescriptor& descriptor,
    SessionState&& state,
    const WebPresentationState& presentation);

} // namespace cha::web
