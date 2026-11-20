#pragma once

#include "app/d1_client.h"
#include "chat/session_identity.h"
#include "storage/event_storage.h"
#include "storage/sync_storage.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace cha {

class WorkspaceConfigStore;
class LiveSessionManager;
// Minimal Worker for D1 sync. Owns one thread for the 30-second loop.
// Tests call run_once() directly without sleeping. Production starts the
// thread after the first synchronous loop at vault open.
class D1SyncWorker {
public:
    struct Config {
        std::filesystem::path database_path;
        std::string database_password;
        std::string vault;
        std::string machine;
        D1Client::Transport transport;
        // Reservation and store access; null in raw-DB tests.
        WorkspaceConfigStore* store = nullptr;
        // Live sessions for per-chat and global reservations; null skips reservation.
        LiveSessionManager* live_sessions = nullptr;
        std::function<bool()> application_stopping;
        // Called once after a receive group changes notes.
        std::function<void()> on_notes_changed;
        // Called once for each chat that a receive group changed and did not delete.
        std::function<void(const FullSessionId&, const std::string& label)> on_chat_changed;
    };

    explicit D1SyncWorker(Config config);
    ~D1SyncWorker();

    D1SyncWorker(const D1SyncWorker&) = delete;
    D1SyncWorker& operator=(const D1SyncWorker&) = delete;

    // One full loop: collect flags, send, receive, acknowledge.
    void run_once(bool notify_notes = true);
    // Send-only flush for shutdown and R2 upload preparation.
    void flush_send();
    void ensure_local_queue();
    // Starts the 30-second periodic thread. The first loop already ran
    // synchronously at vault open.
    void start(bool immediately = false);
    // Stops the periodic thread without flushing. Join is unbounded;
    // in-flight requests keep their normal timeouts.
    void stop();
    [[nodiscard]] bool stopped() const noexcept { return stopped_.load(); }

private:
    void run_loop(std::stop_token stop, bool notify_notes);
    [[nodiscard]] bool interrupted(std::stop_token stop) const;
    void halt_with_missing_table();
    // Provision legacy peers only at the v2 schema upgrade boundary.
    bool prepare_v2(std::stop_token stop = {});
    void report_v2(EventSeverity severity, const std::string& message);
    void pause_v2(bool sending, std::string_view stage, std::string_view reason,
        std::string_view op_id = {});
    void resume_v2(bool sending);
    void collect_flags();
    // Returns false when the Worker halted (missing own table).
    bool send_pass(std::stop_token stop = {});
    void receive_pass(std::stop_token stop, bool notify_notes);
    bool receive_legacy(D1Client& client, std::stop_token stop, bool notify_notes);
    void receive_v2(D1Client& client, std::stop_token stop, bool notify_notes);

    Config config_;
    std::string send_condition_;
    std::string receive_condition_;
    std::atomic_bool stopped_{false};
    std::jthread thread_;
};

} // namespace cha
