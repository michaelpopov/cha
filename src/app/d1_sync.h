#pragma once

#include "app/d1_client.h"
#include "chat/session_identity.h"
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
class SessionRepository;
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
        SessionRepository* sessions = nullptr;
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
    void collect_flags();
    // Returns false when the Worker halted (missing own table).
    bool send_pass(std::stop_token stop = {});
    void receive_pass(std::stop_token stop, bool notify_notes);
    void handle_full_database(D1Client& client);

    Config config_;
    std::atomic_bool stopped_{false};
    std::jthread thread_;
};

} // namespace cha
