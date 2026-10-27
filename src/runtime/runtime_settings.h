#pragma once

#include <chrono>
#include <atomic>
#include <cstdint>
#include <memory>
#include <cstddef>
#include <functional>

namespace cha {

// Public fields deliberately make every deadline and bound injectable by tests.
struct RuntimeSettings {
    std::size_t session_limit{8};
    std::size_t command_queue_capacity{64};
    std::size_t command_batch_size{8};
    std::size_t event_batch_size{64};
    std::chrono::milliseconds open_deadline{10000};
    std::chrono::milliseconds command_deadline{30000};
    std::chrono::milliseconds delete_deadline{10000};
    std::size_t prompt_limit{32768};
    std::chrono::milliseconds shutdown_grace{10000};
    std::size_t pending_append_byte_limit{65536};
    // Optional wait injection for deterministic note-audio retry tests.
    std::function<void(int)> note_audio_wait;
    // Optional launch failure and worker-barrier hooks for lifecycle tests.
    std::function<void(std::int64_t)> note_audio_before_launch;
    std::function<void(std::int64_t, std::shared_ptr<std::atomic_bool>)> note_audio_worker_started;
};

} // namespace cha
