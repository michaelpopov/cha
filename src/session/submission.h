#pragma once
#include <atomic>
#include <chrono>

namespace cha {
// Shared cancellation only; the controller never retains a runtime reply.
struct SubmissionState {
    std::atomic_bool cancelled{};
    std::chrono::steady_clock::time_point deadline{
        std::chrono::steady_clock::time_point::max()};
    bool expired() const {
        return cancelled.load() || std::chrono::steady_clock::now() >= deadline;
    }
};
} // namespace cha
