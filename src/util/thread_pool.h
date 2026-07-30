#pragma once

#include "util/concurrent_queue.h"

#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace cha {

// A fixed-size, session-scoped executor for blocking work.  It owns worker
// lifetime but deliberately knows nothing about cancellation of the work it
// runs; domain owners must arrange that before stopping the pool.
class ThreadPool {
public:
    using Task = std::function<void()>;

    explicit ThreadPool(std::size_t worker_count);
    ~ThreadPool() noexcept;

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    // Thread-safe.  Returns false once stop() has closed admission.  Task
    // allocation can still throw.
    [[nodiscard]] bool submit(Task task);

    // Closes admission, drains already-accepted tasks, and joins all workers.
    // Thread-safe; concurrent and repeated calls return only after the joining
    // call has completed.
    void stop() noexcept;

    [[nodiscard]] std::size_t worker_count() const noexcept;

private:
    void worker_loop() noexcept;

    ConcurrentQueue<Task> tasks_;
    std::vector<std::thread> workers_;
    std::mutex stop_mutex_;
    bool stopped_{};
};

} // namespace cha
