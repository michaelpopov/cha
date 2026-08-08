#pragma once

#include "agents/model_backend.h"
#include "util/concurrent_queue.h"
#include "util/thread_pool.h"
#include "util/wake_notifier.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace cha {

class GenerationExecutor;

// One in-flight generation operation. It owns the ordered execution slots
// selected for that operation, their shared start gate, the foreground
// position, cancellation state, and the wait state that proves no execution can
// still reach a backend. Each slot owns the one GenerationRequest its backend
// uses, so the foreground run and the foreground event queue are always the
// same slot.
//
// Only the session owner thread selects, receives, advances, cancels, waits, or
// destroys. Pool workers touch only their own execution, the shared gate, their
// backend, their cancellation flag, their event queue, and the notifier.
class GenerationBatch final {
public:
    GenerationBatch(GenerationBatch&&) noexcept;
    GenerationBatch& operator=(GenerationBatch&&) = delete;
    GenerationBatch(const GenerationBatch&) = delete;
    GenerationBatch& operator=(const GenerationBatch&) = delete;
    // Safety fallback: cancels and waits for every submitted execution.
    // Production destruction happens on the session owner thread, never on one
    // of this batch's own pool workers.
    ~GenerationBatch() noexcept;

    // The run of the currently selected slot. try_receive_foreground() reads
    // that same slot's queue, so no caller passes an index between objects.
    [[nodiscard]] const RunSpec& foreground_run() const;
    // Owner-thread position, exposed only for the controller's activation
    // fault-injection hook. It must never index another collection.
    [[nodiscard]] std::size_t foreground_index() const noexcept;
    [[nodiscard]] bool has_next_foreground() const noexcept;

    // Releases every execution from the shared gate. The first gate transition
    // wins, so opening an already cancelled gate does nothing.
    void open() noexcept;
    [[nodiscard]] ChannelReadStatus try_receive_foreground(GenerationEvent& event);
    // Valid only once the foreground slot's terminal event has been delivered
    // and only while a next slot exists.
    void advance_foreground();

    // Idempotent: marks the batch cancelled, cancels every execution, and
    // cancels the gate if it has not already opened.
    void cancel() noexcept;
    [[nodiscard]] bool cancellation_requested() const noexcept;
    [[nodiscard]] bool executions_finished() const noexcept;
    // Waits until no execution can access a backend. Event queues stay alive
    // and drainable, so shutdown can cancel, wait, then drain the foreground
    // terminal event.
    void wait_until_finished() noexcept;

private:
    friend class GenerationExecutor;

    struct Impl;

    // Failure-atomic staging: every slot is constructed and submitted behind
    // one closed gate, or no backend is called and no task remains live.
    // `backends` is parallel to `inputs` and is resolved by the executor.
    [[nodiscard]] static GenerationBatch stage(
        std::vector<GenerationRequest> inputs,
        const std::vector<ModelBackend*>& backends,
        WakeNotifier& notifier,
        ThreadPool& worker_pool,
        const std::function<void(std::size_t)>& before_submit);

    explicit GenerationBatch(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace cha
