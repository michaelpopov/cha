#pragma once

#include "ui/web/protocol.h"
#include "ui/web/web_session_runtime.h"

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

namespace cha {
class Workspace;
}

namespace cha::web {

struct SessionKey {
    std::string forum;
    std::string session_id;
    bool operator==(const SessionKey&) const = default;
    bool operator<(const SessionKey& other) const noexcept {
        return forum == other.forum ? session_id < other.session_id : forum < other.forum;
    }
};

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
using RegistryControllerFactory = std::function<std::unique_ptr<WebSessionController>(
    const SessionKey&, WakeNotifier&)>;

// The registry is the sole authority for in-process session liveness.  The
// supplied factory is deliberately the small test seam; production may use
// from_workspace(), which keeps path validation and leasing in Workspace.
class SessionRegistry {
public:
    SessionRegistry(WebSettings settings, RegistryControllerFactory factory);
    static SessionRegistry from_workspace(
        WebSettings settings,
        std::shared_ptr<const Workspace> workspace);
    ~SessionRegistry();
    SessionRegistry(const SessionRegistry&) = delete;
    SessionRegistry& operator=(const SessionRegistry&) = delete;

    [[nodiscard]] RegistryOpenResult open(
        SessionKey key,
        std::chrono::milliseconds deadline);
    [[nodiscard]] SessionHandle lookup(const SessionKey& key);

    // Begins process shutdown without writing startup results.  It wakes open
    // waiters and asks only already-published runtimes to stop.
    void begin_shutdown();
    // Returns the identities whose owner threads have not yet reported
    // completion.  The process-shutdown coordinator uses this after its
    // bounded grace period to report threads it cannot safely join.
    [[nodiscard]] std::vector<SessionKey> unfinished_owners();
    void sweep();

private:
    struct Entry;
    struct StartupResult;
    using RetiredEntries = std::vector<std::unique_ptr<Entry>>;

    [[nodiscard]] RetiredEntries sweep_locked();
    static void reap(RetiredEntries retired);
    void owner_main(SessionKey key, std::shared_ptr<StartupResult> startup);
    [[nodiscard]] static std::string path_for(const SessionKey& key);

    WebSettings settings_;
    RegistryControllerFactory factory_;
    std::mutex mutex_;
    std::map<SessionKey, std::unique_ptr<Entry>, std::less<>> entries_;
    bool stopping_{};
};

} // namespace cha::web
