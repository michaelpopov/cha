#pragma once

#include "app/application.h"
#include "web/application_config.h"
#include "web/r2_database_transfer.h"
#include "workspace/workspace_config_store.h"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cha::web {

// Temporary HTTP adapter around Application. Vault, transfer, and maintenance
// operations are owned by Application; this object binds the listener.
class ApplicationRuntime {
public:
    static std::unique_ptr<ApplicationRuntime> open(
        const ApplicationCommand& command,
        std::string access_token = {},
        std::string vault_password = {});

    ~ApplicationRuntime();
    ApplicationRuntime(const ApplicationRuntime&) = delete;
    ApplicationRuntime& operator=(const ApplicationRuntime&) = delete;

    // A negative override uses the configured port. Zero asks the operating
    // system for an ephemeral port.
    [[nodiscard]] int start(int port_override = -1);
    void wait_for_shutdown_signal();
    void shutdown();

    [[nodiscard]] VaultDefinition current_vault() const;
    [[nodiscard]] VaultRegistrySnapshot vault_snapshot() const;
    [[nodiscard]] std::string api_key_value(std::string_view id) const;
    [[nodiscard]] bool has_r2_storage() const;
    [[nodiscard]] VaultDefinition create_vault(VaultCreate create);
    [[nodiscard]] VaultDefinition update_vault(
        std::string_view current_name,
        VaultUpdate update);
    void delete_vault(std::string_view name);
    void switch_vault(std::string_view name, std::string password = {});
    void merge_vault(std::string_view source_name, std::string password = {});
    [[nodiscard]] std::vector<std::string> list_r2_vaults() const;
    [[nodiscard]] VaultDefinition download_r2_vault(std::string_view name);
    [[nodiscard]] R2DatabaseTransfer upload_database();
    [[nodiscard]] R2DatabaseTransfer download_database();
    [[nodiscard]] WorkspaceConfigTransfer import_configuration();
    [[nodiscard]] WorkspaceConfigTransfer export_configuration();

private:
    struct Impl;
    explicit ApplicationRuntime(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace cha::web
