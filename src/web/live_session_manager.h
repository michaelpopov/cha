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

class WorkspaceReloadReservation {
public:
    ~WorkspaceReloadReservation();
    WorkspaceReloadReservation(WorkspaceReloadReservation&& other) noexcept;
    WorkspaceReloadReservation& operator=(WorkspaceReloadReservation&& other) noexcept;
    WorkspaceReloadReservation(const WorkspaceReloadReservation&) = delete;
    WorkspaceReloadReservation& operator=(const WorkspaceReloadReservation&) = delete;

private:
    friend class LiveSessionManager;
    explicit WorkspaceReloadReservation(LiveSessionManager& manager);
    void release() noexcept;

    LiveSessionManager* manager_{};
};

using WorkspaceReloadResult = std::variant<
    WorkspaceReloadReservation,
    MaintenanceFailure>;

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
    // Every actor a workspace change could still reach: those already running,
    // and those still opening. A starting actor may already have read its
    // definitions, so leaving it out could let it come up on settings that were
    // overwritten while it opened.
    //
    // It hands back the actors themselves rather than their identities because
    // both of the ordinary ways to reach one -- snapshot()'s running_sessions
    // and lookup() -- deliberately admit only Running actors, so an identity
    // returned here could not be resolved back into anything to act on.
    // Retaining the handles also keeps each actor alive for the caller's use.
    [[nodiscard]] std::vector<LiveSessionHandle> active_sessions();
    // Prevents new session opens, then stops every existing actor and waits
    // for its owner to release session storage. The reservation keeps opens
    // blocked while the caller backs up and reloads the workspace.
    [[nodiscard]] WorkspaceReloadResult reserve_workspace_reload(
        std::chrono::milliseconds grace);
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
    friend class WorkspaceReloadReservation;
    using RetiredSessions = std::vector<LiveSessionHandle>;

    [[nodiscard]] RetiredSessions sweep_locked();
    static void reap(RetiredSessions retired);
    void release_maintenance(const SessionIdentity& key) noexcept;
    void release_workspace_reload() noexcept;

    WebSettings settings_;
    SessionOpener opener_;
    LiveSessionClock clock_;
    std::mutex mutex_;
    std::map<SessionIdentity, LiveSessionHandle, std::less<>> sessions_;
    std::set<SessionIdentity, std::less<>> maintenance_;
    bool stopping_{};
    bool workspace_reloading_{};
};

} // namespace cha::web
