#include "application/web_discovery.h"
#include "application/welcome_storage.h"
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
        // Section 19.1 step 7 destroys the registry and Workspace before the
        // logging resources, so their teardown records still reach the sink.
        // The inner scope is what orders those destructors ahead of the
        // shutdown call below; returning from inside it would invert them.
        int status = 0;
        {
            auto workspace =
                std::make_shared<const cha::Workspace>(".", config);
            cha::WebDiscovery discovery(*workspace);
            cha::WelcomeStorage welcome_storage;
            cha::web::WebSettings settings;
            cha::web::SessionRegistry registry =
                cha::web::SessionRegistry::from_workspace(
                    settings, workspace, discovery, welcome_storage);
            httplib::Server server;
            cha::web::configure_http_server(
                server, settings, config.host, config.port);
            cha::web::AssetHandler().install(server);
            cha::web::LobbyRoutes(workspace, discovery, welcome_storage, registry, settings).install(server);
            cha::web::SessionRoutes(registry, settings).install(server);
            cha::web::ProcessShutdownSignal signals;
            cha::web::ServerShutdownCoordinator shutdown(registry, server);
            cha::log_info(
                "web server event=startup session_limit="
                + std::to_string(settings.session_limit)
                + " http_thread_pool_size=" + std::to_string(settings.http_thread_pool_size)
                + " http_pending_request_limit=" + std::to_string(settings.http_pending_request_limit)
                + " command_queue_capacity=" + std::to_string(settings.command_queue_capacity)
                + " request_body_limit=" + std::to_string(settings.request_body_limit)
                + " prompt_limit=" + std::to_string(settings.prompt_limit)
                + " http_read_timeout_ms=" + std::to_string(settings.http_read_timeout.count())
                + " http_write_timeout_ms=" + std::to_string(settings.http_write_timeout.count())
                + " open_deadline_ms=" + std::to_string(settings.open_deadline.count())
                + " command_deadline_ms=" + std::to_string(settings.command_deadline.count()));
            if (!server.bind_to_port(config.host, config.port)) {
                const std::string message =
                    "Could not listen on " + config.host + ':'
                    + std::to_string(config.port);
                cha::log_error(message);
                std::cerr << "Failed: " << message << '\n';
                status = 1;
            } else {
                cha::log_info(
                    "web server event=bound address=" + config.host
                    + ':' + std::to_string(config.port));
                std::thread listener([&server] { server.listen_after_bind(); });
                server.wait_until_ready();
                shutdown.wait_and_shutdown(
                    signals, listener, settings.shutdown_grace);
            }
        }
        cha::shutdown_diagnostic_logging();
        return status;
    } catch (const std::exception& error) {
        std::cerr << "Failed: " << error.what() << '\n';
        return 1;
    }
}
