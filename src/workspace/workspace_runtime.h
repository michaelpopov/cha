#pragma once

#include "session/opened_session.h"
#include "session/session_repository.h"
#include "workspace/workspace_definition.h"

#include <filesystem>
#include <memory>
#include <mutex>

namespace cha {

class WakeNotifier;

// One complete, immutable workspace generation. Keeping the definition and
// repository together means discovery and session storage always agree about
// the forums that exist in one generation.
struct WorkspaceGeneration {
    std::shared_ptr<const WorkspaceDefinition> model;
    std::shared_ptr<const SessionRepository> sessions;
};

// Owns the workspace generation currently published by chaweb. Reload builds
// and validates a complete replacement before publishing it, so an invalid
// hand edit leaves the running application unchanged.
class WorkspaceRuntime final {
public:
    WorkspaceRuntime(
        std::filesystem::path root,
        TemporarySessionSeed temporary_session);
    WorkspaceRuntime(const WorkspaceRuntime&) = delete;
    WorkspaceRuntime& operator=(const WorkspaceRuntime&) = delete;

    [[nodiscard]] std::shared_ptr<const WorkspaceGeneration> snapshot() const;

    // Re-reads the workspace configuration and publishes a fresh generation.
    // Throws when the candidate cannot be loaded; the current generation is
    // retained in that case.
    void reload();

    // Opens from one retained generation. The returned lifetime keeps the
    // definition alive for controller callbacks that borrow it for the
    // session's lifetime.
    [[nodiscard]] OpenedSession open_session(
        const SessionIdentity& identity,
        WakeNotifier& notifier) const;

private:
    [[nodiscard]] std::shared_ptr<const WorkspaceGeneration> load_generation() const;

    const std::filesystem::path root_;
    const TemporarySessionSeed temporary_session_;
    mutable std::mutex mutex_;
    std::shared_ptr<const WorkspaceGeneration> current_;
};

} // namespace cha
