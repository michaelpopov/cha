#pragma once

#include "session/opened_session.h"
#include "ui/web/web_session_runtime.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace cha {
class Workspace;
}

namespace cha::web {

class SessionHandle {
public:
    SessionHandle() = default;
    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] WebSessionRuntime& runtime() const;

private:
    friend class SessionRegistry;
    explicit SessionHandle(std::shared_ptr<WebSessionRuntime> runtime);
    std::shared_ptr<WebSessionRuntime> runtime_;
};

using RegistryOpenResult = std::variant<OpenSessionSuccess, Error>;
// The port-backed alternative is limited to injected web tests. Production
// workspace startup always returns the OpenedSession alternative.
struct PortBackedSession {
    SessionDescriptor descriptor;
    std::unique_ptr<WebSessionController> controller;
};
using RegistryOwnerInput = std::variant<
    OpenedSession,
    PortBackedSession>;
using RegistrySessionFactory = std::function<RegistryOwnerInput(
    const SessionIdentity&, WakeNotifier&)>;

struct RegistrySnapshot {
    std::size_t live_entry_count{};
    std::vector<SessionIdentity> running_sessions;
};

// The registry is the sole authority for in-process session liveness.  The
// supplied factory is deliberately the small test seam; production may use
// from_workspace(), which keeps path validation and leasing in Workspace.
class SessionRegistry {
public:
    SessionRegistry(
        WebSettings settings,
        RegistrySessionFactory factory);
    static SessionRegistry from_workspace(
        WebSettings settings,
        std::shared_ptr<const Workspace> workspace);
    ~SessionRegistry();
    SessionRegistry(const SessionRegistry&) = delete;
    SessionRegistry& operator=(const SessionRegistry&) = delete;

    [[nodiscard]] RegistryOpenResult open(
        SessionIdentity key,
        std::chrono::milliseconds deadline);
    // Answers reattach and shutdown cases entirely from registry state. An
    // empty result means the caller must validate storage before open().
    [[nodiscard]] std::optional<RegistryOpenResult> try_reattach(
        const SessionIdentity& key);
    [[nodiscard]] SessionHandle lookup(const SessionIdentity& key);
    // A point-in-time view for lobby listings and health. Starting and
    // stopping entries count against the bound but only running entries are
    // returned as reattachable sessions.
    [[nodiscard]] RegistrySnapshot snapshot();

    // Begins process shutdown without writing startup results. The optional
    // callback runs after the stopping flag is published but before open
    // waiters are woken and already-published runtimes are asked to stop. The
    // process coordinator uses that point to stop HTTP acceptance in the
    // documented order.
    void begin_shutdown(const std::function<void()>& stop_accepting = {});
    // Returns the identities whose owner threads have not yet reported
    // completion.  The process-shutdown coordinator uses this after its
    // bounded grace period to report threads it cannot safely join.
    [[nodiscard]] std::vector<SessionIdentity> unfinished_owners();
    // Waits under one process-wide deadline for every owner to report its
    // final state, then reaps them outside the registry mutex. False leaves
    // the stuck owners untouched for the process coordinator to report before
    // it takes the no-destructor exit path.
    [[nodiscard]] bool join_shutdown(std::chrono::milliseconds grace);
    void sweep();

private:
    struct Entry;
    struct RetiredEntry;
    struct StartupResult;
    using RetiredEntries = std::vector<RetiredEntry>;

    [[nodiscard]] RetiredEntries sweep_locked();
    static void reap(RetiredEntries retired);
    void owner_main(SessionIdentity key, std::shared_ptr<StartupResult> startup);
    [[nodiscard]] static std::string path_for(const SessionIdentity& key);

    WebSettings settings_;
    RegistrySessionFactory factory_;
    std::mutex mutex_;
    std::condition_variable lifecycle_changed_;
    std::map<SessionIdentity, std::unique_ptr<Entry>, std::less<>> entries_;
    bool stopping_{};
};

} // namespace cha::web
