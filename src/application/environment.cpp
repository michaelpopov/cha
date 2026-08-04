#include "application/environment.h"

#include "application/builtins.h"
#include "session/session_database.h"

namespace cha {
ApplicationEnvironment::ApplicationEnvironment(const Workspace& workspace)
    : workspace_(workspace), snapshot_(workspace_), inventory_(snapshot_), personas_(snapshot_),
      forums_(workspace_, snapshot_, personas_.roster(), inventory_), welcome_storage_(),
      welcome_source_(make_welcome_session_source(workspace_, personas_.roster(), inventory_, welcome_storage_)) {}

OpenedSession ApplicationEnvironment::open_welcome(WakeNotifier& notifier) {
    return welcome_source_->open(welcome_name, notifier);
}
} // namespace cha
