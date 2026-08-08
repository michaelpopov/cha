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

std::string session_log(const SessionIdentity& key, std::string_view event) {
    return "web session forum_id=" + key.forum_id + " session_id=" + key.session_id
        + " event=" + std::string(event);
}

LiveSessionOpenResult map_start_result(LiveSessionStartResult result) {
    switch (result) {
    case LiveSessionStartResult::ready: return LiveSessionReady{};
    case LiveSessionStartResult::busy: return LiveSessionOpenFailure::busy;
    case LiveSessionStartResult::not_found:
        return LiveSessionOpenFailure::not_found;
    case LiveSessionStartResult::failed:
        return LiveSessionOpenFailure::internal_error;
    case LiveSessionStartResult::shutting_down:
        return LiveSessionOpenFailure::manager_stopping;
    }
    return LiveSessionOpenFailure::internal_error;
}

} // namespace

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
    SessionIdentity key,
    std::chrono::milliseconds deadline) {
    log_info(session_log(key, "open_requested"));
    RetiredSessions retired;
    LiveSessionHandle starting;
    std::string deferred_event;
    {
        std::unique_lock lock(mutex_);
        retired = sweep_locked();
        if (stopping_) {
            lock.unlock();
            reap(std::move(retired));
            return LiveSessionOpenFailure::manager_stopping;
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
        return stopping_
            ? LiveSessionOpenResult{LiveSessionOpenFailure::manager_stopping}
            : LiveSessionOpenResult{LiveSessionOpenFailure::open_timeout};
    }
    return map_start_result(*outcome);
}

std::optional<LiveSessionOpenResult> LiveSessionManager::try_reattach(
    const SessionIdentity& key) {
    RetiredSessions retired;
    std::optional<LiveSessionOpenResult> result;
    {
        std::lock_guard lock(mutex_);
        retired = sweep_locked();
        if (stopping_) {
            result = LiveSessionOpenFailure::manager_stopping;
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

LiveSessionHandle LiveSessionManager::lookup(const SessionIdentity& key) {
    RetiredSessions retired;
    LiveSessionHandle result;
    {
        std::lock_guard lock(mutex_);
        retired = sweep_locked();
        const auto found = sessions_.find(key);
        if (found != sessions_.end()
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

std::vector<SessionIdentity> LiveSessionManager::unfinished_owners() {
    RetiredSessions retired;
    std::vector<SessionIdentity> result;
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
        // Completion is recorded after join so a wedged owner can never look
        // like a successfully reaped session. An external route handle may
        // still retain this actor; that is safe because its thread is joined
        // and every call now returns the not-live/stopping result.
        log_info(session_log(session->identity(), "registry_sweep_joined"));
    }
}

} // namespace cha::web
