#include "agents/agent_registry.h"

#include "agents/completion_client.h"
#include "util/text.h"

#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
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
                "Agent registry has duplicate persona ID '"
                + info.persona.id + "'");
        }
        if (!names.insert(fold_ascii(info.persona.name)).second) {
            throw std::invalid_argument(
                "Agent registry has duplicate persona name '"
                + info.persona.name + "'");
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
    enum class GateState {
        closed,
        open,
        cancelled,
    };

    struct StartGate {
        bool arrive_and_wait() {
            std::unique_lock lock(mutex);
            ++parked;
            parked_changed.notify_one();
            changed.wait(lock, [this] {
                return state != GateState::closed;
            });
            return state == GateState::open;
        }

        void wait_until_parked(std::size_t expected) {
            std::unique_lock lock(mutex);
            parked_changed.wait(lock, [this, expected] {
                return parked == expected;
            });
        }

        void open() noexcept {
            transition(GateState::open);
        }

        void cancel() noexcept {
            transition(GateState::cancelled);
        }

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
        std::condition_variable parked_changed;
        GateState state{GateState::closed};
        std::size_t parked{};
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
            if (!gate->arrive_and_wait()) {
                publish_terminal(AgentCancelled{request_id});
                finish();
                return;
            }

            try {
                if (cancellation.load(std::memory_order_acquire)) {
                    publish_terminal(AgentCancelled{request_id});
                    finish();
                    return;
                }
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
                    publish_terminal(AgentCompleted{request_id});
                } else if (result.outcome == CompletionOutcome::cancelled) {
                    publish_terminal(AgentCancelled{request_id});
                } else {
                    publish_terminal(AgentFailed{request_id, result.message});
                }
            } catch (const std::exception& error) {
                publish_failure(request_id, error.what());
            } catch (...) {
                publish_failure(
                    request_id,
                    "Unknown agent execution failure");
            }
            finish();
        }

        void request_cancel() noexcept {
            cancellation.store(true, std::memory_order_release);
        }

        void wait() noexcept {
            std::unique_lock lock(done_mutex);
            done_changed.wait(lock, [this] { return done; });
        }

        bool is_done() noexcept {
            std::lock_guard lock(done_mutex);
            return done;
        }

        ChannelReadStatus try_receive(AgentEvent& event) {
            return events.try_get(event);
        }

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
                AgentEvent detailed = AgentFailed{
                    request_id,
                    std::string(message),
                };
                failure = std::move(detailed);
            } catch (...) {
                // Keep the preallocated failure when copying details allocates.
            }
            publish_terminal(std::move(failure));
        }

        void finish() noexcept {
            {
                std::lock_guard lock(done_mutex);
                done = true;
            }
            done_changed.notify_all();
            // Terminal publication may wake and be consumed before execute()
            // reaches this point. Abort polling needs a later wake once it is
            // safe to reset or join this runner without waiting on a backend.
            notifier.wake();
        }

        CompletionInput input;
        CompletionBackend& backend;
        std::shared_ptr<StartGate> gate;
        WakeNotifier& notifier;
        ConcurrentQueue<AgentEvent> events;
        AgentEvent fallback_terminal;
        std::atomic_bool cancellation{false};
        std::mutex done_mutex;
        std::condition_variable done_changed;
        bool done{};
    };

    class RegularRunner {
    public:
        RegularRunner()
            : thread_(&RegularRunner::dialog, this) {
        }

        ~RegularRunner() noexcept {
            stop();
        }

        bool stage(const std::shared_ptr<Execution>& execution) {
            std::lock_guard lock(mutex_);
            if (stopping_ || current_) {
                return false;
            }
            current_ = execution;
            pending_ = execution;
            changed_.notify_one();
            return true;
        }

        void reset(const std::shared_ptr<Execution>& execution) noexcept {
            execution->wait();
            std::lock_guard lock(mutex_);
            if (current_ == execution) {
                current_.reset();
            }
        }

        void stop() noexcept {
            {
                std::lock_guard lock(mutex_);
                if (stopping_) {
                    return;
                }
                stopping_ = true;
            }
            changed_.notify_all();
            if (thread_.joinable()) {
                try {
                    thread_.join();
                } catch (...) {
                    std::terminate();
                }
            }
        }

    private:
        void dialog() noexcept {
            while (true) {
                std::shared_ptr<Execution> execution;
                {
                    std::unique_lock lock(mutex_);
                    changed_.wait(lock, [this] {
                        return stopping_ || pending_;
                    });
                    if (stopping_ && !pending_) {
                        break;
                    }
                    execution = std::move(pending_);
                }
                execution->execute();
            }
        }

        std::mutex mutex_;
        std::condition_variable changed_;
        std::shared_ptr<Execution> current_;
        std::shared_ptr<Execution> pending_;
        bool stopping_{};
        std::thread thread_;
    };

    struct RunRecord {
        std::size_t backend_index{};
        bool regular{};
        bool started{};
        std::shared_ptr<Execution> execution;
        std::thread thread;
    };

    struct BatchRecord {
        BatchId id{};
        std::vector<std::unique_ptr<RunRecord>> runs;
        std::optional<std::size_t> foreground;
        std::shared_ptr<StartGate> gate;
        bool aborting{};
    };

    Impl(
        std::vector<std::unique_ptr<CompletionBackend>> owned_backends,
        WakeNotifier& wake_notifier,
        std::function<void()> stage_thread_hook)
        : backends(std::move(owned_backends)),
          runtime_info(build_runtime_info(backends)),
          leased(backends.size(), false),
          notifier(wake_notifier),
          before_stage_thread_start(std::move(stage_thread_hook)) {
    }

    ~Impl() noexcept {
        try {
            stop();
        } catch (...) {
        }
        std::optional<BatchId> remaining;
        {
            std::lock_guard lock(state_mutex);
            if (batch) {
                remaining = batch->id;
            }
        }
        if (remaining) {
            discard_batch(*remaining);
        }
        regular.stop();
    }

    std::size_t backend_index(std::string_view id) const {
        for (std::size_t index = 0; index < runtime_info.size(); ++index) {
            if (runtime_info[index].persona.id == id) {
                return index;
            }
        }
        return runtime_info.size();
    }

    void release_lease(std::size_t index) noexcept {
        std::lock_guard lock(state_mutex);
        if (index < leased.size()) {
            leased[index] = false;
        }
    }

    void cleanup_record(std::unique_ptr<RunRecord> record) noexcept {
        if (!record) {
            return;
        }
        record->execution->request_cancel();
        record->execution->wait();
        if (record->regular) {
            regular.reset(record->execution);
        } else if (record->thread.joinable()) {
            try {
                record->thread.join();
            } catch (...) {
                std::terminate();
            }
        }
        release_lease(record->backend_index);
    }

    void rollback_staging(BatchRecord& candidate) noexcept {
        candidate.gate->cancel();
        for (std::unique_ptr<RunRecord>& record : candidate.runs) {
            if (record && record->started) {
                cleanup_record(std::move(record));
            } else if (record) {
                release_lease(record->backend_index);
            }
        }
    }

    BatchId stage_batch(std::vector<CompletionInput> inputs) {
        if (inputs.empty()) {
            throw std::invalid_argument(
                "Staged batch requires at least one completion input");
        }

        std::vector<std::size_t> backend_indices;
        backend_indices.reserve(inputs.size());
        std::unordered_set<std::size_t> distinct;
        for (const CompletionInput& input : inputs) {
            if (!input.history) {
                throw std::invalid_argument(
                    "Completion input requires history");
            }
            const std::size_t index =
                backend_index(input.run.target.id);
            if (index == runtime_info.size()) {
                throw std::invalid_argument(
                    "Completion input has unknown target");
            }
            if (!distinct.insert(index).second) {
                throw std::invalid_argument(
                    "Staged batch has duplicate targets");
            }
            backend_indices.push_back(index);
        }

        const BatchId batch_id = next_batch_id++;
        BatchRecord candidate;
        candidate.id = batch_id;
        candidate.gate = std::make_shared<StartGate>();
        candidate.runs.reserve(inputs.size());

        std::vector<RunRecord*> record_views;
        record_views.reserve(inputs.size());
        for (std::size_t index = 0; index < inputs.size(); ++index) {
            auto record = std::make_unique<RunRecord>();
            record->backend_index = backend_indices[index];
            record->regular = index == 0;
            record->execution = std::make_shared<Execution>(
                std::move(inputs[index]),
                *backends[backend_indices[index]],
                candidate.gate,
                notifier);
            record_views.push_back(record.get());
            candidate.runs.push_back(std::move(record));
        }

        std::vector<std::size_t> claimed;
        claimed.reserve(backend_indices.size());
        std::unique_lock state_lock(state_mutex);
        if (stopping || stopped || batch) {
            throw std::runtime_error("Agent registry is busy");
        }
        for (const std::size_t index : backend_indices) {
            if (leased[index]) {
                for (const std::size_t rollback : claimed) {
                    leased[rollback] = false;
                }
                throw std::runtime_error("Agent backend is already leased");
            }
            leased[index] = true;
            claimed.push_back(index);
        }

        try {
            if (!regular.stage(record_views.front()->execution)) {
                throw std::runtime_error("Regular agent runner is unavailable");
            }
            record_views.front()->started = true;
            for (std::size_t index = 1; index < record_views.size(); ++index) {
                RunRecord& record = *record_views[index];
                if (before_stage_thread_start) {
                    before_stage_thread_start();
                }
                record.thread = std::thread(
                    [execution = record.execution] {
                        execution->execute();
                    });
                record.started = true;
            }
            candidate.gate->wait_until_parked(record_views.size());
        } catch (...) {
            state_lock.unlock();
            rollback_staging(candidate);
            throw;
        }

        static_assert(std::is_nothrow_move_constructible_v<BatchRecord>);
        batch.emplace(std::move(candidate));
        return batch_id;
    }

    void set_foreground(BatchId batch_id, std::size_t run_index) {
        std::lock_guard lock(state_mutex);
        if (stopped || !batch || batch->id != batch_id
            || run_index >= batch->runs.size()
            || !batch->runs[run_index]) {
            throw std::logic_error(
                "Foreground run is not live in the agent registry");
        }
        batch->foreground = run_index;
    }

    void open_batch_gate(BatchId batch_id) noexcept {
        std::shared_ptr<StartGate> gate;
        {
            std::lock_guard lock(state_mutex);
            if (batch && batch->id == batch_id) {
                gate = batch->gate;
            }
        }
        if (gate) {
            gate->open();
        }
    }

    void retire(BatchId batch_id, std::size_t run_index) {
        std::unique_ptr<RunRecord> record;
        {
            std::lock_guard lock(state_mutex);
            if (!batch || batch->id != batch_id
                || run_index >= batch->runs.size()
                || !batch->runs[run_index]) {
                return;
            }
            if (batch->foreground == run_index) {
                batch->foreground.reset();
            }
            record = std::move(batch->runs[run_index]);
        }
        cleanup_record(std::move(record));
    }

    void retire_batch(BatchId batch_id) noexcept {
        std::lock_guard lock(state_mutex);
        if (!batch || batch->id != batch_id) {
            return;
        }
        for (const std::unique_ptr<RunRecord>& run : batch->runs) {
            if (run) {
                return;
            }
        }
        batch.reset();
    }

    void cancel_all() noexcept {
        std::lock_guard lock(state_mutex);
        if (!batch) {
            return;
        }
        for (const std::unique_ptr<RunRecord>& run : batch->runs) {
            if (run) {
                run->execution->request_cancel();
            }
        }
    }

    void discard_batch(BatchId batch_id) noexcept {
        std::optional<BatchRecord> discarded;
        {
            std::lock_guard lock(state_mutex);
            if (!batch || batch->id != batch_id) {
                return;
            }
            discarded.emplace(std::move(*batch));
            batch.reset();
        }
        discarded->gate->cancel();
        for (std::unique_ptr<RunRecord>& record : discarded->runs) {
            cleanup_record(std::move(record));
        }
    }

    void begin_abort_cleanup(
        BatchId batch_id,
        std::optional<std::size_t> retained_foreground) noexcept {
        std::lock_guard lock(state_mutex);
        if (!batch || batch->id != batch_id) {
            return;
        }
        for (const std::unique_ptr<RunRecord>& run : batch->runs) {
            if (run) {
                run->execution->request_cancel();
            }
        }
        batch->gate->cancel();
        batch->aborting = true;
        const bool retain =
            retained_foreground
            && *retained_foreground < batch->runs.size()
            && batch->runs[*retained_foreground];
        if (retain) {
            batch->foreground = *retained_foreground;
        } else {
            batch->foreground.reset();
        }
    }

    void release_abort_foreground(
        BatchId batch_id,
        std::size_t run_index) noexcept {
        std::lock_guard lock(state_mutex);
        if (!batch || batch->id != batch_id || !batch->aborting
            || run_index >= batch->runs.size()
            || !batch->runs[run_index]) {
            return;
        }
        if (batch->foreground == run_index) {
            batch->foreground.reset();
        }
    }

    CleanupStatus poll_abort_cleanup(BatchId batch_id) noexcept {
        while (true) {
            std::unique_ptr<RunRecord> ready;
            bool complete = false;
            {
                std::lock_guard lock(state_mutex);
                if (!batch || batch->id != batch_id
                    || !batch->aborting) {
                    return CleanupStatus::none;
                }
                bool any_live = false;
                for (std::size_t index = 0;
                     index < batch->runs.size();
                     ++index) {
                    if (!batch->runs[index]) {
                        continue;
                    }
                    any_live = true;
                    if (batch->foreground == index) {
                        continue;
                    }
                    if (batch->runs[index]->execution->is_done()) {
                        ready = std::move(batch->runs[index]);
                        break;
                    }
                }
                complete = !any_live;
            }
            if (ready) {
                cleanup_record(std::move(ready));
                continue;
            }
            if (!complete) {
                return CleanupStatus::pending;
            }
            retire_batch(batch_id);
            return CleanupStatus::complete;
        }
    }

    ChannelReadStatus try_receive(AgentEvent& event) {
        std::shared_ptr<Execution> execution;
        bool is_stopped = false;
        {
            std::lock_guard lock(state_mutex);
            is_stopped = stopped;
            if (batch && batch->foreground
                && *batch->foreground < batch->runs.size()
                && batch->runs[*batch->foreground]) {
                execution =
                    batch->runs[*batch->foreground]->execution;
            }
        }
        if (!execution) {
            return is_stopped
                ? ChannelReadStatus::closed
                : ChannelReadStatus::empty;
        }

        const ChannelReadStatus status = execution->try_receive(event);
        if (status == ChannelReadStatus::value) {
            return ChannelReadStatus::value;
        }
        return is_stopped
            ? ChannelReadStatus::closed
            : ChannelReadStatus::empty;
    }

    void stop() {
        std::optional<BatchId> batch_id;
        std::optional<std::size_t> retained;
        {
            std::lock_guard lock(state_mutex);
            if (stopped) {
                return;
            }
            if (stopping) {
                return;
            }
            stopping = true;
            if (batch) {
                batch_id = batch->id;
                retained = batch->foreground;
            }
        }

        if (batch_id) {
            begin_abort_cleanup(*batch_id, retained);
            while (true) {
                std::unique_ptr<RunRecord> record;
                {
                    std::lock_guard lock(state_mutex);
                    if (!batch || batch->id != *batch_id) {
                        break;
                    }
                    for (std::size_t index = 0;
                         index < batch->runs.size();
                         ++index) {
                        if (retained == index) {
                            continue;
                        }
                        if (batch->runs[index]) {
                            record = std::move(batch->runs[index]);
                            break;
                        }
                    }
                }
                if (!record) {
                    break;
                }
                cleanup_record(std::move(record));
            }
        }

        std::shared_ptr<Execution> foreground_execution;
        std::thread foreground_thread;
        {
            std::lock_guard lock(state_mutex);
            if (batch_id && batch && batch->id == *batch_id
                && retained && *retained < batch->runs.size()
                && batch->runs[*retained]) {
                RunRecord& record = *batch->runs[*retained];
                foreground_execution = record.execution;
                if (!record.regular) {
                    foreground_thread = std::move(record.thread);
                }
            }
        }
        if (foreground_execution) {
            foreground_execution->request_cancel();
            foreground_execution->wait();
            if (foreground_thread.joinable()) {
                foreground_thread.join();
            }
        }
        regular.stop();

        bool discard_empty_batch = false;
        {
            std::lock_guard lock(state_mutex);
            stopped = true;
            discard_empty_batch =
                batch_id && batch && !batch->foreground;
        }
        if (discard_empty_batch) {
            retire_batch(*batch_id);
        }
        notifier.wake();
    }

    std::vector<std::unique_ptr<CompletionBackend>> backends;
    std::vector<AgentRuntimeInfo> runtime_info;
    std::vector<bool> leased;
    WakeNotifier& notifier;
    std::function<void()> before_stage_thread_start;
    RegularRunner regular;

    std::mutex state_mutex;
    std::optional<BatchRecord> batch;
    BatchId next_batch_id{1};
    bool stopping{};
    bool stopped{};
};

AgentRegistry::AgentRegistry(
    std::vector<AgentDefinition> definitions,
    WakeNotifier& notifier)
    : AgentRegistry(
          build_backends(std::move(definitions)),
          notifier,
          {}) {
}

AgentRegistry::AgentRegistry(
    std::vector<std::unique_ptr<CompletionBackend>> backends,
    WakeNotifier& notifier,
    StageThreadHook before_stage_thread_start)
    : impl_(std::make_unique<Impl>(
          std::move(backends),
          notifier,
          std::move(before_stage_thread_start))) {
}

AgentRegistry::~AgentRegistry() noexcept = default;

const std::vector<AgentRuntimeInfo>&
AgentRegistry::runtime_info() const noexcept {
    return impl_->runtime_info;
}

BatchId AgentRegistry::stage_batch(
    std::vector<CompletionInput> inputs) {
    return impl_->stage_batch(std::move(inputs));
}

void AgentRegistry::set_foreground(
    BatchId batch,
    std::size_t run_index) {
    impl_->set_foreground(batch, run_index);
}

void AgentRegistry::open_batch_gate(BatchId batch) noexcept {
    impl_->open_batch_gate(batch);
}

void AgentRegistry::retire(BatchId batch, std::size_t run_index) {
    impl_->retire(batch, run_index);
}

void AgentRegistry::retire_batch(BatchId batch) noexcept {
    impl_->retire_batch(batch);
}

void AgentRegistry::cancel_all() noexcept {
    impl_->cancel_all();
}

void AgentRegistry::discard_batch(BatchId batch) noexcept {
    impl_->discard_batch(batch);
}

void AgentRegistry::begin_abort_cleanup(
    BatchId batch,
    std::optional<std::size_t> retained_foreground) noexcept {
    impl_->begin_abort_cleanup(batch, retained_foreground);
}

void AgentRegistry::release_abort_foreground(
    BatchId batch,
    std::size_t run_index) noexcept {
    impl_->release_abort_foreground(batch, run_index);
}

CleanupStatus AgentRegistry::poll_abort_cleanup(
    BatchId batch) noexcept {
    return impl_->poll_abort_cleanup(batch);
}

ChannelReadStatus AgentRegistry::try_receive(AgentEvent& event) {
    return impl_->try_receive(event);
}

void AgentRegistry::stop() {
    impl_->stop();
}

} // namespace cha
