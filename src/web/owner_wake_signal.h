#pragma once

#include "util/wake_notifier.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace cha::web {

// A portable, coalescing wake source consumed by one session owner loop.
class OwnerWakeSignal final : public cha::WakeNotifier {
public:
    void wake() noexcept override;

    // Waits until a wake not yet observed by this caller, or the deadline.
    // Returns true when work may be available.
    [[nodiscard]] bool wait_until(std::chrono::steady_clock::time_point deadline);

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::uint64_t generation_{};
    std::uint64_t observed_{};
};

} // namespace cha::web
