#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace cha::app {

// Owns asynchronous workers, not their dependencies. The application must join
// before destroying those dependencies, or retain both after a shutdown timeout.
// Callers must complete join() or successfully join_until() before destruction.
class BackgroundJobs {
public:
    bool launch(std::function<void(std::atomic_bool&)> work);
    void join();
    bool join_until(std::chrono::steady_clock::time_point deadline);

private:
    void reap_finished_locked();
    struct BackgroundControl {
        std::atomic_bool cancel{};
        std::atomic_bool finished{};
        std::mutex mutex;
        std::condition_variable changed;
    };
    struct BackgroundJob {
        std::thread worker;
        std::shared_ptr<BackgroundControl> control;
    };
    std::mutex mutex_;
    std::vector<BackgroundJob> jobs_;
    bool closed_{};
};

} // namespace cha::app
