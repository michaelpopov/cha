#pragma once

#include "agents/agent.h"
#include "agents/completion_backend.h"
#include "util/concurrent_queue.h"
#include "util/thread_pool.h"
#include "util/wake_notifier.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

namespace cha {

// Owns every configured backend and submits one gated pool task per execution.
// Exactly one batch may be live, and its fixed run positions match input order
// for the batch's full lifetime.
class AgentRegistry {
public:
    // Test-only fault injection immediately before each pool submission. The
    // argument is the zero-based submission index and exceptions abort staging.
    using BeforeSubmitHook = std::function<void(std::size_t)>;

    // The caller owns the pool and must join it while this registry and its
    // notifier remain alive.
    AgentRegistry(
        std::vector<AgentDefinition> definitions,
        WakeNotifier& notifier,
        ThreadPool& worker_pool);
    AgentRegistry(
        std::vector<std::unique_ptr<CompletionBackend>> backends,
        WakeNotifier& notifier,
        ThreadPool& worker_pool,
        BeforeSubmitHook before_submit = {});
    ~AgentRegistry() noexcept;

    AgentRegistry(const AgentRegistry&) = delete;
    AgentRegistry& operator=(const AgentRegistry&) = delete;

    const std::vector<AgentRuntimeInfo>& runtime_info() const noexcept;

    // Strong guarantee: on success every task was accepted by the pool but is
    // held behind an unopened start gate. On failure no backend is called and
    // no task remains live.
    void stage_batch(std::vector<CompletionInput> inputs);
    void open_gate() noexcept;
    [[nodiscard]] ChannelReadStatus try_receive(
        std::size_t run_index,
        AgentEvent& event);
    void cancel_batch() noexcept;
    [[nodiscard]] bool executions_finished() const noexcept;
    // Synchronously cancels any remaining work, waits until no execution can
    // access a backend, then releases the one live batch.
    void clear_batch() noexcept;

    // Cancels and waits until execution state can no longer access a backend.
    // It retains terminal event queues for shutdown draining. The controller
    // must then join the pool before destroying the registry.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cha
