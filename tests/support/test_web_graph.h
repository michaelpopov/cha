#pragma once

#include "workspace/builtins.h"
#include "workspace/session_open.h"
#include "workspace/workspace_definition.h"
#include "workspace/workspace_runtime.h"
#include "session/session_repository.h"
#include "web/live_session.h"
#include "web/lobby_routes.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace cha::test {

// The production web graph over one fixture workspace: the workspace runtime
// publishing model and repository together, and the session opener chaweb
// installs. Web tests construct this instead of restating the startup steps.
class WebGraph {
public:
    explicit WebGraph(std::filesystem::path root)
        : root_(std::move(root)),
          runtime(std::make_shared<WorkspaceRuntime>(
              root_,
              TemporarySessionSeed{
                  {std::string(entrance_id), std::string(welcome_id)},
                  std::string(welcome_name)})) {}

    const std::filesystem::path& root() const noexcept { return root_; }

    // Read through the runtime rather than caching, so a test that reloads sees
    // the generation the routes under test are actually serving.
    std::shared_ptr<const WorkspaceDefinition> model() const {
        return runtime->snapshot()->model;
    }

    std::shared_ptr<const SessionRepository> sessions() const {
        return runtime->snapshot()->sessions;
    }

    web::SessionOpener opener() const {
        return [runtime = runtime](
                   const SessionIdentity& identity, WakeNotifier& notifier) {
            return runtime->open_session(identity, notifier);
        };
    }

    static web::InitialSelection initial_selection() {
        return {{std::string(entrance_id), std::string(welcome_id)}};
    }

    static SessionIdentity welcome() {
        return {std::string(entrance_id), std::string(welcome_id)};
    }

private:
    std::filesystem::path root_;

public:
    std::shared_ptr<WorkspaceRuntime> runtime;
};

} // namespace cha::test
