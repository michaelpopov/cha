#pragma once

#include "agents/agent.h"
#include "agents/completion_backend.h"
#include "util/concurrent_queue.h"
#include "util/wake_notifier.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace cha {

using BatchId = std::uint64_t;
using RunId = std::uint64_t;

struct StagedBatch {
    BatchId batch_id{};
    std::vector<RunId> run_ids;
};

enum class CleanupStatus {
    none,
    pending,
    complete,
};

// Owns every configured backend and coordinates one reusable regular runner
// plus temporary runners for concurrent multicast batches. Runners prepare
// exclusively from owned CompletionInput values and retain separate event
// queues; only the selected foreground queue is visible to the controller.
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
    // runner parked at the returned batch's unopened start gate. On failure,
    // no backend is called and no lease or runner remains live.
    StagedBatch stage_batch(std::vector<CompletionInput> inputs);
    void set_foreground(RunId run);
    void open_batch_gate(BatchId batch) noexcept;
    void retire(RunId run);
    void retire_batch(BatchId batch) noexcept;

    void cancel_all() noexcept;

    // Exceptional, synchronous teardown for a batch whose foreground
    // activation failed. Interactive /stop uses the non-blocking cleanup API.
    void discard_batch(BatchId batch) noexcept;
    void begin_abort_cleanup(
        BatchId batch,
        std::optional<RunId> retained_foreground) noexcept;
    void release_foreground_to_cleanup(RunId run) noexcept;
    CleanupStatus poll_abort_cleanup(BatchId batch) noexcept;

    [[nodiscard]] ChannelReadStatus try_receive(AgentEvent& event);

    // Cancels and joins every worker. The foreground queue remains drainable;
    // once drained, try_receive() reports closed.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cha
