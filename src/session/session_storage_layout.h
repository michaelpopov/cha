#pragma once

#include "chat/session_identity.h"

#include <filesystem>
#include <vector>

namespace cha {

class Workspace;

struct LegacySessionSource {
    std::filesystem::path path;
    FullSessionId expected_identity;
    bool archived{};
};

[[nodiscard]] std::filesystem::path workspace_session_database_path(
    const Workspace& workspace);
[[nodiscard]] std::filesystem::path workspace_session_migration_path(
    const Workspace& workspace);
[[nodiscard]] std::vector<LegacySessionSource>
discover_legacy_session_sources(const Workspace& workspace);

} // namespace cha
