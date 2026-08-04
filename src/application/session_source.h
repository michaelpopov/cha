#pragma once

#include "agents/persona.h"
#include "session/opened_session.h"
#include "session/workspace.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cha {
class WakeNotifier;
class WorkspaceInventory;
class WelcomeStorage;

class SessionSource {
public:
    virtual ~SessionSource() = default;
    virtual std::vector<SessionSummary> list() const = 0;
    virtual OpenedSession open(std::string_view session_name, WakeNotifier& notifier) = 0;
    virtual OpenedSession create(std::string session_name, WakeNotifier& notifier) = 0;
};

std::unique_ptr<SessionSource> make_entrance_session_source(
    const Workspace& workspace,
    SharedPersonaRoster personas,
    const WorkspaceInventory& inventory);
std::unique_ptr<SessionSource> make_workspace_session_source(
    const Workspace& workspace,
    Forum forum,
    SharedPersonaRoster personas);
std::unique_ptr<SessionSource> make_welcome_session_source(
    const Workspace& workspace,
    SharedPersonaRoster personas,
    const WorkspaceInventory& inventory,
    WelcomeStorage& storage);
} // namespace cha
