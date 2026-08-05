#include "application/chat_application.h"
#include "session/workspace.h"
#include "ui/tui/terminal.h"
#include "ui/tui/persona.h"
#include "util/environment.h"
#include "util/logging.h"
#include "util/uv_event_loop.h"

#include <exception>
#include <iostream>

static int main_internal();

int main() {
    try {
        return main_internal();
    } catch (const std::exception& error) {
        cha::log_critical("Terminal application terminated by an unhandled exception");
        std::cerr << "Failed: " << error.what() << '\n';
        return 1;
    }
}

int main_internal() {
    cha::load_dotenv();

    const cha::ApplicationConfig app_config =
        cha::load_application_config();
    cha::initialize_diagnostic_logging(
        app_config.log_file,
        app_config.log_level);
    cha::log_info("Terminal application started");
    cha::Workspace workspace(".", app_config);
    cha::Terminal terminal;
    cha::UvEventLoop event_loop;
    cha::ChatApplication application(workspace, event_loop);
    cha::run_application(terminal, application, event_loop);
    cha::log_info("Terminal application stopped");
    return 0;
}
