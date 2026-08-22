#include "web/browser_connection_state.h"

namespace cha::web {

void BrowserConnectionState::published(Clock::time_point now) {
    active_connection_id_.reset();
    disconnected_since_ = now;
}

BrowserConnectionState::Accepted BrowserConnectionState::accept() {
    // Taking over never consults a deadline: a reader who opens the session on
    // a second device must not wait for the first one's socket to be noticed.
    const Accepted accepted{next_connection_id_++, active_connection_id_};
    active_connection_id_ = accepted.connection_id;
    disconnected_since_.reset();
    return accepted;
}

bool BrowserConnectionState::close(
    std::uint64_t connection_id,
    Clock::time_point now) {
    // A displaced device tears its stream down late; that close names an old
    // connection and must not start a deadline against the current one.
    if (active_connection_id_ != connection_id) return false;
    active_connection_id_.reset();
    disconnected_since_ = now;
    return true;
}

std::optional<BrowserConnectionState::Clock::time_point>
BrowserConnectionState::deadline(
    bool is_generating,
    std::chrono::milliseconds idle_grace,
    std::chrono::milliseconds orphan_limit) const {
    if (active_connection_id_ || !disconnected_since_) return std::nullopt;
    return *disconnected_since_ + (is_generating ? orphan_limit : idle_grace);
}

} // namespace cha::web
