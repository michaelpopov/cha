#pragma once

#include "application/session_source.h"
#include "application/workspace_snapshot.h"

#include <memory>
#include <string_view>
#include <vector>

namespace cha {
class WorkspaceInventory;
class ForumCatalog {
public:
    ForumCatalog(const Workspace& workspace, const WorkspaceSnapshot& snapshot, SharedPersonaRoster personas, const WorkspaceInventory& inventory);
    const Forum* find(std::string_view public_name) const;
    std::vector<std::string> custom_names() const;
    SessionSource& entrance_source() noexcept { return *entrance_source_; }
    SessionSource& source_for(std::string_view forum_name) const;
private:
    const WorkspaceSnapshot& snapshot_;
    std::unique_ptr<SessionSource> entrance_source_;
    std::vector<std::pair<std::string, std::unique_ptr<SessionSource>>> workspace_sources_;
};
} // namespace cha
