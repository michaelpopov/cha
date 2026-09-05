#pragma once

#include "web/application_config.h"
#include "web/r2_database_transfer.h"

#include <memory>
#include <string>

namespace cha::web {

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

    [[nodiscard]] R2DatabaseTransfer upload_database();
    [[nodiscard]] R2DatabaseTransfer download_database();

private:
    struct Impl;
    explicit ApplicationRuntime(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace cha::web
