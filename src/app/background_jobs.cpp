#include "app/background_jobs.h"

#include <algorithm>
#include <utility>

namespace cha::app {

bool BackgroundJobs::launch(std::function<void(std::atomic_bool&)> work) {
    auto control = std::make_shared<BackgroundControl>();
    std::lock_guard lock(mutex_);
    if (closed_) return false;
    reap_finished_locked();
    jobs_.push_back({std::thread{}, control});
    try {
        jobs_.back().worker = std::thread(
            [work = std::move(work), control] {
                work(control->cancel);
                {
                    std::lock_guard lock(control->mutex);
                    control->finished.store(true);
                }
                control->changed.notify_all();
            });
    } catch (...) {
        jobs_.pop_back();
        throw;
    }
    return true;
}

void BackgroundJobs::join() {
    for (;;) {
        std::vector<BackgroundJob> jobs;
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
            jobs.swap(jobs_);
        }
        if (jobs.empty()) return;
        for (auto& job : jobs) {
            job.control->cancel.store(true);
            if (job.worker.joinable()) job.worker.join();
        }
    }
}

bool BackgroundJobs::join_until(
    std::chrono::steady_clock::time_point deadline) {
    std::vector<std::shared_ptr<BackgroundControl>> controls;
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        controls.reserve(jobs_.size());
        for (const BackgroundJob& job : jobs_) {
            job.control->cancel.store(true);
            controls.push_back(job.control);
        }
    }
    for (const auto& control : controls) {
        std::unique_lock lock(control->mutex);
        if (!control->changed.wait_until(lock, deadline, [&] {
                return control->finished.load();
            })) {
            return false;
        }
    }

    std::vector<BackgroundJob> jobs;
    {
        std::lock_guard lock(mutex_);
        jobs.swap(jobs_);
    }
    // finished means all access to Application-owned state is complete.
    // Detaching here keeps the deadline strict while the thread tears down
    // its own harmless captures.
    for (BackgroundJob& job : jobs) {
        if (job.worker.joinable()) job.worker.detach();
    }
    return true;
}

void BackgroundJobs::reap_finished_locked() {
    const auto first_live = std::remove_if(
        jobs_.begin(),
        jobs_.end(),
        [](BackgroundJob& job) {
            if (!job.control->finished.load()) return false;
            if (job.worker.joinable()) job.worker.detach();
            return true;
        });
    jobs_.erase(first_live, jobs_.end());
}

} // namespace cha::app
