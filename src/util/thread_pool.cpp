#include "util/thread_pool.h"

#include <stdexcept>
#include <optional>
#include <utility>

namespace cha {

ThreadPool::ThreadPool(std::size_t worker_count) {
    if (worker_count == 0) {
        throw std::invalid_argument("Thread pool requires at least one worker");
    }
    workers_.reserve(worker_count);
    try {
        for (std::size_t index = 0; index < worker_count; ++index) {
            workers_.emplace_back(&ThreadPool::worker_loop, this);
        }
    } catch (...) {
        tasks_.close();
        for (std::thread& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        throw;
    }
}

ThreadPool::~ThreadPool() noexcept {
    stop();
}

bool ThreadPool::submit(Task task) {
    if (!task) {
        throw std::invalid_argument("Thread pool task must not be empty");
    }
    return tasks_.push(std::move(task));
}

void ThreadPool::stop() noexcept {
    std::lock_guard lock(stop_mutex_);
    if (stopped_) {
        return;
    }
    stopped_ = true;
    tasks_.close();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            try {
                worker.join();
            } catch (...) {
                std::terminate();
            }
        }
    }
}

std::size_t ThreadPool::worker_count() const noexcept {
    return workers_.size();
}

void ThreadPool::worker_loop() noexcept {
    while (std::optional<Task> task = tasks_.get()) {
        try {
            (*task)();
        } catch (...) {
            std::terminate();
        }
    }
}

} // namespace cha
