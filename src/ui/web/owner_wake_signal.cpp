#include "ui/web/owner_wake_signal.h"

namespace cha::web {

void OwnerWakeSignal::wake() noexcept {
    {
        std::lock_guard lock(mutex_);
        ++generation_;
    }
    changed_.notify_one();
}

bool OwnerWakeSignal::wait_until(
    std::chrono::steady_clock::time_point deadline) {
    std::unique_lock lock(mutex_);
    if (generation_ != observed_) {
        observed_ = generation_;
        return true;
    }
    if (!changed_.wait_until(
            lock, deadline, [this] { return generation_ != observed_; })) {
        return false;
    }
    observed_ = generation_;
    return true;
}

} // namespace cha::web
