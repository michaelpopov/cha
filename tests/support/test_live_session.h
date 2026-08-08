#pragma once

#include "agents/completion_backend.h"
#include "agents/config.h"
#include "agents/persona.h"
#include "session/opened_session.h"
#include "session/session_controller.h"
#include "session/session_database.h"
#include "session/session_identity.h"
#include "session/session_lease.h"
#include "support/test_backends.h"
#include "support/test_controller.h"
#include "util/wake_notifier.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace cha::test {

// One temporary session database plus the companion file its lease uses.
class TemporarySessionFile {
public:
    explicit TemporarySessionFile(
        std::string tag = "live_session",
        SessionIdentity identity = {"forum", "session"})
        : path_(std::filesystem::temp_directory_path()
                / ("cha_" + std::move(tag) + "_"
                   + std::to_string(std::chrono::steady_clock::now()
                                        .time_since_epoch()
                                        .count())
                   + "_" + std::to_string(next_serial()) + ".sqlite3")) {
        if (!create_session_database(
                path_,
                {identity.session_id, identity.forum_id, "Test session"})) {
            throw std::runtime_error("Failed to create test session database");
        }
    }
    ~TemporarySessionFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(SessionLease::companion_path(path_), ignored);
    }
    TemporarySessionFile(const TemporarySessionFile&) = delete;
    TemporarySessionFile& operator=(const TemporarySessionFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    static int next_serial() {
        static std::atomic<int> value{0};
        return ++value;
    }
    std::filesystem::path path_;
};

// Cross-thread controls for one scripted completion backend. Tests hold it
// through a shared pointer because the backend runs inside the controller's
// worker pool and outlives any single call.
class BackendControls {
public:
    using Clock = std::chrono::steady_clock;

    [[nodiscard]] bool wait_until_running(
        std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [this] { return running_; });
    }
    [[nodiscard]] bool wait_until_idle(
        std::chrono::milliseconds timeout = std::chrono::seconds(2)) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [this] { return !running_; });
    }

    void emit_answer(std::string text) {
        emit({CompletionDeltaKind::answer, std::move(text)});
    }
    void emit_reasoning(std::string text) {
        emit({CompletionDeltaKind::reasoning, std::move(text)});
    }
    void emit(CompletionDelta delta) {
        {
            std::lock_guard lock(mutex_);
            queued_.push_back(std::move(delta));
        }
        changed_.notify_all();
    }

    // Ends the running completion normally, or with a transport failure.
    void finish(CompletionResult result = {}) {
        {
            std::lock_guard lock(mutex_);
            ending_ = std::move(result);
        }
        changed_.notify_all();
    }
    void fail(std::string message) {
        finish({CompletionOutcome::transport_error, std::move(message)});
    }

    // Makes the backend ignore the controller's cancellation flag. An
    // in-flight completion then blocks SessionController::shutdown() until
    // finish() is called, which is how a wedged owner is produced without any
    // controller or lifecycle fake.
    void ignore_cancellation() {
        std::lock_guard lock(mutex_);
        ignore_cancellation_ = true;
    }
    [[nodiscard]] bool cancellation_observed() const {
        std::lock_guard lock(mutex_);
        return cancellation_observed_;
    }
    [[nodiscard]] std::size_t runs() const {
        std::lock_guard lock(mutex_);
        return runs_;
    }

private:
    friend class ScriptedCompletionBackend;

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<CompletionDelta> queued_;
    std::optional<CompletionResult> ending_;
    bool running_{};
    bool ignore_cancellation_{};
    bool cancellation_observed_{};
    std::size_t runs_{};
};

// A completion backend whose whole behavior is decided by the test through
// BackendControls. It is a provider fake, not a controller or output fake: the
// controller, journal, worker pool, and web actor around it are all real.
class ScriptedCompletionBackend final : public CompletionBackend {
public:
    ScriptedCompletionBackend(
        std::shared_ptr<BackendControls> controls,
        std::string id,
        std::string name)
        : controls_(std::move(controls)),
          id_(std::move(id)),
          name_(std::move(name)) {}

    RequestPayload prepare(const CompletionInput& input) override {
        return {.bytes = input.run.prompt_text};
    }

    CompletionResult perform(
        RequestPayload,
        const CompletionDeltaSink& on_delta,
        const std::atomic_bool& cancellation) override {
        {
            std::lock_guard lock(controls_->mutex_);
            controls_->running_ = true;
            ++controls_->runs_;
        }
        controls_->changed_.notify_all();
        while (true) {
            std::vector<CompletionDelta> batch;
            std::optional<CompletionResult> ending;
            {
                std::unique_lock lock(controls_->mutex_);
                // The cancellation flag is not a condition variable, so this
                // wait is bounded and rechecks it on every wake.
                controls_->changed_.wait_for(lock, std::chrono::milliseconds(5), [this] {
                    return !controls_->queued_.empty()
                        || controls_->ending_.has_value();
                });
                if (!controls_->ignore_cancellation_
                    && cancellation.load(std::memory_order_acquire)) {
                    controls_->cancellation_observed_ = true;
                    controls_->running_ = false;
                    controls_->queued_.clear();
                    controls_->ending_.reset();
                    lock.unlock();
                    controls_->changed_.notify_all();
                    return {CompletionOutcome::cancelled, {}};
                }
                batch.swap(controls_->queued_);
                ending = controls_->ending_;
            }
            for (const CompletionDelta& delta : batch) on_delta(delta);
            if (!ending) continue;
            {
                std::lock_guard lock(controls_->mutex_);
                controls_->ending_.reset();
                controls_->running_ = false;
            }
            controls_->changed_.notify_all();
            return *ending;
        }
    }

    AgentRuntimeInfo info() const override {
        return {
            .character = {.id = id_, .name = name_},
            .model = "test-model",
            .api = "test://completion",
            .streaming = true,
        };
    }

private:
    std::shared_ptr<BackendControls> controls_;
    std::string id_;
    std::string name_;
};

inline std::unique_ptr<CompletionBackend> scripted_backend(
    std::shared_ptr<BackendControls> controls,
    std::string id = "guide",
    std::string name = "Guide") {
    return std::make_unique<ScriptedCompletionBackend>(
        std::move(controls), std::move(id), std::move(name));
}

inline PersonaRoster reader_roster() {
    return {{.id = "reader", .display_name = "Reader"}};
}

inline SessionDescriptor test_descriptor(const SessionIdentity& identity) {
    return {
        .identity = identity,
        .forum_display_name = "Test forum " + identity.forum_id,
        .session_label = "Test session " + identity.session_id,
    };
}

// A real controller driven by controlled completion backends. The controller's
// backend seam does not take a session lease, so lease behavior belongs to
// open_leased_session() below rather than to a second controller abstraction.
inline OpenedSession open_scripted_session(
    const SessionIdentity& identity,
    const std::filesystem::path& database_path,
    WakeNotifier& notifier,
    std::vector<std::unique_ptr<CompletionBackend>> backends,
    SessionController::ActivationHook before_activation = {}) {
    return {
        .descriptor = test_descriptor(identity),
        .controller = from_backends_for_testing(
            std::move(backends),
            reader_roster(),
            database_path,
            notifier,
            load_session_state(database_path),
            std::move(before_activation)),
    };
}

inline OpenedSession open_scripted_session(
    const SessionIdentity& identity,
    const std::filesystem::path& database_path,
    WakeNotifier& notifier,
    std::shared_ptr<BackendControls> controls,
    SessionController::ActivationHook before_activation = {}) {
    return open_scripted_session(
        identity,
        database_path,
        notifier,
        one_backend(scripted_backend(std::move(controls))),
        std::move(before_activation));
}

// A real controller restored from a supplied transcript rather than from the
// database, for tests that need a specific starting projection.
inline OpenedSession open_restored_session(
    const SessionIdentity& identity,
    const std::filesystem::path& database_path,
    WakeNotifier& notifier,
    SessionRestore restored,
    std::shared_ptr<BackendControls> controls) {
    return {
        .descriptor = test_descriptor(identity),
        .controller = from_backends_for_testing(
            one_backend(scripted_backend(std::move(controls))),
            reader_roster(),
            database_path,
            notifier,
            std::move(restored)),
    };
}

// One character configured for a port nothing listens on. A session opened
// this way is quiescent: it never generates on its own, which is what
// liveness, capacity, and lease tests want.
inline AgentDefinition unreachable_definition(
    std::string id = "guide",
    std::string name = "Guide") {
    return {
        .config = {
            .id = std::move(id),
            .display_name = std::move(name),
            .host = "127.0.0.1",
            .port = 1,
        },
        .system_prompt = "Test prompt",
    };
}

// A real controller holding the production cross-process session lease. This
// is the same construction path production uses, so releasing the controller
// releases the lease.
inline OpenedSession open_leased_session(
    const SessionIdentity& identity,
    const std::filesystem::path& database_path,
    WakeNotifier& notifier) {
    SessionLease lease = SessionLease::acquire(database_path);
    SessionRestore restored = load_session_state(database_path);
    return {
        .descriptor = test_descriptor(identity),
        .controller = SessionController::from_shared_definitions(
            {unreachable_definition()},
            std::make_shared<const PersonaRoster>(reader_roster()),
            "guide",
            database_path,
            std::move(lease),
            notifier,
            std::move(restored)),
    };
}

} // namespace cha::test
