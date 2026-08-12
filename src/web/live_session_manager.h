#pragma once

#include "web/live_session.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <variant>
#include <vector>

namespace cha::web {

class LiveSessionManager;

// A route retains the live actor itself; there is no wrapper whose only
// operation is to hand out the thing inside it.
using LiveSessionHandle = std::shared_ptr<LiveSession>;

// Manager results describe owner lifecycle only. HTTP paths and error
// envelopes are constructed at the route boundary.
struct LiveSessionReady {};

enum class LiveSessionOpenFailure {
    not_found,
    busy,
    stopping,
    limit_reached,
    open_timeout,
    manager_stopping,
    internal_error,
};

using LiveSessionOpenResult =
    std::variant<LiveSessionReady, LiveSessionOpenFailure>;

struct LiveSessionManagerSnapshot {
    std::size_t live_session_count{};
    std::vector<SessionIdentity> running_sessions;
};

enum class MaintenanceFailure { stopping, manager_stopping };

class LiveSessionMaintenanceReservation {
public:
    ~LiveSessionMaintenanceReservation();
    LiveSessionMaintenanceReservation(LiveSessionMaintenanceReservation&& other) noexcept;
    LiveSessionMaintenanceReservation& operator=(
        LiveSessionMaintenanceReservation&& other) noexcept;
    LiveSessionMaintenanceReservation(const LiveSessionMaintenanceReservation&) = delete;
    LiveSessionMaintenanceReservation& operator=(
        const LiveSessionMaintenanceReservation&) = delete;

private:
    friend class LiveSessionManager;
    LiveSessionMaintenanceReservation(
        LiveSessionManager& manager,
        SessionIdentity identity);
    void release() noexcept;

    LiveSessionManager* manager_{};
    SessionIdentity identity_;
};

using MaintenanceReservationResult = std::variant<
    LiveSessionMaintenanceReservation,
    MaintenanceFailure>;

// The process-wide collection of live actors, and the sole authority for
// in-process session liveness. It owns no storage or workspace knowledge: the
// supplied opener constructs one session, and production passes a lambda
// calling open_session(). Routes validate URL components before entering it.
//
// The manager coordinates the collection; each mapped LiveSession owns its own
// thread, controller, and lifecycle. No actor ever calls back into here, so
// the lock relationship is one-way: the manager may take an actor's short
// lifecycle lock, never the reverse.
class LiveSessionManager {
public:
    LiveSessionManager(
        WebSettings settings,
        SessionOpener opener,
        LiveSessionClock clock = {});
    ~LiveSessionManager();
    LiveSessionManager(const LiveSessionManager&) = delete;
    LiveSessionManager& operator=(const LiveSessionManager&) = delete;

    [[nodiscard]] LiveSessionOpenResult open(
        SessionIdentity key,
        std::chrono::milliseconds deadline);
    // Answers reattach and shutdown cases entirely from manager state. An
    // empty result means the caller must validate storage before open().
    [[nodiscard]] std::optional<LiveSessionOpenResult> try_reattach(
        const SessionIdentity& key);
    [[nodiscard]] LiveSessionHandle lookup(const SessionIdentity& key);
    // A point-in-time view for lobby listings and health. Starting and
    // stopping actors count against the bound but only running actors are
    // returned as reattachable sessions.
    [[nodiscard]] LiveSessionManagerSnapshot snapshot();
    // Reserves one identity against open/reattach and waits for any actor to
    // finish, releasing its lease. Filesystem work happens only after this
    // returns and therefore never under the manager mutex.
    [[nodiscard]] MaintenanceReservationResult reserve_for_deletion(
        const SessionIdentity& key,
        std::chrono::milliseconds deadline);

    // Begins process shutdown without writing startup results. The optional
    // callback runs after the stopping flag is published but before open
    // waiters are woken and live actors are asked to stop. The process
    // coordinator uses that point to stop HTTP acceptance in the documented
    // order.
    void begin_shutdown(const std::function<void()>& stop_accepting = {});
    // Returns the identities whose owner threads have not yet published
    // Finished. The process-shutdown coordinator uses this after its bounded
    // grace period to report owners it cannot safely join.
    [[nodiscard]] std::vector<SessionIdentity> unfinished_owners();
    // Waits under one process-wide deadline for every owner to finish, then
    // reaps them outside this mutex. False leaves the stuck owners untouched
    // for the process coordinator to report before it takes the no-destructor
    // exit path.
    [[nodiscard]] bool join_shutdown(std::chrono::milliseconds grace);
    void sweep();

private:
    friend class LiveSessionMaintenanceReservation;
    using RetiredSessions = std::vector<LiveSessionHandle>;

    [[nodiscard]] RetiredSessions sweep_locked();
    static void reap(RetiredSessions retired);
    void release_maintenance(const SessionIdentity& key) noexcept;

    WebSettings settings_;
    SessionOpener opener_;
    LiveSessionClock clock_;
    std::mutex mutex_;
    std::map<SessionIdentity, LiveSessionHandle, std::less<>> sessions_;
    std::set<SessionIdentity, std::less<>> maintenance_;
    bool stopping_{};
};

} // namespace cha::web
