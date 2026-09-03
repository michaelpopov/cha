#pragma once

#include "characters/character_config.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

struct WorkspaceConfigTransfer {
    std::size_t file_count{};
};

// Offline directory-to-database conversion. The caller supplies ordinary
// filesystem paths; the store acquires the CHA lease, validates a
// byte-identical materialization of the rows it will commit, and never
// starts HTTP or provider threads.
WorkspaceConfigTransfer import_workspace_configuration(
    const std::filesystem::path& source_directory,
    const std::filesystem::path& database_path);

WorkspaceConfigTransfer export_workspace_configuration(
    const std::filesystem::path& database_path,
    const std::filesystem::path& destination_directory);

// Reported when a runtime edit committed to SQLite but in-memory publication
// failed, or when pre-commit restoration of the materialized tree failed.
// The process must restart; the next startup publishes the committed rows.
class WorkspaceRestartRequiredError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct WorkspaceConfigEditResult {
    std::vector<std::string> affected_forum_ids;
};

// Normal-runtime owner: database lease, SQLite handle, one private temporary
// root with workspace/ and welcome/ children, and the configuration mutex.
class WorkspaceConfigStore {
public:
    static std::unique_ptr<WorkspaceConfigStore> open(
        const std::filesystem::path& database_path);

    ~WorkspaceConfigStore();
    WorkspaceConfigStore(const WorkspaceConfigStore&) = delete;
    WorkspaceConfigStore& operator=(const WorkspaceConfigStore&) = delete;
    WorkspaceConfigStore(WorkspaceConfigStore&&) = delete;
    WorkspaceConfigStore& operator=(WorkspaceConfigStore&&) = delete;

    [[nodiscard]] const std::filesystem::path& private_root() const noexcept;
    [[nodiscard]] const std::filesystem::path& workspace_path() const noexcept;
    [[nodiscard]] const std::filesystem::path& welcome_path() const noexcept;
    [[nodiscard]] const std::filesystem::path& database_path() const noexcept;
    WorkspaceConfigEditResult apply_character_settings(
        std::string_view character_id,
        std::string_view provider_id,
        std::optional<std::string_view> style_id,
        std::optional<std::string_view> reasoning_effort = std::nullopt,
        std::optional<WebSearchMode> web_search = std::nullopt);
    WorkspaceConfigEditResult apply_forum_default_character(
        std::string_view forum_id,
        std::string_view character_id);
    WorkspaceConfigEditResult apply_forum_default_persona(
        std::string_view forum_id,
        std::string_view persona_id);

private:
    struct Impl;
    explicit WorkspaceConfigStore(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Test-only seam for simulated collection, SQLite, and publication failures.
enum class WorkspaceConfigFault {
    none,
    collect_rows,
    sqlite_begin,
    sqlite_write,
    sqlite_commit,
    restore,
    publication,
};

void force_next_workspace_config_fault(WorkspaceConfigFault fault);

} // namespace cha
