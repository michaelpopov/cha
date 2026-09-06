#pragma once

#include "web/application_config.h"
#include "web/r2_database_transfer.h"
#include "workspace/workspace_config_store.h"

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cha::web {

class UnknownVaultError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Owns one complete running web application. The command-line executable and
// the macOS in-process bridge share this composition root; only the requested
// listener port and optional private access token differ.
class ApplicationRuntime {
public:
    static std::unique_ptr<ApplicationRuntime> open(
        const ApplicationCommand& command,
        std::string access_token = {});

    ~ApplicationRuntime();
    ApplicationRuntime(const ApplicationRuntime&) = delete;
    ApplicationRuntime& operator=(const ApplicationRuntime&) = delete;

    // A negative override uses the configured port. Zero asks the operating
    // system for an ephemeral port.
    [[nodiscard]] int start(int port_override = -1);
    void wait_for_shutdown_signal();
    void shutdown();

    [[nodiscard]] VaultDefinition current_vault() const;
    void switch_vault(std::string_view name);
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
