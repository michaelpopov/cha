#pragma once

#include "workspace/builtins.h"
#include "workspace/session_open.h"
#include "workspace/workspace_definition.h"
#include "session/session_repository.h"
#include "web/live_session.h"
#include "web/lobby_routes.h"

#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace cha::test {

// The production web graph over one fixture workspace: one loaded model, one
// repository owning its temporary Welcome database, and the session opener
// chaweb installs. Web tests construct this instead of restating the three
// startup steps.
class WebGraph {
public:
    explicit WebGraph(std::filesystem::path root)
        : root_(std::move(root)),
          model(std::make_shared<const WorkspaceDefinition>(
              WorkspaceDefinition::load(root_, load_workspace_config(root_)))),
          sessions(std::make_shared<const SessionRepository>(
              model->session_directories(),
              TemporarySessionSeed{
                  {std::string(entrance_id), std::string(welcome_id)},
                  std::string(welcome_name)})) {}

    const std::filesystem::path& root() const noexcept { return root_; }

    web::SessionOpener opener() const {
        return [model = model, sessions = sessions](
                   const SessionIdentity& identity, WakeNotifier& notifier) {
            return open_session(*model, *sessions, identity, notifier);
        };
    }

    static web::InitialSelection initial_selection() {
        return {std::string(guest_id),
                {std::string(entrance_id), std::string(welcome_id)}};
    }

    static SessionIdentity welcome() {
        return {std::string(entrance_id), std::string(welcome_id)};
    }

private:
    std::filesystem::path root_;

public:
    std::shared_ptr<const WorkspaceDefinition> model;
    std::shared_ptr<const SessionRepository> sessions;
};

} // namespace cha::test
