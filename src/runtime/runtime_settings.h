#pragma once

#include <chrono>
#include <atomic>
#include <cstdint>
#include <memory>
#include <cstddef>
#include <functional>

namespace cha {

struct FolderStoreHttpRequest;
struct FolderStoreHttpResponse;

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
    std::chrono::milliseconds folder_store_poll_interval{5000};

    // Test-only seams. Production leaves every hook empty; the note-audio,
    // compression, and folder-store workers call a hook only when it is set.
    struct TestHooks {
        // Wait injection for deterministic retry tests.
        std::function<void(int)> note_audio_wait;
        std::function<void(int)> note_compression_wait;
        // Launch failure and worker-barrier hooks for lifecycle tests.
        std::function<void(std::int64_t)> note_audio_before_launch;
        std::function<void(std::int64_t, std::shared_ptr<std::atomic_bool>)> note_audio_worker_started;
        std::function<void(int)> folder_store_wait;
        // Barrier after reading the poll predicate, while its wait mutex is held.
        std::function<void()> folder_store_poll_predicate_read;
        std::function<void()> folder_store_before_launch;
        std::function<void(std::int64_t, std::shared_ptr<std::atomic_bool>)> folder_store_worker_started;
        std::function<void()> folder_store_before_lifetime_lock;
        std::function<void(std::int64_t)> folder_store_worker_finished;
        std::function<FolderStoreHttpResponse(const FolderStoreHttpRequest&, const std::atomic_bool&)> folder_store_transport;
    };
    TestHooks test_hooks;
};

} // namespace cha
