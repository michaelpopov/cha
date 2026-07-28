#pragma once

#include "agents/agent.h"
#include "agents/completion_backend.h"
#include "util/concurrent_queue.h"
#include "util/wake_notifier.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace cha {

using BatchId = std::uint64_t;

enum class CleanupStatus {
    none,
    pending,
    complete,
};

// Owns every configured backend and coordinates one reusable regular runner
// plus temporary runners for concurrent multicast batches. Runners prepare
// exclusively from owned CompletionInput values and retain separate delta
// queues plus terminal slots; only the selected foreground event channel is
// visible to the controller. Exactly one batch may be live, and its fixed run
// positions match the input order for the batch's full lifetime.
class AgentRegistry {
public:
    // Optional fault-injection seam invoked immediately before each cleanup or
    // temporary staging thread is constructed.
    using StageThreadHook = std::function<void()>;

    AgentRegistry(
        std::vector<AgentDefinition> definitions,
        WakeNotifier& notifier);
    AgentRegistry(
        std::vector<std::unique_ptr<CompletionBackend>> backends,
        WakeNotifier& notifier,
        StageThreadHook before_stage_thread_start = {});
    ~AgentRegistry() noexcept;

    AgentRegistry(const AgentRegistry&) = delete;
    AgentRegistry& operator=(const AgentRegistry&) = delete;

    const std::vector<AgentRuntimeInfo>& runtime_info() const noexcept;

    // Strong guarantee: on success, every input and backend lease belongs to a
    // runner parked at the returned batch's unopened start gate. Run positions
    // are the corresponding input indices. On failure, no backend is called
    // and no lease or runner remains live.
    BatchId stage_batch(std::vector<CompletionInput> inputs);
    void set_foreground(BatchId batch, std::size_t run_index);
    void open_batch_gate(BatchId batch) noexcept;
    void retire(BatchId batch, std::size_t run_index);
    void retire_batch(BatchId batch) noexcept;

    void cancel_all() noexcept;

    // Exceptional, synchronous teardown for a batch whose foreground
    // activation failed. Interactive /stop uses the non-blocking cleanup API.
    void discard_batch(BatchId batch) noexcept;
    void begin_abort_cleanup(
        BatchId batch,
        std::optional<std::size_t> retained_foreground) noexcept;
    void release_foreground_to_cleanup(
        BatchId batch,
        std::size_t run_index) noexcept;
    CleanupStatus poll_abort_cleanup(BatchId batch) noexcept;

    [[nodiscard]] ChannelReadStatus try_receive(AgentEvent& event);

    // Cancels and joins every worker. The foreground event channel remains
    // drainable; once drained, try_receive() reports closed.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cha
