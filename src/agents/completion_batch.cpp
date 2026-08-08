#include "agents/completion_batch.h"

#include "util/logging.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace cha {
namespace {

enum class GateState {
    closed,
    open,
    cancelled,
};

// One start decision shared by every execution in a batch. The first
// transition wins, so opening and cancelling are both idempotent.
struct StartGate {
    [[nodiscard]] bool wait() noexcept {
        std::unique_lock lock(mutex);
        changed.wait(lock, [this] { return state != GateState::closed; });
        return state == GateState::open;
    }

    void open() noexcept { transition(GateState::open); }
    void cancel() noexcept { transition(GateState::cancelled); }

private:
    void transition(GateState next) noexcept {
        {
            std::lock_guard lock(mutex);
            if (state != GateState::closed) {
                return;
            }
            state = next;
        }
        changed.notify_all();
    }

    std::mutex mutex;
    std::condition_variable changed;
    GateState state{GateState::closed};
};

// One execution slot: it owns its CompletionInput, borrows exactly one backend
// and the notifier, shares the batch's gate, and owns its cancellation flag and
// event queue. It publishes any number of deltas followed by exactly one
// terminal event, including when it is cancelled before start or throws.
struct Execution {
    Execution(
        CompletionInput owned_input,
        CompletionBackend& owned_backend,
        std::shared_ptr<StartGate> start_gate,
        WakeNotifier& wake_notifier)
        : input(std::move(owned_input)),
          backend(owned_backend),
          gate(std::move(start_gate)),
          notifier(wake_notifier),
          fallback_terminal(CompletionFailed{
              input.run.request_id,
              "Completion execution failed before details could be reported",
          }) {
        static_assert(std::is_nothrow_move_constructible_v<CompletionEvent>);
        static_assert(std::is_nothrow_move_assignable_v<CompletionEvent>);
    }

    void execute() noexcept {
        const RequestId request_id = input.run.request_id;
        if (!gate->wait()) {
            log_info("Completion execution cancelled before start");
            publish_terminal(CompletionCancelled{request_id});
            finish();
            return;
        }

        try {
            if (cancellation.load(std::memory_order_acquire)) {
                log_info("Completion execution cancelled before preparation");
                publish_terminal(CompletionCancelled{request_id});
            } else {
                log_info("Completion execution started");
                RequestPayload payload = backend.prepare(input);
                const CompletionResult result = backend.perform(
                    std::move(payload),
                    [this, request_id](CompletionDelta delta) {
                        publish_delta(CompletionEventDelta{
                            request_id,
                            delta.kind,
                            std::move(delta.text),
                        });
                    },
                    cancellation);
                if (result.outcome == CompletionOutcome::completed) {
                    log_info("Completion execution completed");
                    publish_terminal(CompletionCompleted{request_id});
                } else if (result.outcome == CompletionOutcome::cancelled) {
                    log_info("Completion execution cancelled");
                    publish_terminal(CompletionCancelled{request_id});
                } else {
                    log_error("Completion execution failed");
                    publish_failure(request_id, result.message);
                }
            }
        } catch (const std::exception& error) {
            log_error("Completion execution raised an exception");
            publish_failure(request_id, error.what());
        } catch (...) {
            log_error("Completion execution raised an unknown exception");
            publish_failure(request_id, "Unknown completion execution failure");
        }
        finish();
    }

    [[nodiscard]] const RunSpec& run() const noexcept { return input.run; }

    void request_cancel() noexcept {
        cancellation.store(true, std::memory_order_release);
    }

    void wait_until_finished() noexcept {
        std::unique_lock lock(finished_mutex);
        finished_changed.wait(lock, [this] { return execution_finished; });
    }

    [[nodiscard]] bool is_finished() const noexcept {
        std::lock_guard lock(finished_mutex);
        return execution_finished;
    }

    ChannelReadStatus try_receive(CompletionEvent& event) {
        return events.try_get(event);
    }

private:
    void publish_delta(CompletionEvent event) {
        if (!events.push(std::move(event))) {
            throw std::logic_error(
                "Completion event queue closed before execution stopped");
        }
        notifier.wake();
    }

    void publish_terminal(CompletionEvent event) noexcept {
        events.close_with(std::move(event));
        notifier.wake();
    }

    void publish_failure(
        RequestId request_id,
        std::string_view message) noexcept {
        CompletionEvent failure = std::move(fallback_terminal);
        try {
            failure = CompletionFailed{request_id, std::string(message)};
        } catch (...) {
            // Keep the preallocated fallback if copying details allocates.
            log_critical(
                "Completion failure details could not be preserved; using fallback");
        }
        publish_terminal(std::move(failure));
    }

    void finish() noexcept {
        {
            std::lock_guard lock(finished_mutex);
            execution_finished = true;
        }
        finished_changed.notify_all();
        // This can occur after a waiter observes execution_finished.  The
        // flag guarantees backend safety, while ThreadPool::stop() is the
        // stronger task-quiescence boundary used at shutdown.
        notifier.wake();
    }

    CompletionInput input;
    CompletionBackend& backend;
    std::shared_ptr<StartGate> gate;
    WakeNotifier& notifier;
    ConcurrentQueue<CompletionEvent> events;
    CompletionEvent fallback_terminal;
    std::atomic_bool cancellation{false};
    mutable std::mutex finished_mutex;
    std::condition_variable finished_changed;
    bool execution_finished{};
};

bool is_terminal(const CompletionEvent& event) noexcept {
    return !std::holds_alternative<CompletionEventDelta>(event);
}

} // namespace

struct CompletionBatch::Impl {
    std::shared_ptr<StartGate> gate;
    // Fixed for the batch's whole lifetime, so slot positions never drift.
    std::vector<std::shared_ptr<Execution>> executions;
    std::size_t foreground{};
    bool foreground_terminal_delivered{};
    bool cancelled{};
};

CompletionBatch CompletionBatch::stage(
    std::vector<CompletionInput> inputs,
    const std::vector<CompletionBackend*>& backends,
    WakeNotifier& notifier,
    ThreadPool& worker_pool,
    const std::function<void(std::size_t)>& before_submit) {
    auto impl = std::make_unique<Impl>();
    impl->gate = std::make_shared<StartGate>();
    impl->executions.reserve(inputs.size());
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        impl->executions.push_back(std::make_shared<Execution>(
            std::move(inputs[index]), *backends[index], impl->gate, notifier));
    }

    std::size_t submitted = 0;
    try {
        for (const std::shared_ptr<Execution>& execution : impl->executions) {
            if (before_submit) {
                before_submit(submitted);
            }
            if (!worker_pool.submit([execution] { execution->execute(); })) {
                throw std::runtime_error("Completion worker pool is unavailable");
            }
            ++submitted;
        }
    } catch (...) {
        // No backend can be reached through the unopened gate; wait only for
        // the tasks the pool actually accepted, then release the rest.
        impl->gate->cancel();
        for (std::size_t index = 0; index < submitted; ++index) {
            impl->executions[index]->wait_until_finished();
        }
        throw;
    }
    return CompletionBatch(std::move(impl));
}

CompletionBatch::CompletionBatch(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {
}

CompletionBatch::CompletionBatch(CompletionBatch&&) noexcept = default;

CompletionBatch::~CompletionBatch() noexcept {
    if (!impl_) {
        return;
    }
    cancel();
    wait_until_finished();
}

const RunSpec& CompletionBatch::foreground_run() const {
    if (impl_->foreground >= impl_->executions.size()) {
        throw std::logic_error("Completion batch has no foreground run");
    }
    return impl_->executions[impl_->foreground]->run();
}

std::size_t CompletionBatch::foreground_index() const noexcept {
    return impl_->foreground;
}

bool CompletionBatch::has_next_foreground() const noexcept {
    return impl_->foreground + 1 < impl_->executions.size();
}

void CompletionBatch::open() noexcept {
    impl_->gate->open();
}

ChannelReadStatus CompletionBatch::try_receive_foreground(CompletionEvent& event) {
    if (impl_->foreground >= impl_->executions.size()) {
        throw std::logic_error("Completion batch has no foreground run");
    }
    const ChannelReadStatus status =
        impl_->executions[impl_->foreground]->try_receive(event);
    if (status == ChannelReadStatus::value && is_terminal(event)) {
        impl_->foreground_terminal_delivered = true;
    }
    return status;
}

void CompletionBatch::advance_foreground() {
    if (impl_->foreground >= impl_->executions.size()) {
        throw std::logic_error("Completion batch has no foreground run");
    }
    if (!impl_->foreground_terminal_delivered) {
        throw std::logic_error(
            "Completion batch foreground has no delivered terminal event");
    }
    if (!has_next_foreground()) {
        throw std::logic_error("Completion batch has no next foreground run");
    }
    ++impl_->foreground;
    impl_->foreground_terminal_delivered = false;
}

void CompletionBatch::cancel() noexcept {
    impl_->cancelled = true;
    for (const std::shared_ptr<Execution>& execution : impl_->executions) {
        execution->request_cancel();
    }
    // A closed gate becomes cancelled and calls no backend; an already opened
    // gate leaves cancellation to the per-execution flags set above.
    impl_->gate->cancel();
}

bool CompletionBatch::cancellation_requested() const noexcept {
    return impl_->cancelled;
}

bool CompletionBatch::executions_finished() const noexcept {
    for (const std::shared_ptr<Execution>& execution : impl_->executions) {
        if (!execution->is_finished()) {
            return false;
        }
    }
    return true;
}

void CompletionBatch::wait_until_finished() noexcept {
    for (const std::shared_ptr<Execution>& execution : impl_->executions) {
        execution->wait_until_finished();
    }
}

} // namespace cha
