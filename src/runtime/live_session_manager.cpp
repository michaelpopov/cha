#include "runtime/live_session_manager.h"

#include "storage/not_found_error.h"
#include "util/logging.h"
#include "util/owner_wake_signal.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <future>
#include <map>
#include <mutex>
#include <new>
#include <set>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace cha {
namespace {

std::string session_log(const FullSessionId& key, std::string_view event) {
    return "web session forum_id=" + key.forum_id + " session_id=" + key.session_id
        + " event=" + std::string(event);
}

template<typename T>
class RuntimeReply {
public:
    bool complete(T value) {
        {
            std::lock_guard lock(mutex_);
            if (value_ || abandoned_) return false;
            value_ = std::move(value);
        }
        changed_.notify_all();
        return true;
    }

    std::optional<T> wait_for(
        std::chrono::milliseconds timeout,
        std::stop_token stopping) {
        return wait_until(std::chrono::steady_clock::now() + timeout, stopping);
    }

    std::optional<T> wait_until(
        std::chrono::steady_clock::time_point deadline,
        std::stop_token stopping) {
        std::unique_lock lock(mutex_);
        while (!value_ && !stopping.stop_requested()) {
            if (claimed_) {
                if (!changed_.wait(lock, stopping, [this] {
                        return value_.has_value();
                    })) break;
            } else if (!changed_.wait_until(lock, stopping, deadline, [this] {
                           return value_.has_value() || claimed_;
                       })) {
                break;
            }
        }
        if (!value_) {
            abandoned_ = true;
            return std::nullopt;
        }
        return std::move(value_);
    }

    std::optional<T> wait(std::stop_token stopping) {
        std::unique_lock lock(mutex_);
        (void)changed_.wait(lock, stopping, [this] { return value_.has_value(); });
        if (!value_) {
            abandoned_ = true;
            return std::nullopt;
        }
        return std::move(*value_);
    }

    [[nodiscard]] bool claim() {
        {
            std::lock_guard lock(mutex_);
            if (abandoned_ || value_) return false;
            claimed_ = true;
        }
        changed_.notify_all();
        return true;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable_any changed_;
    std::optional<T> value_;
    bool abandoned_{};
    bool claimed_{};
};

} // namespace

struct SessionRuntime::Impl {
    struct WebWork {
        FullSessionId identity;
        std::uint64_t instance{};
        std::uint64_t context_epoch{};
        OwnerCommand command;
    };

    struct Work {
        std::optional<WebWork> web;
        std::function<void()> control;
    };

    Impl(
        SessionRuntime& owner,
        RuntimeSettings settings_value,
        SessionOpener opener_value,
        LiveSessionClock clock_value)
        : owner(owner),
          settings(validate_live_session_settings(std::move(settings_value))),
          opener(std::move(opener_value)),
          notifier(std::make_shared<OwnerWakeSignal>()) {
        (void)clock_value;
        if (!opener) {
            throw std::invalid_argument("Session runtime needs a session opener");
        }
        if (settings.session_limit == 0) {
            throw std::invalid_argument("Session runtime limit must be positive");
        }
    }

    void start(std::weak_ptr<SessionRuntime> runtime_value) {
        runtime = std::move(runtime_value);
        thread = std::thread([this] { run(); });
    }

    bool enqueue_control_until(
        std::function<void()> control,
        std::chrono::steady_clock::time_point deadline) {
        if (thread_finished.load()) return false;
        {
            std::unique_lock lock(queue_mutex);
            if (!queue_changed.wait_until(lock, deadline, [this] {
                    return thread_finished.load() || stopping_requested.load()
                        || work.size() < settings.command_queue_capacity;
                })) return false;
            if (thread_finished.load() || stopping_requested.load()) return false;
            work.push_back({.control = std::move(control)});
        }
        notifier->wake();
        return true;
    }

    bool enqueue_control(std::function<void()> control) {
        return enqueue_control_until(
            std::move(control), std::chrono::steady_clock::time_point::max());
    }

    std::variant<std::shared_ptr<CommandReply>, ErrorCode> enqueue_web(
        const FullSessionId& identity,
        std::uint64_t instance,
        WebCommand command,
        std::uint64_t subscribe_ticket) {
        if (stopping_requested.load()) return ErrorCode::server_stopping;
        auto reply = std::make_shared<CommandReply>();
        {
            std::lock_guard lock(queue_mutex);
            if (stopping_requested.load()) return ErrorCode::server_stopping;
            if (work.size() >= settings.command_queue_capacity) {
                return ErrorCode::command_queue_full;
            }
            work.push_back({.web = WebWork{
                identity,
                instance,
                context_epoch.load(),
                OwnerCommand{std::move(command), reply, subscribe_ticket}}});
        }
        notifier->wake();
        return reply;
    }

    std::optional<Work> pop() {
        std::lock_guard lock(queue_mutex);
        if (work.empty()) return std::nullopt;
        Work next = std::move(work.front());
        work.pop_front();
        queue_changed.notify_all();
        return next;
    }

    std::shared_ptr<LiveSession> create_session(const FullSessionId& identity) {
        auto session = owner.make_session(identity, next_instance++);
        OpenedSession opened = opener(identity, notifier);
        session->install(std::move(opened));
        return session;
    }

    void publish_live(const FullSessionId& identity) {
        std::lock_guard lock(state_mutex);
        published_live.insert(identity);
    }

    void publish_released(const FullSessionId& identity) noexcept {
        try {
            {
                std::lock_guard lock(state_mutex);
                published_live.erase(identity);
            }
            state_changed.notify_all();
        } catch (...) {
            std::terminate();
        }
    }

    std::vector<FullSessionId> published_identities() {
        std::lock_guard lock(state_mutex);
        return {published_live.begin(), published_live.end()};
    }

    LiveSessionOpenResult open_now(const FullSessionId& key, std::uint64_t epoch) {
        if (stopping_requested.load() || global_maintenance || epoch != context_epoch.load()) {
            return LiveSessionOpenFailure::manager_stopping;
        }
        if (maintenance.contains(key)) return LiveSessionOpenFailure::stopping;
        if (const auto found = sessions.find(key); found != sessions.end()) {
            return found->second->lifecycle() == LiveSessionState::running
                ? LiveSessionOpenResult{LiveSessionReady{}}
                : LiveSessionOpenResult{LiveSessionOpenFailure::stopping};
        }
        if (sessions.size() >= settings.session_limit) {
            return LiveSessionOpenFailure::limit_reached;
        }
        publish_live(key);
        try {
            auto session = create_session(key);
            if (stopping_requested.load() || global_maintenance || epoch != context_epoch.load()
                || maintenance.contains(key)) {
                session->finalize(stopping_requested.load()
                    ? ShutdownReason::server_stopping
                    : ShutdownReason::reloading);
                publish_released(key);
                return stopping_requested.load() || epoch != context_epoch.load()
                    ? LiveSessionOpenResult{LiveSessionOpenFailure::manager_stopping}
                    : LiveSessionOpenResult{LiveSessionOpenFailure::stopping};
            }
            session->set_running();
            sessions.emplace(key, std::move(session));
            log_info(session_log(key, "registry_running"));
            return LiveSessionReady{};
        } catch (const std::bad_alloc&) {
            std::terminate();
        } catch (const SessionNotFoundError&) {
            publish_released(key);
            log_warn(session_log(key, "storage_not_found"));
            return LiveSessionOpenFailure::not_found;
        } catch (const ForumNotFoundError&) {
            publish_released(key);
            log_warn(session_log(key, "storage_not_found"));
            return LiveSessionOpenFailure::not_found;
        } catch (...) {
            publish_released(key);
            log_error(session_log(key, "startup_failed"));
            return LiveSessionOpenFailure::internal_error;
        }
    }

    void retire(std::map<FullSessionId, LiveSessionHandle, std::less<>>::iterator it,
                ShutdownReason reason) {
        const FullSessionId identity = it->first;
        auto session = std::move(it->second);
        if (selected && *selected == identity) selected.reset();
        sessions.erase(it);
        session->finalize(reason);
        publish_released(identity);
        log_info(session_log(identity, "registry_retired"));
    }

    void cleanup_retired() {
        for (auto it = sessions.begin(); it != sessions.end();) {
            const bool explicit_stop = it->second->shutdown_requested();
            const bool idle_retirement = it->second->retirement_requested()
                && (!selected || *selected != it->first);
            if (!explicit_stop && !idle_retirement) {
                ++it;
                continue;
            }
            const auto doomed = it++;
            const ShutdownReason reason = explicit_stop
                ? doomed->second->shutdown_reason()
                : ShutdownReason::retired;
            retire(doomed, reason);
        }
    }

    void execute_web(WebWork web) {
        if (stopping_requested.load()) {
            (void)web.command.reply->complete(ErrorCode::server_stopping);
            return;
        }
        if (web.context_epoch != context_epoch.load()) {
            (void)web.command.reply->complete(ErrorCode::vault_changed);
            return;
        }
        const auto found = sessions.find(web.identity);
        if (found == sessions.end() || found->second->instance_ != web.instance
            || found->second->lifecycle() != LiveSessionState::running) {
            (void)web.command.reply->complete(stopping_requested.load()
                ? ErrorCode::server_stopping
                : ErrorCode::session_not_live);
            return;
        }
        const auto reply = web.command.reply;
        try {
            found->second->execute(std::move(web.command));
        } catch (const std::bad_alloc&) {
            std::terminate();
        } catch (...) {
            found->second->fail_current(reply);
        }
    }

    void drain_shutdown_queue() {
        while (auto next = pop()) {
            if (next->web) {
                (void)next->web->command.reply->complete(ErrorCode::server_stopping);
            } else if (next->control) {
                next->control();
            }
        }
    }

    void finish_shutdown() {
        for (auto it = sessions.begin(); it != sessions.end();) {
            const auto doomed = it++;
            retire(doomed, ShutdownReason::server_stopping);
        }
        drain_shutdown_queue();
        {
            std::lock_guard lock(state_mutex);
            thread_finished.store(true);
        }
        queue_changed.notify_all();
        state_changed.notify_all();
    }

    void cleanup_maintenance() {
        std::erase_if(maintenance, [](const auto& entry) {
            return !entry.second->load();
        });
        if (global_maintenance && !global_maintenance->load()) {
            global_maintenance.reset();
        }
    }

    void run() noexcept {
        try {
            for (;;) {
                std::size_t processed = 0;
                while (processed < settings.command_batch_size
                    && !stopping_requested.load()) {
                    auto next = pop();
                    cleanup_maintenance();
                    if (!next) break;
                    if (next->web) execute_web(std::move(*next->web));
                    else next->control();
                    ++processed;
                }

                if (stopping_requested.load()) {
                    finish_shutdown();
                    return;
                }

                bool more_events = false;
                for (auto& [identity, session] : sessions) {
                    (void)identity;
                    if (session->lifecycle() != LiveSessionState::running) continue;
                    try {
                        more_events |= session->receive_events(settings.event_batch_size);
                    } catch (const std::bad_alloc&) {
                        std::terminate();
                    } catch (...) {
                        session->fail_current();
                    }
                }
                cleanup_retired();

                if (processed == settings.command_batch_size || more_events) continue;
                (void)notifier->wait_until(
                    std::chrono::steady_clock::time_point::max());
            }
        } catch (...) {
            stopping_requested.store(true);
            finish_shutdown();
        }
    }

    SessionRuntime& owner;
    RuntimeSettings settings;
    SessionOpener opener;
    std::weak_ptr<SessionRuntime> runtime;
    std::shared_ptr<OwnerWakeSignal> notifier;

    std::mutex queue_mutex;
    std::condition_variable queue_changed;
    std::deque<Work> work;
    std::thread thread;
    std::atomic<bool> stopping_requested{};
    // Publishing a recovered context must not wait for a stalled controller.
    std::atomic<std::uint64_t> context_epoch{1};
    std::stop_source stop_source;
    std::atomic<bool> thread_finished{};
    std::mutex state_mutex;
    std::condition_variable state_changed;
    std::set<FullSessionId, std::less<>> published_live;

    // Runtime-thread only.
    std::map<FullSessionId, LiveSessionHandle, std::less<>> sessions;
    // Releasing a reservation only cancels its token and wakes the runtime;
    // it never waits for room in the ordinary command queue.
    std::map<FullSessionId, std::shared_ptr<std::atomic_bool>, std::less<>> maintenance;
    std::optional<FullSessionId> selected;
    std::uint64_t next_instance{1};
    std::shared_ptr<std::atomic_bool> global_maintenance;
};

SessionRuntime::SessionRuntime(
    RuntimeSettings settings,
    SessionOpener opener,
    LiveSessionClock clock)
    : impl_(std::make_unique<Impl>(
          *this, std::move(settings), std::move(opener), std::move(clock))) {}

SessionRuntime::~SessionRuntime() {
    impl_->stopping_requested.store(true);
    impl_->notifier->wake();
    if (impl_->thread.joinable()) impl_->thread.join();
}

std::shared_ptr<LiveSession> SessionRuntime::make_session(
    FullSessionId identity,
    std::uint64_t instance) {
    return std::shared_ptr<LiveSession>(new LiveSession(
        std::move(identity),
        instance,
        impl_->runtime,
        impl_->settings.pending_append_byte_limit));
}

std::variant<std::shared_ptr<CommandReply>, ErrorCode> SessionRuntime::enqueue(
    const FullSessionId& identity,
    std::uint64_t instance,
    WebCommand command,
    std::uint64_t subscribe_ticket) {
    return impl_->enqueue_web(
        identity, instance, std::move(command), subscribe_ticket);
}

void SessionRuntime::wake() noexcept {
    impl_->notifier->wake();
}

LiveSessionMaintenanceReservation::LiveSessionMaintenanceReservation(
    LiveSessionManager& manager,
    std::shared_ptr<std::atomic_bool> active)
    : manager_(&manager), active_(std::move(active)) {}

LiveSessionMaintenanceReservation::~LiveSessionMaintenanceReservation() {
    release();
}

LiveSessionMaintenanceReservation::LiveSessionMaintenanceReservation(
    LiveSessionMaintenanceReservation&& other) noexcept
    : manager_(std::exchange(other.manager_, nullptr)),
      active_(std::move(other.active_)) {}

LiveSessionMaintenanceReservation&
LiveSessionMaintenanceReservation::operator=(
    LiveSessionMaintenanceReservation&& other) noexcept {
    if (this != &other) {
        release();
        manager_ = std::exchange(other.manager_, nullptr);
        active_ = std::move(other.active_);
    }
    return *this;
}

void LiveSessionMaintenanceReservation::release() noexcept {
    if (LiveSessionManager* const manager = std::exchange(manager_, nullptr)) {
        manager->release_maintenance(active_);
    }
}

LiveSessionGlobalMaintenance::LiveSessionGlobalMaintenance(
    LiveSessionManager& manager,
    std::shared_ptr<std::atomic_bool> active)
    : manager_(&manager), active_(std::move(active)) {}

LiveSessionGlobalMaintenance::~LiveSessionGlobalMaintenance() {
    release();
}

LiveSessionGlobalMaintenance::LiveSessionGlobalMaintenance(
    LiveSessionGlobalMaintenance&& other) noexcept
    : manager_(std::exchange(other.manager_, nullptr)),
      active_(std::move(other.active_)) {}

LiveSessionGlobalMaintenance& LiveSessionGlobalMaintenance::operator=(
    LiveSessionGlobalMaintenance&& other) noexcept {
    if (this != &other) {
        release();
        manager_ = std::exchange(other.manager_, nullptr);
        active_ = std::move(other.active_);
    }
    return *this;
}

void LiveSessionGlobalMaintenance::release() noexcept {
    if (LiveSessionManager* const manager = std::exchange(manager_, nullptr)) {
        manager->release_maintenance(active_);
    }
}

LiveSessionManager::LiveSessionManager(
    RuntimeSettings settings,
    SessionOpener opener,
    LiveSessionClock clock)
    : runtime_(std::shared_ptr<SessionRuntime>(new SessionRuntime(
          std::move(settings), std::move(opener), std::move(clock)))) {
    runtime_->impl_->start(runtime_);
}

LiveSessionManager::~LiveSessionManager() {
    begin_shutdown();
    if (!join_shutdown(runtime_->impl_->settings.shutdown_grace)) {
        // The process coordinator owns the forced-exit path. Do not turn its
        // bounded grace period into an unconditional thread join here.
        (void)new std::shared_ptr<SessionRuntime>(std::move(runtime_));
    }
}

LiveSessionOpenResult LiveSessionManager::open(
    FullSessionId key,
    std::chrono::milliseconds deadline) {
    auto& impl = *runtime_->impl_;
    const auto epoch = impl.context_epoch.load();
    const auto absolute_deadline = std::chrono::steady_clock::now() + deadline;
    if (impl.stopping_requested.load()) {
        return LiveSessionOpenFailure::manager_stopping;
    }
    auto reply = std::make_shared<RuntimeReply<LiveSessionOpenResult>>();
    if (!impl.enqueue_control_until([&impl, key = std::move(key), reply, epoch] {
            (void)reply->complete(impl.open_now(key, epoch));
        }, absolute_deadline)) {
        return impl.stopping_requested.load()
            ? LiveSessionOpenResult{LiveSessionOpenFailure::manager_stopping}
            : LiveSessionOpenResult{LiveSessionOpenFailure::open_timeout};
    }
    const auto remaining = std::max(
        std::chrono::milliseconds{0},
        std::chrono::duration_cast<std::chrono::milliseconds>(
            absolute_deadline - std::chrono::steady_clock::now()));
    auto result = reply->wait_for(remaining, impl.stop_source.get_token());
    if (result) return std::move(*result);
    return impl.stopping_requested.load()
        ? LiveSessionOpenResult{LiveSessionOpenFailure::manager_stopping}
        : LiveSessionOpenResult{LiveSessionOpenFailure::open_timeout};
}

LiveSessionOpenResult LiveSessionManager::select(
    FullSessionId key,
    std::chrono::milliseconds deadline) {
    auto& impl = *runtime_->impl_;
    const auto epoch = impl.context_epoch.load();
    const auto absolute_deadline = std::chrono::steady_clock::now() + deadline;
    if (impl.stopping_requested.load()) {
        return LiveSessionOpenFailure::manager_stopping;
    }
    auto reply = std::make_shared<RuntimeReply<LiveSessionOpenResult>>();
    if (!impl.enqueue_control_until([&impl, key = std::move(key), reply, epoch] {
            if (impl.stopping_requested.load() || impl.global_maintenance
                || epoch != impl.context_epoch.load()) {
                (void)reply->complete(LiveSessionOpenFailure::manager_stopping);
                return;
            }
            if (impl.maintenance.contains(key)) {
                (void)reply->complete(LiveSessionOpenFailure::stopping);
                return;
            }

            impl.cleanup_retired();
            if (const auto found = impl.sessions.find(key);
                found != impl.sessions.end()) {
                if (!reply->claim()) return;
                found->second->cancel_retirement();
                const auto previous = impl.selected;
                impl.selected = key;
                if (previous && *previous != key) {
                    impl.sessions.at(*previous)->request_retire_when_idle();
                }
                (void)reply->complete(LiveSessionReady{});
                return;
            }

            LiveSessionHandle replaceable;
            if (impl.sessions.size() >= impl.settings.session_limit
                && impl.selected) {
                const auto selected = impl.sessions.find(*impl.selected);
                if (selected != impl.sessions.end()
                    && selected->second->idle_for_retirement()) {
                    replaceable = selected->second;
                }
            }
            if (impl.sessions.size() >= impl.settings.session_limit
                && !replaceable) {
                (void)reply->complete(LiveSessionOpenFailure::limit_reached);
                return;
            }

            LiveSessionHandle candidate;
            impl.publish_live(key);
            try {
                candidate = impl.create_session(key);
            } catch (const std::bad_alloc&) {
                std::terminate();
            } catch (const SessionNotFoundError&) {
                impl.publish_released(key);
                (void)reply->complete(LiveSessionOpenFailure::not_found);
                return;
            } catch (const ForumNotFoundError&) {
                impl.publish_released(key);
                (void)reply->complete(LiveSessionOpenFailure::not_found);
                return;
            } catch (...) {
                impl.publish_released(key);
                (void)reply->complete(LiveSessionOpenFailure::internal_error);
                return;
            }

            if (!reply->claim() || impl.stopping_requested.load()
                || epoch != impl.context_epoch.load()
                || impl.global_maintenance || impl.maintenance.contains(key)) {
                candidate->finalize(impl.stopping_requested.load()
                    ? ShutdownReason::server_stopping
                    : ShutdownReason::retired);
                impl.publish_released(key);
                (void)reply->complete(LiveSessionOpenFailure::manager_stopping);
                return;
            }

            const auto previous = impl.selected;
            candidate->set_running();
            impl.sessions.emplace(key, candidate);
            impl.selected = key;
            if (previous && *previous != key) {
                const auto old = impl.sessions.find(*previous);
                if (old != impl.sessions.end()) old->second->request_retire_when_idle();
            }
            (void)reply->complete(LiveSessionReady{});
        }, absolute_deadline)) {
        return impl.stopping_requested.load()
            ? LiveSessionOpenResult{LiveSessionOpenFailure::manager_stopping}
            : LiveSessionOpenResult{LiveSessionOpenFailure::open_timeout};
    }
    const auto remaining = std::max(
        std::chrono::milliseconds{0},
        std::chrono::duration_cast<std::chrono::milliseconds>(
            absolute_deadline - std::chrono::steady_clock::now()));
    auto result = reply->wait_for(remaining, impl.stop_source.get_token());
    if (result) return std::move(*result);
    return impl.stopping_requested.load()
        ? LiveSessionOpenResult{LiveSessionOpenFailure::manager_stopping}
        : LiveSessionOpenResult{LiveSessionOpenFailure::open_timeout};
}

std::optional<FullSessionId> LiveSessionManager::selected() const {
    auto& impl = *runtime_->impl_;
    if (impl.stopping_requested.load()) return std::nullopt;
    auto reply = std::make_shared<RuntimeReply<std::optional<FullSessionId>>>();
    if (!impl.enqueue_control([&impl, reply] {
            (void)reply->complete(impl.selected);
        })) return std::nullopt;
    auto result = reply->wait(impl.stop_source.get_token());
    return result ? std::move(*result) : std::nullopt;
}

void LiveSessionManager::close_session(
    const FullSessionId& key,
    std::uint64_t epoch) {
    auto& impl = *runtime_->impl_;
    if (impl.stopping_requested.load()) return;
    (void)impl.enqueue_control([&impl, key, epoch] {
        if (impl.global_maintenance
            || (epoch != 0 && epoch != impl.context_epoch)) return;
        const auto found = impl.sessions.find(key);
        if (found != impl.sessions.end()) {
            found->second->request_shutdown(ShutdownReason::retired);
        }
        if (impl.selected && *impl.selected == key) impl.selected.reset();
    });
}

std::uint64_t LiveSessionManager::context_epoch() const {
    return runtime_->impl_->context_epoch.load();
}

std::uint64_t LiveSessionManager::bump_context_epoch() {
    return runtime_->impl_->context_epoch.fetch_add(1) + 1;
}

std::optional<LiveSessionOpenResult> LiveSessionManager::try_reattach(
    const FullSessionId& key) {
    auto& impl = *runtime_->impl_;
    if (impl.stopping_requested.load()) {
        return LiveSessionOpenFailure::manager_stopping;
    }
    auto reply = std::make_shared<RuntimeReply<std::optional<LiveSessionOpenResult>>>();
    if (!impl.enqueue_control([&impl, key, reply] {
            if (impl.global_maintenance) {
                (void)reply->complete(LiveSessionOpenFailure::manager_stopping);
            } else if (impl.maintenance.contains(key)) {
                (void)reply->complete(LiveSessionOpenFailure::stopping);
            } else if (impl.sessions.contains(key)) {
                (void)reply->complete(LiveSessionReady{});
            } else {
                (void)reply->complete(std::nullopt);
            }
        })) return LiveSessionOpenFailure::manager_stopping;
    auto result = reply->wait(impl.stop_source.get_token());
    return result ? std::move(*result)
                  : std::optional<LiveSessionOpenResult>{
                        LiveSessionOpenFailure::manager_stopping};
}

LiveSessionHandle LiveSessionManager::lookup(
    const FullSessionId& key,
    std::uint64_t epoch) {
    auto& impl = *runtime_->impl_;
    if (impl.stopping_requested.load()) return {};
    auto reply = std::make_shared<RuntimeReply<LiveSessionHandle>>();
    if (!impl.enqueue_control([&impl, key, epoch, reply] {
            LiveSessionHandle result;
            const auto found = impl.sessions.find(key);
            if (!impl.global_maintenance
                && (epoch == 0 || epoch == impl.context_epoch)
                && found != impl.sessions.end()
                && found->second->lifecycle() == LiveSessionState::running) {
                result = found->second;
            }
            (void)reply->complete(std::move(result));
        })) return {};
    auto result = reply->wait(impl.stop_source.get_token());
    return result ? std::move(*result) : LiveSessionHandle{};
}

LiveSessionManagerSnapshot LiveSessionManager::snapshot() {
    auto& impl = *runtime_->impl_;
    if (impl.stopping_requested.load()) return {};
    auto reply = std::make_shared<RuntimeReply<LiveSessionManagerSnapshot>>();
    if (!impl.enqueue_control([&impl, reply] {
            LiveSessionManagerSnapshot result;
            result.live_session_count = impl.sessions.size();
            for (const auto& [key, session] : impl.sessions) {
                if (session->lifecycle() == LiveSessionState::running) {
                    result.running_sessions.push_back(key);
                }
            }
            (void)reply->complete(std::move(result));
        })) return {};
    auto result = reply->wait(impl.stop_source.get_token());
    return result ? std::move(*result) : LiveSessionManagerSnapshot{};
}

std::vector<LiveSessionHandle> LiveSessionManager::active_sessions() {
    auto& impl = *runtime_->impl_;
    if (impl.stopping_requested.load()) return {};
    auto reply = std::make_shared<RuntimeReply<std::vector<LiveSessionHandle>>>();
    if (!impl.enqueue_control([&impl, reply] {
            std::vector<LiveSessionHandle> result;
            result.reserve(impl.sessions.size());
            for (const auto& [key, session] : impl.sessions) {
                (void)key;
                result.push_back(session);
            }
            (void)reply->complete(std::move(result));
        })) return {};
    auto result = reply->wait(impl.stop_source.get_token());
    return result ? std::move(*result) : std::vector<LiveSessionHandle>{};
}

MaintenanceReservationResult LiveSessionManager::reserve_for_deletion(
    const FullSessionId& key,
    std::chrono::milliseconds deadline) {
    auto& impl = *runtime_->impl_;
    const auto end = std::chrono::steady_clock::now() + deadline;
    if (impl.stopping_requested.load()) return MaintenanceFailure::manager_stopping;
    auto active = std::make_shared<std::atomic_bool>(true);
    LiveSessionMaintenanceReservation reservation(*this, active);
    auto reply = std::make_shared<RuntimeReply<bool>>();
    if (!impl.enqueue_control_until([&impl, key, reply, active, end] {
            if (!active->load() || std::chrono::steady_clock::now() >= end
                || impl.stopping_requested.load() || impl.global_maintenance
                || impl.maintenance.contains(key)) {
                (void)reply->complete(false);
                return;
            }
            impl.maintenance.emplace(key, active);
            if (impl.selected && *impl.selected == key) impl.selected.reset();
            const auto found = impl.sessions.find(key);
            if (found != impl.sessions.end()) {
                found->second->request_shutdown(ShutdownReason::session_deleted);
            }
            (void)reply->complete(true);
        }, end)) {
        return impl.stopping_requested.load()
            ? MaintenanceFailure::manager_stopping : MaintenanceFailure::stopping;
    }
    auto admitted = reply->wait_until(end, impl.stop_source.get_token());
    if (!admitted || impl.stopping_requested.load()) {
        return impl.stopping_requested.load()
            ? MaintenanceFailure::manager_stopping : MaintenanceFailure::stopping;
    }
    if (!*admitted) return MaintenanceFailure::stopping;

    bool released;
    {
        std::unique_lock lock(impl.state_mutex);
        released = impl.state_changed.wait_until(lock, end, [&impl, &key] {
            return !impl.published_live.contains(key);
        });
    }
    if (!released) return MaintenanceFailure::stopping;
    return reservation;
}

GlobalMaintenanceResult LiveSessionManager::reserve_global_maintenance(
    std::chrono::milliseconds deadline) {
    auto& impl = *runtime_->impl_;
    const auto end = std::chrono::steady_clock::now() + deadline;
    if (impl.stopping_requested.load()) return MaintenanceFailure::manager_stopping;
    auto active = std::make_shared<std::atomic_bool>(true);
    LiveSessionGlobalMaintenance reservation(*this, active);
    auto reply = std::make_shared<RuntimeReply<bool>>();
    if (!impl.enqueue_control_until([&impl, reply, active, end] {
            if (!active->load() || std::chrono::steady_clock::now() >= end
                || impl.stopping_requested.load() || impl.global_maintenance) {
                (void)reply->complete(false);
                return;
            }
            impl.global_maintenance = active;
            impl.selected.reset();
            for (const auto& [key, session] : impl.sessions) {
                (void)key;
                session->request_shutdown(ShutdownReason::reloading);
            }
            (void)reply->complete(true);
        }, end)) {
        return impl.stopping_requested.load()
            ? MaintenanceFailure::manager_stopping : MaintenanceFailure::stopping;
    }
    auto admitted = reply->wait_until(end, impl.stop_source.get_token());
    if (!admitted || impl.stopping_requested.load()) {
        return impl.stopping_requested.load()
            ? MaintenanceFailure::manager_stopping : MaintenanceFailure::stopping;
    }
    if (!*admitted) return MaintenanceFailure::stopping;

    bool released;
    {
        std::unique_lock lock(impl.state_mutex);
        released = impl.state_changed.wait_until(lock, end, [&impl] {
            return impl.published_live.empty();
        });
    }
    if (!released) return MaintenanceFailure::stopping;
    return reservation;
}

void LiveSessionManager::begin_shutdown(
    const std::function<void()>& stop_accepting) {
    auto& impl = *runtime_->impl_;
    const bool first = !impl.stopping_requested.exchange(true);
    (void)impl.stop_source.request_stop();
    if (first && stop_accepting) stop_accepting();
    impl.queue_changed.notify_all();
    impl.notifier->wake();
}

bool LiveSessionManager::join_shutdown(std::chrono::milliseconds grace) {
    auto& impl = *runtime_->impl_;
    begin_shutdown();
    const auto deadline = std::chrono::steady_clock::now() + grace;
    {
        std::unique_lock lock(impl.state_mutex);
        if (!impl.state_changed.wait_until(lock, deadline, [&impl] {
                return impl.thread_finished.load();
            })) {
            return false;
        }
    }
    if (impl.thread.joinable()) impl.thread.join();
    return true;
}

std::vector<FullSessionId> LiveSessionManager::unfinished_owners() {
    return runtime_->impl_->published_identities();
}

void LiveSessionManager::sweep() {}

void LiveSessionManager::release_maintenance(
    const std::shared_ptr<std::atomic_bool>& active) noexcept {
    active->store(false);
    runtime_->wake();
}

} // namespace cha
