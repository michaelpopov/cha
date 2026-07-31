#include "session/workspace.h"
#include "util/environment.h"
#include "util/logging.h"

#include <httplib.h>

#include <exception>
#include <iostream>
#include <string>

// This is deliberately only a composition root. Route registration and live
// session ownership arrive with their respective web-layer components.
int main() {
    try {
        cha::load_dotenv();
        const cha::ApplicationConfig config = cha::load_application_config();
        cha::initialize_diagnostic_logging(config.log_file, config.log_level);
        cha::Workspace workspace(".", config);
        httplib::Server server;
        cha::log_info("Web server listener starting");
        // The initial skeleton has no readiness protocol or signal-driven stop;
        // production lifecycle handling arrives with the server-process block.
        if (!server.listen(config.host, config.port)) {
            const std::string message =
                "Could not listen on " + config.host + ':'
                + std::to_string(config.port);
            cha::log_error(message);
            std::cerr << "Failed: " << message << '\n';
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Failed: " << error.what() << '\n';
        return 1;
    }
}
