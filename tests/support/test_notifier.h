#pragma once

#include "util/wake_notifier.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace cha::test {

class TestNotifier final : public WakeNotifier {
public:
    void wake() noexcept override {
        {
            std::lock_guard lock(mutex_);
            ++wake_count_;
        }
        ready_.notify_all();
    }

    std::size_t wake_count() const {
        std::lock_guard lock(mutex_);
        return wake_count_;
    }

    bool wait_for_wake(
        std::size_t observed,
        std::chrono::milliseconds timeout = std::chrono::seconds(1)) {
        std::unique_lock lock(mutex_);
        return ready_.wait_for(lock, timeout, [this, observed] {
            return wake_count_ > observed;
        });
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::size_t wake_count_{};
};

class NoopNotifier final : public WakeNotifier {
public:
    void wake() noexcept override {
    }
};

} // namespace cha::test
