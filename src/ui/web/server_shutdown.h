#pragma once

#include "ui/web/session_registry.h"

#include <chrono>
#include <csignal>
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
    ServerShutdownCoordinator(SessionRegistry& registry, httplib::Server& server);

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
    SessionRegistry& registry_;
    httplib::Server& server_;
};

} // namespace cha::web
