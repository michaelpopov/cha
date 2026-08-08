#include "ui/web/server_shutdown.h"

#include "util/logging.h"

#include <httplib.h>

#include <cstdlib>
#include <thread>

namespace cha::web {
namespace {

volatile std::sig_atomic_t shutdown_requested = 0;
constexpr std::chrono::milliseconds signal_poll_interval{10};

extern "C" void record_shutdown_signal(int) {
    shutdown_requested = 1;
}

} // namespace

ProcessShutdownSignal::ProcessShutdownSignal() {
    shutdown_requested = 0;
    previous_interrupt_ = std::signal(SIGINT, record_shutdown_signal);
    previous_terminate_ = std::signal(SIGTERM, record_shutdown_signal);
}

ProcessShutdownSignal::~ProcessShutdownSignal() {
    std::signal(SIGINT, previous_interrupt_);
    std::signal(SIGTERM, previous_terminate_);
}

bool ProcessShutdownSignal::requested() const noexcept {
    return shutdown_requested != 0;
}

ServerShutdownCoordinator::ServerShutdownCoordinator(
    LiveSessionManager& live_sessions,
    httplib::Server& server)
    : live_sessions_(live_sessions), server_(server) {}

void ServerShutdownCoordinator::wait_and_shutdown(
    const ProcessShutdownSignal& signals,
    std::thread& listener,
    std::chrono::milliseconds grace) {
    while (!signals.requested() && server_.is_running()) {
        std::this_thread::sleep_for(signal_poll_interval);
    }
    shutdown_now(listener, grace);
}

void ServerShutdownCoordinator::shutdown_now(
    std::thread& listener,
    std::chrono::milliseconds grace) {
    live_sessions_.begin_shutdown([this] { server_.stop(); });
    if (!live_sessions_.join_shutdown(grace)) {
        for (const SessionIdentity& key : live_sessions_.unfinished_owners()) {
            log_critical(
                "Web shutdown grace expired: forum_id=" + key.forum_id
                + " session_id=" + key.session_id);
        }
        // A stuck owner cannot be safely joined. The operating system releases
        // leases, and skipping static destructors prevents an unbounded
        // teardown after the documented grace period.
        std::_Exit(1);
    }
    // cpp-httplib joins request workers before listen_after_bind returns. It
    // is safe to do that only after every owner is within the bounded grace;
    // otherwise a stuck HTTP worker could suppress the forced process exit.
    if (listener.joinable()) listener.join();
    log_info("web server event=shutdown");
}

} // namespace cha::web
