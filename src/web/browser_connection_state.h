#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace cha::web {

// Owner-thread-only bookkeeping for the supported one-page browser contract.
class BrowserConnectionState {
public:
    using Clock = std::chrono::steady_clock;

    void published(Clock::time_point now);
    [[nodiscard]] std::optional<std::uint64_t> accept();
    bool close(std::uint64_t connection_id, Clock::time_point now);
    [[nodiscard]] std::optional<Clock::time_point> deadline(
        bool is_generating,
        std::chrono::milliseconds idle_grace,
        std::chrono::milliseconds orphan_limit) const;

private:
    std::uint64_t next_connection_id_{1};
    std::optional<std::uint64_t> active_connection_id_;
    std::optional<Clock::time_point> disconnected_since_;
};

} // namespace cha::web
