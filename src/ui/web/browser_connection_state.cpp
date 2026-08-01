#include "ui/web/browser_connection_state.h"

namespace cha::web {

void BrowserConnectionState::published(Clock::time_point now) {
    active_connection_id_.reset();
    disconnected_since_ = now;
}

std::optional<std::uint64_t> BrowserConnectionState::accept() {
    if (active_connection_id_) return std::nullopt;
    const std::uint64_t id = next_connection_id_++;
    active_connection_id_ = id;
    disconnected_since_.reset();
    return id;
}

bool BrowserConnectionState::close(
    std::uint64_t connection_id,
    Clock::time_point now) {
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
