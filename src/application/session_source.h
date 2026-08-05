#pragma once

#include "agents/persona.h"
#include "session/opened_session.h"
#include "session/workspace.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cha {
class WakeNotifier;
class WorkspaceInventory;
class WelcomeStorage;

// Signals that durable catalog publication succeeded but controller setup did
// not, allowing the application transaction to retain the new session.
class SessionCreatedOpenError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Marks text that was deliberately constructed only from public names. Other
// exception text is diagnostic-only and must not cross a frontend boundary.
class PublicApplicationError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

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
    const WorkspaceInventory& inventory,
    WelcomeStorage& welcome_storage);
std::unique_ptr<SessionSource> make_workspace_session_source(
    const Workspace& workspace,
    Forum forum,
    SharedPersonaRoster personas);
} // namespace cha
