#pragma once

#include "web/live_session.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace cha::web {

class LiveSessionManager;

using LiveSessionHandle = std::shared_ptr<LiveSession>;

struct LiveSessionReady {};

enum class LiveSessionOpenFailure {
    not_found,
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
    std::vector<FullSessionId> running_sessions;
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
        std::shared_ptr<std::atomic_bool> active);
    void release() noexcept;

    LiveSessionManager* manager_{};
    std::shared_ptr<std::atomic_bool> active_;
};

class LiveSessionGlobalMaintenance {
public:
    ~LiveSessionGlobalMaintenance();
    LiveSessionGlobalMaintenance(LiveSessionGlobalMaintenance&& other) noexcept;
    LiveSessionGlobalMaintenance& operator=(
        LiveSessionGlobalMaintenance&& other) noexcept;
    LiveSessionGlobalMaintenance(const LiveSessionGlobalMaintenance&) = delete;
    LiveSessionGlobalMaintenance& operator=(
        const LiveSessionGlobalMaintenance&) = delete;

private:
    friend class LiveSessionManager;
    LiveSessionGlobalMaintenance(
        LiveSessionManager& manager,
        std::shared_ptr<std::atomic_bool> active);
    void release() noexcept;

    LiveSessionManager* manager_{};
    std::shared_ptr<std::atomic_bool> active_;
};

using MaintenanceReservationResult = std::variant<
    LiveSessionMaintenanceReservation,
    MaintenanceFailure>;
using GlobalMaintenanceResult = std::variant<
    LiveSessionGlobalMaintenance,
    MaintenanceFailure>;

// Compatibility facade around the one process-wide SessionRuntime. The
// runtime thread is the sole owner of controllers, the live map, selection,
// event processing, and retirement decisions.
class LiveSessionManager {
public:
    LiveSessionManager(
        cha::app::RuntimeSettings settings,
        SessionOpener opener,
        LiveSessionClock clock = {});
    ~LiveSessionManager();
    LiveSessionManager(const LiveSessionManager&) = delete;
    LiveSessionManager& operator=(const LiveSessionManager&) = delete;

    [[nodiscard]] LiveSessionOpenResult open(
        FullSessionId key,
        std::chrono::milliseconds deadline);
    [[nodiscard]] LiveSessionOpenResult select(
        FullSessionId key,
        std::chrono::milliseconds deadline);
    [[nodiscard]] std::optional<FullSessionId> selected() const;
    void close_session(const FullSessionId& key, std::uint64_t epoch = 0);
    [[nodiscard]] std::uint64_t context_epoch() const;
    std::uint64_t bump_context_epoch();
    [[nodiscard]] std::optional<LiveSessionOpenResult> try_reattach(
        const FullSessionId& key);
    [[nodiscard]] LiveSessionHandle lookup(
        const FullSessionId& key,
        std::uint64_t epoch = 0);
    [[nodiscard]] LiveSessionManagerSnapshot snapshot();
    [[nodiscard]] std::vector<LiveSessionHandle> active_sessions();
    [[nodiscard]] MaintenanceReservationResult reserve_for_deletion(
        const FullSessionId& key,
        std::chrono::milliseconds deadline);
    [[nodiscard]] GlobalMaintenanceResult reserve_global_maintenance(
        std::chrono::milliseconds deadline);

    void begin_shutdown(const std::function<void()>& stop_accepting = {});
    [[nodiscard]] std::vector<FullSessionId> unfinished_owners();
    [[nodiscard]] bool join_shutdown(std::chrono::milliseconds grace);
    // Kept as a harmless compatibility operation; retirement is immediate on
    // the runtime thread, so there are no finished owner threads to reap.
    void sweep();

private:
    friend class LiveSessionMaintenanceReservation;
    friend class LiveSessionGlobalMaintenance;
    void release_maintenance(const std::shared_ptr<std::atomic_bool>& active) noexcept;

    std::shared_ptr<SessionRuntime> runtime_;
};

} // namespace cha::web
