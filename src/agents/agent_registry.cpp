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
#include <unordered_map>
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

        ChannelReadStatus try_receive(AgentEvent& event) {
            const ChannelReadStatus status = events.try_get(event);
            if (status != ChannelReadStatus::closed) {
                return status;
            }
            std::lock_guard lock(terminal_mutex);
            if (!terminal) {
                return ChannelReadStatus::closed;
            }
            event = std::move(*terminal);
            terminal.reset();
            return ChannelReadStatus::value;
        }

        void publish_delta(AgentEvent event) {
            if (!events.push(std::move(event))) {
                throw std::logic_error(
                    "Agent event queue closed before execution stopped");
            }
            notifier.wake();
        }

        void publish_terminal(AgentEvent event) noexcept {
            {
                std::lock_guard lock(terminal_mutex);
                terminal.emplace(std::move(event));
            }
            events.close();
            notifier.wake();
        }

        void publish_failure(
            RequestId request_id,
            std::string_view message) noexcept {
            try {
                publish_terminal(AgentFailed{
                    request_id,
                    std::string(message),
                });
            } catch (...) {
                publish_terminal(std::move(fallback_terminal));
            }
        }

        void finish() noexcept {
            {
                std::lock_guard lock(done_mutex);
                done = true;
            }
            done_changed.notify_all();
        }

        CompletionInput input;
        CompletionBackend& backend;
        std::shared_ptr<StartGate> gate;
        WakeNotifier& notifier;
        ConcurrentQueue<AgentEvent> events;
        std::mutex terminal_mutex;
        std::optional<AgentEvent> terminal;
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
        RunId id{};
        BatchId batch_id{};
        std::size_t backend_index{};
        bool regular{};
        bool started{};
        std::shared_ptr<Execution> execution;
        std::thread thread;
    };

    struct CleanupState {
        std::mutex mutex;
        std::condition_variable changed;
        std::vector<std::unique_ptr<RunRecord>> jobs;
        std::size_t next_job{};
        bool active{};
        bool aborting{};
        bool waiting_for_foreground{};
        bool complete{};
        bool stopping{};
    };

    struct BatchRecord {
        BatchId id{};
        std::vector<RunId> run_ids;
        std::shared_ptr<StartGate> gate;
        std::shared_ptr<CleanupState> cleanup;
        std::thread cleanup_thread;
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
        std::vector<BatchId> ids;
        {
            std::lock_guard lock(state_mutex);
            ids.reserve(batches.size());
            for (const auto& [id, ignored] : batches) {
                (void)ignored;
                ids.push_back(id);
            }
        }
        for (const BatchId id : ids) {
            discard_batch(id);
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

    void cleanup_dialog(const std::shared_ptr<CleanupState>& cleanup) noexcept {
        while (true) {
            std::unique_ptr<RunRecord> record;
            {
                std::unique_lock lock(cleanup->mutex);
                cleanup->changed.wait(lock, [&cleanup] {
                    return cleanup->stopping
                        || cleanup->next_job < cleanup->jobs.size();
                });
                if (cleanup->next_job == cleanup->jobs.size()) {
                    if (cleanup->stopping) {
                        break;
                    }
                    continue;
                }
                cleanup->active = true;
                record = std::move(cleanup->jobs[cleanup->next_job++]);
            }

            cleanup_record(std::move(record));

            bool became_complete = false;
            {
                std::lock_guard lock(cleanup->mutex);
                cleanup->active = false;
                if (cleanup->aborting
                    && !cleanup->waiting_for_foreground
                    && cleanup->next_job == cleanup->jobs.size()
                    && !cleanup->complete) {
                    cleanup->complete = true;
                    became_complete = true;
                }
            }
            cleanup->changed.notify_all();
            if (became_complete) {
                notifier.wake();
            }
        }
    }

    void stop_cleanup(BatchRecord& batch) noexcept {
        {
            std::lock_guard lock(batch.cleanup->mutex);
            batch.cleanup->stopping = true;
        }
        batch.cleanup->changed.notify_all();
        if (batch.cleanup_thread.joinable()) {
            try {
                batch.cleanup_thread.join();
            } catch (...) {
                std::terminate();
            }
        }
    }

    void rollback_staging(
        std::unique_ptr<BatchRecord> batch,
        std::vector<std::unique_ptr<RunRecord>> records) noexcept {
        batch->gate->cancel();
        for (std::unique_ptr<RunRecord>& record : records) {
            if (record && record->started) {
                cleanup_record(std::move(record));
            } else if (record) {
                release_lease(record->backend_index);
            }
        }
        stop_cleanup(*batch);
    }

    StagedBatch stage_batch(std::vector<CompletionInput> inputs) {
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
        auto batch = std::make_unique<BatchRecord>();
        batch->id = batch_id;
        batch->gate = std::make_shared<StartGate>();
        batch->cleanup = std::make_shared<CleanupState>();
        batch->cleanup->jobs.reserve(inputs.size());
        batch->run_ids.reserve(inputs.size());

        std::vector<std::unique_ptr<RunRecord>> records;
        records.reserve(inputs.size());
        std::vector<RunRecord*> record_views;
        record_views.reserve(inputs.size());
        for (std::size_t index = 0; index < inputs.size(); ++index) {
            const RunId run_id = next_run_id++;
            auto record = std::make_unique<RunRecord>();
            record->id = run_id;
            record->batch_id = batch_id;
            record->backend_index = backend_indices[index];
            record->regular = index == 0;
            record->execution = std::make_shared<Execution>(
                std::move(inputs[index]),
                *backends[backend_indices[index]],
                batch->gate,
                notifier);
            batch->run_ids.push_back(run_id);
            record_views.push_back(record.get());
            records.push_back(std::move(record));
        }
        StagedBatch staged{batch_id, batch->run_ids};
        BatchRecord* const batch_view = batch.get();

        std::vector<std::size_t> claimed;
        claimed.reserve(backend_indices.size());
        std::unique_lock state_lock(state_mutex);
        if (stopping || stopped || !batches.empty()) {
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

        // Allocate every ownership-map node before launching a worker. Once
        // the unique_ptr assignments below occur, registration cannot throw.
        try {
            if (!batches.try_emplace(batch_id, nullptr).second) {
                throw std::logic_error("Duplicate staged batch ID");
            }
            for (const RunId run_id : batch->run_ids) {
                if (!runs.try_emplace(run_id, nullptr).second) {
                    throw std::logic_error("Duplicate staged run ID");
                }
            }
        } catch (...) {
            batches.erase(batch_id);
            for (const RunId run_id : batch->run_ids) {
                runs.erase(run_id);
            }
            for (const std::size_t index : claimed) {
                leased[index] = false;
            }
            throw;
        }

        batches.find(batch_id)->second = std::move(batch);
        for (std::size_t index = 0; index < records.size(); ++index) {
            runs.find(record_views[index]->id)->second =
                std::move(records[index]);
        }

        try {
            if (before_stage_thread_start) {
                before_stage_thread_start();
            }
            batch_view->cleanup_thread = std::thread(
                [this, cleanup = batch_view->cleanup] {
                    cleanup_dialog(cleanup);
                });
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
            batch_view->gate->wait_until_parked(record_views.size());
        } catch (...) {
            batch = std::move(batches.find(batch_id)->second);
            batches.erase(batch_id);
            for (std::size_t index = 0; index < record_views.size(); ++index) {
                const RunId run_id = batch->run_ids[index];
                records[index] = std::move(runs.find(run_id)->second);
                runs.erase(run_id);
            }
            state_lock.unlock();
            rollback_staging(
                std::move(batch),
                std::move(records));
            throw;
        }

        return staged;
    }

    void set_foreground(RunId run_id) {
        std::lock_guard lock(state_mutex);
        if (stopped || !runs.contains(run_id)) {
            throw std::logic_error(
                "Foreground run is not live in the agent registry");
        }
        foreground = run_id;
    }

    void open_batch_gate(BatchId batch_id) noexcept {
        std::shared_ptr<StartGate> gate;
        {
            std::lock_guard lock(state_mutex);
            if (const auto found = batches.find(batch_id);
                found != batches.end()) {
                gate = found->second->gate;
            }
        }
        if (gate) {
            gate->open();
        }
    }

    void retire(RunId run_id) {
        std::unique_ptr<RunRecord> record;
        {
            std::lock_guard lock(state_mutex);
            const auto found = runs.find(run_id);
            if (found == runs.end()) {
                return;
            }
            if (foreground == run_id) {
                foreground.reset();
            }
            record = std::move(found->second);
            runs.erase(found);
        }
        cleanup_record(std::move(record));
    }

    void retire_batch(BatchId batch_id) noexcept {
        std::unique_ptr<BatchRecord> batch;
        {
            std::lock_guard lock(state_mutex);
            const auto found = batches.find(batch_id);
            if (found == batches.end()) {
                return;
            }
            for (const auto& [ignored, run] : runs) {
                (void)ignored;
                if (run->batch_id == batch_id) {
                    return;
                }
            }
            batch = std::move(found->second);
            batches.erase(found);
        }
        stop_cleanup(*batch);
    }

    void cancel_all() noexcept {
        std::lock_guard lock(state_mutex);
        for (const auto& [ignored, run] : runs) {
            (void)ignored;
            run->execution->request_cancel();
        }
    }

    void discard_batch(BatchId batch_id) noexcept {
        std::unique_ptr<BatchRecord> batch;
        std::vector<std::unique_ptr<RunRecord>> records;
        {
            std::lock_guard lock(state_mutex);
            const auto found = batches.find(batch_id);
            if (found == batches.end()) {
                return;
            }
            batch = std::move(found->second);
            batches.erase(found);
            records.reserve(batch->run_ids.size());
            for (const RunId run_id : batch->run_ids) {
                const auto run = runs.find(run_id);
                if (run != runs.end()) {
                    records.push_back(std::move(run->second));
                    runs.erase(run);
                }
                if (foreground == run_id) {
                    foreground.reset();
                }
            }
        }
        batch->gate->cancel();
        for (std::unique_ptr<RunRecord>& record : records) {
            cleanup_record(std::move(record));
        }
        stop_cleanup(*batch);
    }

    void begin_abort_cleanup(
        BatchId batch_id,
        std::optional<RunId> retained_foreground) noexcept {
        cancel_all();
        std::shared_ptr<CleanupState> cleanup;
        bool became_complete = false;
        {
            std::lock_guard lock(state_mutex);
            const auto found = batches.find(batch_id);
            if (found == batches.end()) {
                return;
            }
            found->second->gate->cancel();
            cleanup = found->second->cleanup;
            std::lock_guard cleanup_lock(cleanup->mutex);
            cleanup->aborting = true;
            cleanup->waiting_for_foreground =
                retained_foreground.has_value();
            for (const RunId run_id : found->second->run_ids) {
                if (retained_foreground == run_id) {
                    continue;
                }
                const auto run = runs.find(run_id);
                if (run != runs.end()) {
                    cleanup->jobs.push_back(std::move(run->second));
                    runs.erase(run);
                }
            }
            if (!cleanup->waiting_for_foreground
                && cleanup->next_job == cleanup->jobs.size()
                && !cleanup->active
                && !cleanup->complete) {
                cleanup->complete = true;
                became_complete = true;
            }
        }
        cleanup->changed.notify_all();
        if (became_complete) {
            notifier.wake();
        }
    }

    void release_foreground_to_cleanup(RunId run_id) noexcept {
        std::shared_ptr<CleanupState> cleanup;
        bool became_complete = false;
        {
            std::lock_guard lock(state_mutex);
            const auto found = runs.find(run_id);
            if (found == runs.end()) {
                return;
            }
            const auto batch = batches.find(found->second->batch_id);
            if (batch == batches.end()) {
                return;
            }
            cleanup = batch->second->cleanup;
            {
                std::lock_guard cleanup_lock(cleanup->mutex);
                cleanup->jobs.push_back(std::move(found->second));
                cleanup->waiting_for_foreground = false;
                cleanup->complete = false;
                if (cleanup->next_job == cleanup->jobs.size()
                    && !cleanup->active) {
                    cleanup->complete = true;
                    became_complete = true;
                }
            }
            runs.erase(found);
            if (foreground == run_id) {
                foreground.reset();
            }
        }
        cleanup->changed.notify_all();
        if (became_complete) {
            notifier.wake();
        }
    }

    CleanupStatus poll_abort_cleanup(BatchId batch_id) noexcept {
        std::shared_ptr<CleanupState> cleanup;
        {
            std::lock_guard lock(state_mutex);
            const auto found = batches.find(batch_id);
            if (found == batches.end()) {
                return CleanupStatus::none;
            }
            cleanup = found->second->cleanup;
        }
        {
            std::lock_guard lock(cleanup->mutex);
            if (!cleanup->aborting) {
                return CleanupStatus::none;
            }
            if (!cleanup->complete) {
                return CleanupStatus::pending;
            }
        }
        retire_batch(batch_id);
        return CleanupStatus::complete;
    }

    ChannelReadStatus try_receive(AgentEvent& event) {
        std::shared_ptr<Execution> execution;
        std::optional<RunId> current;
        bool is_stopped = false;
        {
            std::lock_guard lock(state_mutex);
            current = foreground;
            is_stopped = stopped;
            if (current) {
                const auto found = runs.find(*current);
                if (found != runs.end()) {
                    execution = found->second->execution;
                }
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
        std::optional<RunId> retained;
        {
            std::lock_guard lock(state_mutex);
            if (stopped) {
                return;
            }
            if (stopping) {
                return;
            }
            stopping = true;
            retained = foreground;
            if (!batches.empty()) {
                batch_id = batches.begin()->first;
            }
        }

        cancel_all();
        if (batch_id) {
            begin_abort_cleanup(*batch_id, retained);
            std::shared_ptr<CleanupState> cleanup;
            BatchRecord* batch = nullptr;
            {
                std::lock_guard lock(state_mutex);
                const auto found = batches.find(*batch_id);
                if (found != batches.end()) {
                    batch = found->second.get();
                    cleanup = batch->cleanup;
                }
            }
            if (cleanup) {
                {
                    std::lock_guard lock(cleanup->mutex);
                    cleanup->waiting_for_foreground = false;
                    if (cleanup->next_job == cleanup->jobs.size()
                        && !cleanup->active) {
                        cleanup->complete = true;
                    }
                }
                cleanup->changed.notify_all();
                {
                    std::unique_lock lock(cleanup->mutex);
                    cleanup->changed.wait(lock, [&cleanup] {
                        return cleanup->complete;
                    });
                }
                if (batch) {
                    stop_cleanup(*batch);
                }
            }
        }

        std::shared_ptr<Execution> foreground_execution;
        RunRecord* foreground_record = nullptr;
        {
            std::lock_guard lock(state_mutex);
            if (retained) {
                const auto found = runs.find(*retained);
                if (found != runs.end()) {
                    foreground_record = found->second.get();
                    foreground_execution = foreground_record->execution;
                }
            }
        }
        if (foreground_execution) {
            foreground_execution->request_cancel();
            foreground_execution->wait();
            if (foreground_record && !foreground_record->regular
                && foreground_record->thread.joinable()) {
                foreground_record->thread.join();
            }
        }
        regular.stop();

        bool discard_empty_batch = false;
        {
            std::lock_guard lock(state_mutex);
            stopped = true;
            discard_empty_batch = batch_id && !foreground;
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
    std::unordered_map<RunId, std::unique_ptr<RunRecord>> runs;
    std::unordered_map<BatchId, std::unique_ptr<BatchRecord>> batches;
    std::optional<RunId> foreground;
    BatchId next_batch_id{1};
    RunId next_run_id{1};
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

StagedBatch AgentRegistry::stage_batch(
    std::vector<CompletionInput> inputs) {
    return impl_->stage_batch(std::move(inputs));
}

void AgentRegistry::set_foreground(RunId run) {
    impl_->set_foreground(run);
}

void AgentRegistry::open_batch_gate(BatchId batch) noexcept {
    impl_->open_batch_gate(batch);
}

void AgentRegistry::retire(RunId run) {
    impl_->retire(run);
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
    std::optional<RunId> retained_foreground) noexcept {
    impl_->begin_abort_cleanup(batch, retained_foreground);
}

void AgentRegistry::release_foreground_to_cleanup(RunId run) noexcept {
    impl_->release_foreground_to_cleanup(run);
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
