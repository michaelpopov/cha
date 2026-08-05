#include "application/environment.h"

#include "application/builtins.h"
#include "session/session_database.h"

namespace cha {
ApplicationEnvironment::ApplicationEnvironment(const Workspace& workspace)
    : workspace_(workspace), snapshot_(workspace_), inventory_(snapshot_), personas_(snapshot_),
      forums_(workspace_, snapshot_, personas_.roster(), inventory_) {}

OpenedSession ApplicationEnvironment::open_welcome(WakeNotifier& notifier) {
    return forums_.source_for(entrance_name).open(welcome_name, notifier);
}
} // namespace cha
