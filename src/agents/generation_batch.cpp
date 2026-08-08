#include "agents/generation_batch.h"

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

// One execution slot: it owns its GenerationRequest, borrows exactly one backend
// and the notifier, shares the batch's gate, and owns its cancellation flag and
// event queue. It publishes any number of deltas followed by exactly one
// terminal event, including when it is cancelled before start or throws.
struct Execution {
    Execution(
        GenerationRequest owned_input,
        ModelBackend& owned_backend,
        std::shared_ptr<StartGate> start_gate,
        WakeNotifier& wake_notifier)
        : input(std::move(owned_input)),
          backend(owned_backend),
          gate(std::move(start_gate)),
          notifier(wake_notifier),
          fallback_terminal(GenerationFailed{
              input.run.request_id,
              "Generation execution failed before details could be reported",
          }) {
        static_assert(std::is_nothrow_move_constructible_v<GenerationEvent>);
        static_assert(std::is_nothrow_move_assignable_v<GenerationEvent>);
    }

    void execute() noexcept {
        const RequestId request_id = input.run.request_id;
        if (!gate->wait()) {
            log_info("Generation execution cancelled before start");
            publish_terminal(GenerationCancelled{request_id});
            finish();
            return;
        }

        try {
            if (cancellation.load(std::memory_order_acquire)) {
                log_info("Generation execution cancelled before preparation");
                publish_terminal(GenerationCancelled{request_id});
            } else {
                log_info("Generation execution started");
                RequestPayload payload = backend.prepare(input);
                const GenerationResult result = backend.perform(
                    std::move(payload),
                    [this, request_id](GenerationDelta delta) {
                        publish_delta(GenerationEventDelta{
                            request_id,
                            delta.kind,
                            std::move(delta.text),
                        });
                    },
                    cancellation);
                if (result.outcome == GenerationOutcome::completed) {
                    log_info("Generation execution completed");
                    publish_terminal(GenerationCompleted{request_id});
                } else if (result.outcome == GenerationOutcome::cancelled) {
                    log_info("Generation execution cancelled");
                    publish_terminal(GenerationCancelled{request_id});
                } else {
                    log_error("Generation execution failed");
                    publish_failure(request_id, result.message);
                }
            }
        } catch (const std::exception& error) {
            log_error("Generation execution raised an exception");
            publish_failure(request_id, error.what());
        } catch (...) {
            log_error("Generation execution raised an unknown exception");
            publish_failure(request_id, "Unknown generation execution failure");
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

    ChannelReadStatus try_receive(GenerationEvent& event) {
        return events.try_get(event);
    }

private:
    void publish_delta(GenerationEvent event) {
        if (!events.push(std::move(event))) {
            throw std::logic_error(
                "Generation event queue closed before execution stopped");
        }
        notifier.wake();
    }

    void publish_terminal(GenerationEvent event) noexcept {
        events.close_with(std::move(event));
        notifier.wake();
    }

    void publish_failure(
        RequestId request_id,
        std::string_view message) noexcept {
        GenerationEvent failure = std::move(fallback_terminal);
        try {
            failure = GenerationFailed{request_id, std::string(message)};
        } catch (...) {
            // Keep the preallocated fallback if copying details allocates.
            log_critical(
                "Generation failure details could not be preserved; using fallback");
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

    GenerationRequest input;
    ModelBackend& backend;
    std::shared_ptr<StartGate> gate;
    WakeNotifier& notifier;
    ConcurrentQueue<GenerationEvent> events;
    GenerationEvent fallback_terminal;
    std::atomic_bool cancellation{false};
    mutable std::mutex finished_mutex;
    std::condition_variable finished_changed;
    bool execution_finished{};
};

bool is_terminal(const GenerationEvent& event) noexcept {
    return !std::holds_alternative<GenerationEventDelta>(event);
}

} // namespace

struct GenerationBatch::Impl {
    std::shared_ptr<StartGate> gate;
    // Fixed for the batch's whole lifetime, so slot positions never drift.
    std::vector<std::shared_ptr<Execution>> executions;
    std::size_t foreground{};
    bool foreground_terminal_delivered{};
    bool cancelled{};
};

GenerationBatch GenerationBatch::stage(
    std::vector<GenerationRequest> inputs,
    const std::vector<ModelBackend*>& backends,
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
                throw std::runtime_error("Generation worker pool is unavailable");
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
    return GenerationBatch(std::move(impl));
}

GenerationBatch::GenerationBatch(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {
}

GenerationBatch::GenerationBatch(GenerationBatch&&) noexcept = default;

GenerationBatch::~GenerationBatch() noexcept {
    if (!impl_) {
        return;
    }
    cancel();
    wait_until_finished();
}

const RunSpec& GenerationBatch::foreground_run() const {
    if (impl_->foreground >= impl_->executions.size()) {
        throw std::logic_error("Generation batch has no foreground run");
    }
    return impl_->executions[impl_->foreground]->run();
}

std::size_t GenerationBatch::foreground_index() const noexcept {
    return impl_->foreground;
}

bool GenerationBatch::has_next_foreground() const noexcept {
    return impl_->foreground + 1 < impl_->executions.size();
}

void GenerationBatch::open() noexcept {
    impl_->gate->open();
}

ChannelReadStatus GenerationBatch::try_receive_foreground(GenerationEvent& event) {
    if (impl_->foreground >= impl_->executions.size()) {
        throw std::logic_error("Generation batch has no foreground run");
    }
    const ChannelReadStatus status =
        impl_->executions[impl_->foreground]->try_receive(event);
    if (status == ChannelReadStatus::value && is_terminal(event)) {
        impl_->foreground_terminal_delivered = true;
    }
    return status;
}

void GenerationBatch::advance_foreground() {
    if (impl_->foreground >= impl_->executions.size()) {
        throw std::logic_error("Generation batch has no foreground run");
    }
    if (!impl_->foreground_terminal_delivered) {
        throw std::logic_error(
            "Generation batch foreground has no delivered terminal event");
    }
    if (!has_next_foreground()) {
        throw std::logic_error("Generation batch has no next foreground run");
    }
    ++impl_->foreground;
    impl_->foreground_terminal_delivered = false;
}

void GenerationBatch::cancel() noexcept {
    impl_->cancelled = true;
    for (const std::shared_ptr<Execution>& execution : impl_->executions) {
        execution->request_cancel();
    }
    // A closed gate becomes cancelled and calls no backend; an already opened
    // gate leaves cancellation to the per-execution flags set above.
    impl_->gate->cancel();
}

bool GenerationBatch::cancellation_requested() const noexcept {
    return impl_->cancelled;
}

bool GenerationBatch::executions_finished() const noexcept {
    for (const std::shared_ptr<Execution>& execution : impl_->executions) {
        if (!execution->is_finished()) {
            return false;
        }
    }
    return true;
}

void GenerationBatch::wait_until_finished() noexcept {
    for (const std::shared_ptr<Execution>& execution : impl_->executions) {
        execution->wait_until_finished();
    }
}

} // namespace cha
