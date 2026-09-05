#include "web/live_session_manager.h"

#include "util/logging.h"

#include <exception>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace cha::web {
namespace {

std::string session_log(const FullSessionId& key, std::string_view event) {
    return "web session forum_id=" + key.forum_id + " session_id=" + key.session_id
        + " event=" + std::string(event);
}

LiveSessionOpenResult map_start_result(LiveSessionStartResult result) {
    switch (result) {
    case LiveSessionStartResult::ready: return LiveSessionReady{};
    case LiveSessionStartResult::not_found:
        return LiveSessionOpenFailure::not_found;
    case LiveSessionStartResult::failed:
        return LiveSessionOpenFailure::internal_error;
    case LiveSessionStartResult::shutting_down:
        return LiveSessionOpenFailure::stopping;
    }
    return LiveSessionOpenFailure::internal_error;
}

} // namespace

LiveSessionMaintenanceReservation::LiveSessionMaintenanceReservation(
    LiveSessionManager& manager,
    FullSessionId identity)
    : manager_(&manager), identity_(std::move(identity)) {}

LiveSessionMaintenanceReservation::~LiveSessionMaintenanceReservation() {
    release();
}

LiveSessionMaintenanceReservation::LiveSessionMaintenanceReservation(
    LiveSessionMaintenanceReservation&& other) noexcept
    : manager_(std::exchange(other.manager_, nullptr)),
      identity_(std::move(other.identity_)) {}

LiveSessionMaintenanceReservation&
LiveSessionMaintenanceReservation::operator=(
    LiveSessionMaintenanceReservation&& other) noexcept {
    if (this != &other) {
        release();
        manager_ = std::exchange(other.manager_, nullptr);
        identity_ = std::move(other.identity_);
    }
    return *this;
}

void LiveSessionMaintenanceReservation::release() noexcept {
    if (LiveSessionManager* const manager = std::exchange(manager_, nullptr)) {
        manager->release_maintenance(identity_);
    }
}

LiveSessionGlobalMaintenance::LiveSessionGlobalMaintenance(
    LiveSessionManager& manager)
    : manager_(&manager) {}

LiveSessionGlobalMaintenance::~LiveSessionGlobalMaintenance() {
    release();
}

LiveSessionGlobalMaintenance::LiveSessionGlobalMaintenance(
    LiveSessionGlobalMaintenance&& other) noexcept
    : manager_(std::exchange(other.manager_, nullptr)) {}

LiveSessionGlobalMaintenance&
LiveSessionGlobalMaintenance::operator=(
    LiveSessionGlobalMaintenance&& other) noexcept {
    if (this != &other) {
        release();
        manager_ = std::exchange(other.manager_, nullptr);
    }
    return *this;
}

void LiveSessionGlobalMaintenance::release() noexcept {
    if (LiveSessionManager* const manager = std::exchange(manager_, nullptr)) {
        manager->release_global_maintenance();
    }
}

LiveSessionManager::LiveSessionManager(
    WebSettings settings,
    SessionOpener opener,
    LiveSessionClock clock)
    : settings_(validate_live_session_settings(std::move(settings))),
      opener_(std::move(opener)),
      clock_(std::move(clock)) {
    if (!opener_) throw std::invalid_argument("Live session manager needs a session opener");
    if (settings_.session_limit == 0) {
        throw std::invalid_argument("Web session limit must be positive");
    }
}

LiveSessionManager::~LiveSessionManager() {
    // Production reaches normal destruction only after the process-shutdown
    // coordinator's bounded grace period has confirmed that every owner can be
    // joined.  If that grace expires, Section 19.1 requires immediate process
    // exit without running destructors, so a wedged owner must never reach
    // this unbounded final safety join.
    begin_shutdown();
    RetiredSessions owners;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [key, session] : sessions_) {
            (void)key;
            owners.push_back(session);
        }
    }
    for (const LiveSessionHandle& owner : owners) owner->join_owner();
}

LiveSessionOpenResult LiveSessionManager::open(
    FullSessionId key,
    std::chrono::milliseconds deadline) {
    log_info(session_log(key, "open_requested"));
    RetiredSessions retired;
    LiveSessionHandle starting;
    std::string deferred_event;
    {
        std::unique_lock lock(mutex_);
        retired = sweep_locked();
        if (stopping_ || global_maintenance_) {
            lock.unlock();
            reap(std::move(retired));
            return LiveSessionOpenFailure::manager_stopping;
        }
        if (maintenance_.contains(key)) {
            lock.unlock();
            reap(std::move(retired));
            return LiveSessionOpenFailure::stopping;
        }
        const auto found = sessions_.find(key);
        if (found != sessions_.end()) {
            switch (found->second->lifecycle()) {
            case LiveSessionState::running:
                lock.unlock();
                log_info(session_log(key, "reattached"));
                reap(std::move(retired));
                return LiveSessionReady{};
            case LiveSessionState::stopping:
            case LiveSessionState::finished:
                lock.unlock();
                log_warn(session_log(key, "open_rejected_stopping"));
                reap(std::move(retired));
                return LiveSessionOpenFailure::stopping;
            case LiveSessionState::starting:
                starting = found->second;
                break;
            }
        } else {
            if (sessions_.size() >= settings_.session_limit) {
                lock.unlock();
                log_warn(session_log(key, "open_rejected_limit"));
                reap(std::move(retired));
                return LiveSessionOpenFailure::limit_reached;
            }
            deferred_event = "registry_starting";
            // LiveSession construction is manager-only so no actor can remain
            // permanently Starting without an owner thread. A direct shared
            // pointer construction is required because make_shared's internal
            // constructor call is not covered by LiveSession's friendship.
            starting = LiveSessionHandle(
                new LiveSession(settings_, key, opener_, clock_));
            // Inserting before starting the owner is what makes concurrent
            // same-identity opens share one actor and one outcome, and what
            // guarantees the owner thread's raw `this` stays alive.  Holding
            // the mutex across both steps keeps insertion and thread start
            // transactional, so no waiter can observe an actor whose owner
            // failed to start.
            auto [position, inserted] = sessions_.emplace(key, starting);
            (void)inserted;
            try {
                starting->start_owner();
            } catch (const std::bad_alloc&) {
                std::terminate();
            } catch (...) {
                starting->resolve_unstarted(LiveSessionStartResult::failed);
                sessions_.erase(position);
                lock.unlock();
                reap(std::move(retired));
                return LiveSessionOpenFailure::internal_error;
            }
        }
    }
    if (!deferred_event.empty()) log_info(session_log(key, deferred_event));
    reap(std::move(retired));

    // Waiting happens outside the manager mutex and uses only this caller's
    // deadline. Timing out never cancels the shared startup.
    const auto outcome = starting->wait_for_start(deadline);
    if (!outcome) {
        log_warn(session_log(key, "open_deadline_expired"));
        std::lock_guard lock(mutex_);
        return stopping_ || global_maintenance_
            ? LiveSessionOpenResult{LiveSessionOpenFailure::manager_stopping}
            : LiveSessionOpenResult{LiveSessionOpenFailure::open_timeout};
    }
    if (*outcome == LiveSessionStartResult::shutting_down) {
        std::lock_guard lock(mutex_);
        if (stopping_ || global_maintenance_) {
            return LiveSessionOpenFailure::manager_stopping;
        }
    }
    if (*outcome == LiveSessionStartResult::ready) {
        std::lock_guard lock(mutex_);
        if (stopping_ || global_maintenance_) {
            return LiveSessionOpenFailure::manager_stopping;
        }
        if (maintenance_.contains(key)) {
            return LiveSessionOpenFailure::stopping;
        }
        const auto found = sessions_.find(key);
        if (found == sessions_.end()
            || found->second != starting
            || found->second->lifecycle() != LiveSessionState::running) {
            return LiveSessionOpenFailure::stopping;
        }
    }
    return map_start_result(*outcome);
}

std::optional<LiveSessionOpenResult> LiveSessionManager::try_reattach(
    const FullSessionId& key) {
    RetiredSessions retired;
    std::optional<LiveSessionOpenResult> result;
    {
        std::lock_guard lock(mutex_);
        retired = sweep_locked();
        if (stopping_ || global_maintenance_) {
            result = LiveSessionOpenFailure::manager_stopping;
        } else if (maintenance_.contains(key)) {
            result = LiveSessionOpenFailure::stopping;
        } else {
            const auto found = sessions_.find(key);
            if (found != sessions_.end()
                && found->second->lifecycle() == LiveSessionState::running) {
                result = LiveSessionReady{};
            }
        }
    }
    reap(std::move(retired));
    if (result) {
        if (std::holds_alternative<LiveSessionReady>(*result)) {
            log_info(session_log(key, "reattached"));
        } else {
            log_warn(session_log(key, "open_rejected_server_stopping"));
        }
    }
    return result;
}

LiveSessionHandle LiveSessionManager::lookup(const FullSessionId& key) {
    RetiredSessions retired;
    LiveSessionHandle result;
    {
        std::lock_guard lock(mutex_);
        retired = sweep_locked();
        const auto found = sessions_.find(key);
        if (!global_maintenance_ && found != sessions_.end()
            && found->second->lifecycle() == LiveSessionState::running) {
            result = found->second;
        }
    }
    reap(std::move(retired));
    return result;
}

LiveSessionManagerSnapshot LiveSessionManager::snapshot() {
    RetiredSessions retired;
    LiveSessionManagerSnapshot result;
    {
        std::lock_guard lock(mutex_);
        retired = sweep_locked();
        result.live_session_count = sessions_.size();
        for (const auto& [key, session] : sessions_) {
            if (session->lifecycle() == LiveSessionState::running) {
                result.running_sessions.push_back(key);
            }
        }
    }
    reap(std::move(retired));
    return result;
}

std::vector<LiveSessionHandle> LiveSessionManager::active_sessions() {
    RetiredSessions retired;
    std::vector<LiveSessionHandle> result;
    {
        std::lock_guard lock(mutex_);
        retired = sweep_locked();
        for (const auto& [key, session] : sessions_) {
            (void)key;
            const LiveSessionState state = session->lifecycle();
            if (state == LiveSessionState::starting
                || state == LiveSessionState::running) {
                result.push_back(session);
            }
        }
    }
    reap(std::move(retired));
    return result;
}

MaintenanceReservationResult LiveSessionManager::reserve_for_deletion(
    const FullSessionId& key,
    std::chrono::milliseconds deadline) {
    RetiredSessions retired;
    LiveSessionHandle actor;
    std::optional<MaintenanceFailure> failure;
    {
        std::lock_guard lock(mutex_);
        retired = sweep_locked();
        if (stopping_ || global_maintenance_) {
            failure = MaintenanceFailure::manager_stopping;
        } else if (maintenance_.contains(key)) {
            failure = MaintenanceFailure::stopping;
        } else {
            maintenance_.insert(key);
            const auto found = sessions_.find(key);
            if (found != sessions_.end()) actor = found->second;
        }
    }
    reap(std::move(retired));
    if (failure) return *failure;

    LiveSessionMaintenanceReservation reservation(*this, key);
    if (!actor) return reservation;

    log_info(session_log(key, "delete_shutdown_requested"));
    actor->request_shutdown(ShutdownReason::session_deleted);
    const auto absolute_deadline = std::chrono::steady_clock::now() + deadline;
    if (!actor->wait_until_finished(absolute_deadline)) {
        return MaintenanceFailure::stopping;
    }
    sweep();
    return reservation;
}

GlobalMaintenanceResult LiveSessionManager::reserve_global_maintenance(
    std::chrono::milliseconds deadline) {
    RetiredSessions retired;
    RetiredSessions live;
    std::optional<MaintenanceFailure> failure;
    {
        std::lock_guard lock(mutex_);
        retired = sweep_locked();
        if (stopping_) {
            failure = MaintenanceFailure::manager_stopping;
        } else if (global_maintenance_) {
            failure = MaintenanceFailure::stopping;
        } else {
            // Snapshotting before publishing the flag keeps a failed
            // allocation from leaving the manager closed with no owner.
            for (const auto& [key, session] : sessions_) {
                (void)key;
                live.push_back(session);
            }
            global_maintenance_ = true;
        }
    }
    if (failure) {
        reap(std::move(retired));
        return *failure;
    }

    // The flag is now ours, so it is owned from here on: every failure below
    // releases it instead of closing the manager for the rest of the process.
    LiveSessionGlobalMaintenance reservation(*this);
    for (const LiveSessionHandle& session : live) {
        session->wake_start_waiters();
    }
    reap(std::move(retired));
    for (const LiveSessionHandle& session : live) {
        session->request_shutdown(ShutdownReason::reloading);
    }

    const auto absolute_deadline =
        std::chrono::steady_clock::now() + deadline;
    for (const LiveSessionHandle& session : live) {
        if (!session->wait_until_finished(absolute_deadline)) {
            return MaintenanceFailure::stopping;
        }
    }
    sweep();
    return reservation;
}

void LiveSessionManager::begin_shutdown(
    const std::function<void()>& stop_accepting) {
    RetiredSessions retired;
    RetiredSessions live;
    {
        std::lock_guard lock(mutex_);
        retired = sweep_locked();
        stopping_ = true;
        for (const auto& [key, session] : sessions_) {
            (void)key;
            live.push_back(session);
        }
    }
    if (stop_accepting) stop_accepting();
    // Waking startup waiters never writes a startup result: the owner remains
    // its sole writer, and an interrupted waiter reports manager shutdown.
    for (const LiveSessionHandle& session : live) session->wake_start_waiters();
    reap(std::move(retired));
    for (const LiveSessionHandle& session : live) {
        session->request_shutdown(ShutdownReason::server_stopping);
    }
}

bool LiveSessionManager::join_shutdown(std::chrono::milliseconds grace) {
    // One absolute deadline for the whole process, never a fresh grace period
    // per owner.
    const auto deadline = std::chrono::steady_clock::now() + grace;
    RetiredSessions live;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [key, session] : sessions_) {
            (void)key;
            live.push_back(session);
        }
    }
    for (const LiveSessionHandle& session : live) {
        if (!session->wait_until_finished(deadline)) return false;
    }
    // Finished is published only after the controller and every blocking
    // resource have been released. From that point to owner_main returning
    // there is only non-blocking stack unwinding, so reaping here cannot turn
    // an owner operation into an unbounded join outside the grace deadline.
    sweep();
    return true;
}

std::vector<FullSessionId> LiveSessionManager::unfinished_owners() {
    RetiredSessions retired;
    std::vector<FullSessionId> result;
    {
        std::lock_guard lock(mutex_);
        retired = sweep_locked();
        result.reserve(sessions_.size());
        for (const auto& [key, session] : sessions_) {
            if (session->owner_pending()) result.push_back(key);
        }
    }
    reap(std::move(retired));
    return result;
}

void LiveSessionManager::sweep() {
    RetiredSessions retired;
    {
        std::lock_guard lock(mutex_);
        retired = sweep_locked();
    }
    reap(std::move(retired));
}

LiveSessionManager::RetiredSessions LiveSessionManager::sweep_locked() {
    RetiredSessions retired;
    for (auto it = sessions_.begin(); it != sessions_.end();) {
        if (it->second->lifecycle() == LiveSessionState::finished) {
            retired.push_back(std::move(it->second));
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
    return retired;
}

void LiveSessionManager::reap(RetiredSessions retired) {
    for (const LiveSessionHandle& session : retired) {
        session->join_finished();
        // Success is recorded after join so a wedged owner can never look
        // like a successfully reaped session. An external route handle may
        // still retain this actor; that is safe because its thread is joined
        // and every call now returns the not-live/stopping result.
        log_info(session_log(session->identity(), "registry_sweep_joined"));
    }
}

void LiveSessionManager::release_maintenance(
    const FullSessionId& key) noexcept {
    try {
        std::lock_guard lock(mutex_);
        maintenance_.erase(key);
    } catch (...) {
        std::terminate();
    }
}

void LiveSessionManager::release_global_maintenance() noexcept {
    try {
        std::lock_guard lock(mutex_);
        global_maintenance_ = false;
    } catch (...) {
        std::terminate();
    }
}

} // namespace cha::web
