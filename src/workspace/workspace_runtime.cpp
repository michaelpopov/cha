#include "workspace/workspace_runtime.h"

#include "session/session_snapshot.h"
#include "workspace/session_open.h"

#include <stdexcept>
#include <utility>

namespace cha {

WorkspaceRuntime::WorkspaceRuntime(
    std::filesystem::path root,
    TemporarySessionSeed temporary_session)
    : root_(std::move(root)),
      temporary_session_(std::move(temporary_session)),
      current_(load_generation()) {}

std::shared_ptr<const WorkspaceGeneration> WorkspaceRuntime::snapshot() const {
    std::lock_guard lock(mutex_);
    return current_;
}

void WorkspaceRuntime::reload() {
    // Loading outside the lock keeps a reload's disk work off every route and
    // session open, which all take this mutex to read the current generation.
    std::shared_ptr<const WorkspaceGeneration> candidate = load_generation(true);
    std::lock_guard lock(mutex_);
    current_ = std::move(candidate);
}

OpenedSession WorkspaceRuntime::open_session(
    const SessionIdentity& identity,
    Providers& providers,
    std::shared_ptr<WakeNotifier> notifier) const {
    const std::shared_ptr<const WorkspaceGeneration> generation = snapshot();
    OpenedSession opened = cha::open_session(
        *generation->model, *generation->sessions, identity, providers, std::move(notifier));
    opened.lifetime = generation;
    return opened;
}

std::shared_ptr<const WorkspaceGeneration> WorkspaceRuntime::load_generation(
    bool reload_sessions) const {
    const WorkspaceConfig config = load_workspace_config(root_);
    const auto model = std::make_shared<const WorkspaceDefinition>(
        WorkspaceDefinition::load(root_, config));
    const std::vector<ForumSessionDirectory> session_directories =
        model->session_directories();
    if (reload_sessions) {
        export_and_bootstrap_sessions(session_directories);
    }
    const auto sessions = std::make_shared<const SessionRepository>(
        session_directories, temporary_session_);
    return std::make_shared<const WorkspaceGeneration>(WorkspaceGeneration{
        .model = model,
        .sessions = sessions,
    });
}

} // namespace cha
