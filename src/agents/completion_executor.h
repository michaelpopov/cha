#pragma once

#include "agents/character.h"
#include "agents/completion_backend.h"
#include "agents/completion_batch.h"
#include "util/thread_pool.h"
#include "util/wake_notifier.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace cha {

// The session-lived supplier of completion backends. It owns one backend per
// forum character plus their public runtime metadata, and stages new operations
// into the borrowed worker pool.
//
// It deliberately keeps no current batch: staging hands complete ownership of
// one operation to the caller, so there is nothing here addressed to an
// implicit batch.
class CompletionExecutor final {
public:
    // Test-only fault injection immediately before each pool submission. The
    // argument is the zero-based submission index and exceptions abort staging.
    using BeforeSubmitHook = std::function<void(std::size_t)>;

    // The caller owns the pool and must join it while this executor and its
    // notifier remain alive.
    CompletionExecutor(
        std::vector<CharacterDefinition> definitions,
        WakeNotifier& notifier,
        ThreadPool& worker_pool);
    CompletionExecutor(
        std::vector<std::unique_ptr<CompletionBackend>> backends,
        WakeNotifier& notifier,
        ThreadPool& worker_pool,
        BeforeSubmitHook before_submit = {});

    CompletionExecutor(const CompletionExecutor&) = delete;
    CompletionExecutor& operator=(const CompletionExecutor&) = delete;

    [[nodiscard]] const std::vector<CompletionBackendInfo>& runtime_info()
        const noexcept;

    // Owner-thread staging. Strong guarantee: on success every task was
    // accepted by the pool but is held behind the returned batch's unopened
    // gate. On failure no backend is called and no task remains live.
    [[nodiscard]] CompletionBatch stage_batch(
        std::vector<CompletionInput> inputs);

private:
    [[nodiscard]] std::size_t backend_index(std::string_view id) const;

    std::vector<std::unique_ptr<CompletionBackend>> backends_;
    std::vector<CompletionBackendInfo> runtime_info_;
    WakeNotifier& notifier_;
    ThreadPool& worker_pool_;
    BeforeSubmitHook before_submit_;
};

} // namespace cha
