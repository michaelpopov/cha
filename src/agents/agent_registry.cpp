#include "agents/agent_registry.h"

#include "agents/completion_client.h"
#include "util/logging.h"
#include "util/text.h"

#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>

namespace cha {
namespace {

std::vector<AgentRuntimeInfo> build_runtime_info(
    const std::vector<std::unique_ptr<CompletionBackend>>& backends) {
    if (backends.empty()) {
        throw std::invalid_argument("Agent registry requires at least one agent");
    }
    std::vector<AgentRuntimeInfo> infos;
    infos.reserve(backends.size());
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> names;
    for (const auto& backend : backends) {
        if (!backend) {
            throw std::invalid_argument(
                "Agent registry requires completion backends");
        }
        AgentRuntimeInfo info = backend->info();
        validate_persona_id(info.persona.id);
        validate_persona_name(info.persona.name);
        if (!ids.insert(info.persona.id).second) {
            throw std::invalid_argument(
                "Agent registry has duplicate persona ID '" + info.persona.id
                + "'");
        }
        if (!names.insert(fold_ascii(info.persona.name)).second) {
            throw std::invalid_argument(
                "Agent registry has duplicate persona name '" + info.persona.name
                + "'");
        }
        infos.push_back(std::move(info));
    }
    return infos;
}

std::vector<std::unique_ptr<CompletionBackend>> build_backends(
    std::vector<AgentDefinition> definitions) {
    std::vector<std::unique_ptr<CompletionBackend>> backends;
    backends.reserve(definitions.size());
    for (AgentDefinition& definition : definitions) {
        const std::string id = definition.config.id;
        const std::string name = definition.config.name;
        try {
            backends.push_back(
                std::make_unique<CompletionClient>(std::move(definition)));
        } catch (const std::exception& error) {
            throw std::runtime_error(
                "Persona '" + name + "' (agent ID '" + id
                + "') failed to initialize: " + error.what());
        }
    }
    return backends;
}

} // namespace

struct AgentRegistry::Impl {
    enum class StopState {
        running,
        stopping,
        stopped,
    };

    enum class GateState {
        closed,
        open,
        cancelled,
    };

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
              fallback_terminal(AgentFailed{
                  input.run.request_id,
                  "Agent execution failed before details could be reported",
              }) {
            static_assert(std::is_nothrow_move_constructible_v<AgentEvent>);
            static_assert(std::is_nothrow_move_assignable_v<AgentEvent>);
        }

        void execute() noexcept {
            const RequestId request_id = input.run.request_id;
            if (!gate->wait()) {
                log_info("Agent execution cancelled before start");
                publish_terminal(AgentCancelled{request_id});
                finish();
                return;
            }

            try {
                if (cancellation.load(std::memory_order_acquire)) {
                    log_info("Agent execution cancelled before preparation");
                    publish_terminal(AgentCancelled{request_id});
                } else {
                    log_info("Agent execution started");
                    RequestPayload payload = backend.prepare(input);
                    const CompletionResult result = backend.perform(
                        std::move(payload),
                        [this, request_id](CompletionDelta delta) {
                            publish_delta(AgentDelta{
                                request_id,
                                delta.kind,
                                std::move(delta.text),
                            });
                        },
                        cancellation);
                    if (result.outcome == CompletionOutcome::completed) {
                        log_info("Agent execution completed");
                        publish_terminal(AgentCompleted{request_id});
                    } else if (result.outcome == CompletionOutcome::cancelled) {
                        log_info("Agent execution cancelled");
                        publish_terminal(AgentCancelled{request_id});
                    } else {
                        log_error("Agent execution failed");
                        publish_failure(request_id, result.message);
                    }
                }
            } catch (const std::exception& error) {
                log_error("Agent execution raised an exception");
                publish_failure(request_id, error.what());
            } catch (...) {
                log_error("Agent execution raised an unknown exception");
                publish_failure(request_id, "Unknown agent execution failure");
            }
            finish();
        }

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

        ChannelReadStatus try_receive(AgentEvent& event) {
            return events.try_get(event);
        }

    private:
        void publish_delta(AgentEvent event) {
            if (!events.push(std::move(event))) {
                throw std::logic_error(
                    "Agent event queue closed before execution stopped");
            }
            notifier.wake();
        }

        void publish_terminal(AgentEvent event) noexcept {
            events.close_with(std::move(event));
            notifier.wake();
        }

        void publish_failure(
            RequestId request_id,
            std::string_view message) noexcept {
            AgentEvent failure = std::move(fallback_terminal);
            try {
                failure = AgentFailed{request_id, std::string(message)};
            } catch (...) {
                // Keep the preallocated fallback if copying details allocates.
                log_critical(
                    "Agent failure details could not be preserved; using fallback");
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
        ConcurrentQueue<AgentEvent> events;
        AgentEvent fallback_terminal;
        std::atomic_bool cancellation{false};
        mutable std::mutex finished_mutex;
        std::condition_variable finished_changed;
        bool execution_finished{};
    };

    struct BatchRecord {
        std::shared_ptr<StartGate> gate;
        std::vector<std::shared_ptr<Execution>> executions;
    };

    Impl(
        std::vector<std::unique_ptr<CompletionBackend>> owned_backends,
        WakeNotifier& wake_notifier,
        ThreadPool& borrowed_pool,
        BeforeSubmitHook submission_hook)
        : backends(std::move(owned_backends)),
          runtime_info(build_runtime_info(backends)),
          notifier(wake_notifier),
          worker_pool(borrowed_pool),
          before_submit(std::move(submission_hook)) {
        // Exact equality is deliberate: this pool runs only agent work, and
        // one worker per backend guarantees full-width fan-out.
        if (worker_pool.worker_count() != backends.size()) {
            throw std::invalid_argument(
                "Agent registry requires one pool worker per backend");
        }
    }

    ~Impl() noexcept {
        // stop() already waits for execution completion; clear_batch() only
        // releases the retained batch and event queues.
        stop();
        clear_batch();
    }

    std::size_t backend_index(std::string_view id) const {
        for (std::size_t index = 0; index < runtime_info.size(); ++index) {
            if (runtime_info[index].persona.id == id) {
                return index;
            }
        }
        return runtime_info.size();
    }

    void stage_batch(std::vector<CompletionInput> inputs) {
        if (inputs.empty()) {
            throw std::invalid_argument(
                "Staged batch requires at least one completion input");
        }

        std::vector<std::size_t> backend_indices;
        backend_indices.reserve(inputs.size());
        std::unordered_set<std::size_t> distinct;
        for (const CompletionInput& input : inputs) {
            if (!input.history) {
                throw std::invalid_argument("Completion input requires history");
            }
            const std::size_t index = backend_index(input.run.target.id);
            if (index == runtime_info.size()) {
                throw std::invalid_argument("Completion input has unknown target");
            }
            if (!distinct.insert(index).second) {
                throw std::invalid_argument("Staged batch has duplicate targets");
            }
            backend_indices.push_back(index);
        }

        BatchRecord candidate;
        candidate.gate = std::make_shared<StartGate>();
        candidate.executions.reserve(inputs.size());
        for (std::size_t index = 0; index < inputs.size(); ++index) {
            candidate.executions.push_back(std::make_shared<Execution>(
                std::move(inputs[index]),
                *backends[backend_indices[index]],
                candidate.gate,
                notifier));
        }

        const std::shared_ptr<StartGate> gate = candidate.gate;
        std::vector<std::shared_ptr<Execution>> submitted;
        submitted.reserve(candidate.executions.size());
        std::unique_lock state_lock(state_mutex);
        if (stop_state != StopState::running || batch) {
            throw std::runtime_error("Agent registry is busy");
        }
        try {
            for (const auto& execution : candidate.executions) {
                if (before_submit) {
                    before_submit(submitted.size());
                }
                if (!worker_pool.submit([execution] { execution->execute(); })) {
                    throw std::runtime_error("Agent worker pool is unavailable");
                }
                submitted.push_back(execution);
            }
            batch.emplace(std::move(candidate));
        } catch (...) {
            state_lock.unlock();
            gate->cancel();
            for (const auto& execution : submitted) {
                execution->wait_until_finished();
            }
            throw;
        }
    }

    void open_gate() noexcept {
        std::shared_ptr<StartGate> gate;
        {
            std::lock_guard lock(state_mutex);
            if (batch) {
                gate = batch->gate;
            }
        }
        if (gate) {
            gate->open();
        }
    }

    ChannelReadStatus try_receive(std::size_t run_index, AgentEvent& event) {
        std::shared_ptr<Execution> execution;
        bool is_stopped = false;
        {
            std::lock_guard lock(state_mutex);
            is_stopped = stop_state == StopState::stopped;
            if (batch && run_index >= batch->executions.size()) {
                throw std::logic_error("Agent registry run index is out of range");
            }
            if (batch) {
                execution = batch->executions[run_index];
            }
        }
        if (!execution) {
            return is_stopped ? ChannelReadStatus::closed : ChannelReadStatus::empty;
        }
        const ChannelReadStatus status = execution->try_receive(event);
        if (status == ChannelReadStatus::value) {
            return status;
        }
        return is_stopped ? ChannelReadStatus::closed : ChannelReadStatus::empty;
    }

    void cancel_batch() noexcept {
        std::shared_ptr<StartGate> gate;
        std::vector<std::shared_ptr<Execution>> executions;
        {
            std::lock_guard lock(state_mutex);
            if (!batch) {
                return;
            }
            gate = batch->gate;
            executions = batch->executions;
        }
        for (const auto& execution : executions) {
            execution->request_cancel();
        }
        gate->cancel();
    }

    bool executions_finished() const noexcept {
        std::vector<std::shared_ptr<Execution>> executions;
        {
            std::lock_guard lock(state_mutex);
            if (!batch) {
                return true;
            }
            executions = batch->executions;
        }
        for (const auto& execution : executions) {
            if (!execution->is_finished()) {
                return false;
            }
        }
        return true;
    }

    void clear_batch() noexcept {
        std::optional<BatchRecord> cleared;
        {
            std::lock_guard lock(state_mutex);
            if (!batch) {
                return;
            }
            cleared.emplace(std::move(*batch));
            batch.reset();
        }
        cleared->gate->cancel();
        for (const auto& execution : cleared->executions) {
            execution->request_cancel();
            execution->wait_until_finished();
        }
    }

    void stop() noexcept {
        std::vector<std::shared_ptr<Execution>> executions;
        std::shared_ptr<StartGate> gate;
        {
            std::lock_guard lock(state_mutex);
            if (stop_state != StopState::running) {
                return;
            }
            stop_state = StopState::stopping;
            if (batch) {
                gate = batch->gate;
                executions = batch->executions;
            }
        }
        for (const auto& execution : executions) {
            execution->request_cancel();
        }
        if (gate) {
            gate->cancel();
        }
        for (const auto& execution : executions) {
            execution->wait_until_finished();
        }
        {
            std::lock_guard lock(state_mutex);
            stop_state = StopState::stopped;
        }
        notifier.wake();
    }

    std::vector<std::unique_ptr<CompletionBackend>> backends;
    std::vector<AgentRuntimeInfo> runtime_info;
    WakeNotifier& notifier;
    ThreadPool& worker_pool;
    BeforeSubmitHook before_submit;

    mutable std::mutex state_mutex;
    std::optional<BatchRecord> batch;
    // Staging is allowed only while running; stop() advances this state once.
    StopState stop_state{StopState::running};
};

AgentRegistry::AgentRegistry(
    std::vector<AgentDefinition> definitions,
    WakeNotifier& notifier,
    ThreadPool& worker_pool)
    : AgentRegistry(build_backends(std::move(definitions)), notifier, worker_pool) {
}

AgentRegistry::AgentRegistry(
    std::vector<std::unique_ptr<CompletionBackend>> backends,
    WakeNotifier& notifier,
    ThreadPool& worker_pool,
    BeforeSubmitHook before_submit)
    : impl_(std::make_unique<Impl>(
          std::move(backends), notifier, worker_pool, std::move(before_submit))) {
}

AgentRegistry::~AgentRegistry() noexcept = default;

const std::vector<AgentRuntimeInfo>& AgentRegistry::runtime_info() const noexcept {
    return impl_->runtime_info;
}

void AgentRegistry::stage_batch(std::vector<CompletionInput> inputs) {
    impl_->stage_batch(std::move(inputs));
}

void AgentRegistry::open_gate() noexcept {
    impl_->open_gate();
}

ChannelReadStatus AgentRegistry::try_receive(
    std::size_t run_index,
    AgentEvent& event) {
    return impl_->try_receive(run_index, event);
}

void AgentRegistry::cancel_batch() noexcept {
    impl_->cancel_batch();
}

bool AgentRegistry::executions_finished() const noexcept {
    return impl_->executions_finished();
}

void AgentRegistry::clear_batch() noexcept {
    impl_->clear_batch();
}

void AgentRegistry::stop() {
    impl_->stop();
}

} // namespace cha
