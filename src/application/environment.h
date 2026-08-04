#pragma once

#include "application/effective_personas.h"
#include "application/forum_catalog.h"
#include "application/welcome_storage.h"
#include "application/workspace_inventory.h"

namespace cha {
class WakeNotifier;

// Application-lifetime owner for immutable discovery data and the private
// Welcome database. It deliberately owns no frontend state or current chat.
class ApplicationEnvironment {
public:
    explicit ApplicationEnvironment(const Workspace& workspace);
    const EffectivePersonas& personas() const noexcept { return personas_; }
    const ForumCatalog& forums() const noexcept { return forums_; }
    OpenedSession open_welcome(WakeNotifier& notifier);

private:
    const Workspace& workspace_;
    WorkspaceSnapshot snapshot_;
    WorkspaceInventory inventory_;
    EffectivePersonas personas_;
    ForumCatalog forums_;
    // Members are destroyed in reverse order: the Welcome source first, then
    // its storage. Callers must destroy Welcome controllers before this owner.
    WelcomeStorage welcome_storage_;
    std::unique_ptr<SessionSource> welcome_source_;
};
} // namespace cha
