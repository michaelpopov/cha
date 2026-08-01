#include "session/workspace.h"
#include "ui/web/asset_handler.h"
#include "ui/web/http_server.h"
#include "ui/web/lobby_routes.h"
#include "ui/web/session_routes.h"
#include "ui/web/session_registry.h"
#include "ui/web/server_shutdown.h"
#include "ui/web/web_settings.h"
#include "util/environment.h"
#include "util/logging.h"

#include <httplib.h>

#include <exception>
#include <iostream>
#include <string>
#include <thread>

// This is deliberately only the process composition root.
int main() {
    try {
        cha::load_dotenv();
        const cha::ApplicationConfig config = cha::load_application_config();
        cha::initialize_diagnostic_logging(config.log_file, config.log_level);
        auto workspace = std::make_shared<const cha::Workspace>(".", config);
        cha::web::WebSettings settings;
        cha::web::SessionRegistry registry = cha::web::SessionRegistry::from_workspace(
            settings, workspace);
        httplib::Server server;
        cha::web::configure_http_server(server, settings);
        cha::web::AssetHandler().install(server);
        cha::web::LobbyRoutes(workspace, registry, settings).install(server);
        cha::web::SessionRoutes(registry, settings).install(server);
        cha::web::ProcessShutdownSignal signals;
        cha::web::ServerShutdownCoordinator shutdown(registry, server);
        cha::log_info("Web server listener starting");
        if (!server.bind_to_port(config.host, config.port)) {
            const std::string message =
                "Could not listen on " + config.host + ':'
                + std::to_string(config.port);
            cha::log_error(message);
            std::cerr << "Failed: " << message << '\n';
            return 1;
        }
        std::thread listener([&server] { server.listen_after_bind(); });
        server.wait_until_ready();
        shutdown.wait_and_shutdown(signals, listener, settings.shutdown_grace);
        cha::shutdown_diagnostic_logging();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Failed: " << error.what() << '\n';
        return 1;
    }
}
