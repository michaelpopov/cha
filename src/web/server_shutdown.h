#pragma once

#include "web/live_session_manager.h"

#include <chrono>
#include <csignal>
#include <functional>
#include <thread>

namespace httplib {
class Server;
}

namespace cha::web {

// The signal handler itself only writes sig_atomic_t state. A normal thread
// observes it and performs shutdown work through this portable bridge.
class ProcessShutdownSignal {
public:
    ProcessShutdownSignal();
    ~ProcessShutdownSignal();
    ProcessShutdownSignal(const ProcessShutdownSignal&) = delete;
    ProcessShutdownSignal& operator=(const ProcessShutdownSignal&) = delete;

    [[nodiscard]] bool requested() const noexcept;

private:
    using Handler = void (*)(int);
    Handler previous_interrupt_{};
    Handler previous_terminate_{};
};

// Owns the bounded process-wide shutdown protocol, leaving web_main as wiring.
class ServerShutdownCoordinator {
public:
    // stop_accepting replaces server.stop() for a caller that may already
    // have stopped the server; cpp-httplib must not be stopped twice.
    ServerShutdownCoordinator(
        LiveSessionManager& live_sessions,
        httplib::Server& server,
        std::function<void()> stop_accepting = {},
        std::function<bool(std::chrono::steady_clock::time_point)> join_background = {});

    // Waits through the signal-safe bridge, then owns the complete bounded
    // shutdown policy. shutdown_now() is the directly testable half: it
    // rejects opens, stops HTTP acceptance, wakes/stops owners, enforces one
    // grace deadline, logs stuck identities and takes the no-destructor exit
    // path on expiry, or joins the listener after owners finish.
    void wait_and_shutdown(
        const ProcessShutdownSignal& signals,
        std::thread& listener,
        std::chrono::milliseconds grace);
    void shutdown_now(
        std::thread& listener,
        std::chrono::milliseconds grace);

private:
    LiveSessionManager& live_sessions_;
    httplib::Server& server_;
    std::function<void()> stop_accepting_;
    std::function<bool(std::chrono::steady_clock::time_point)> join_background_;
};

} // namespace cha::web
