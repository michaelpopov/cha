#include "session/session_controller.h"
#include "session/workspace.h"
#include "ui/tui/startup_selector.h"
#include "ui/tui/terminal.h"
#include "ui/tui/persona.h"
#include "util/environment.h"
#include "util/logging.h"
#include "util/uv_event_loop.h"

#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

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
    cha::StartupSelector selector(terminal);

    std::string selected_persona_id;
    {
        const cha::PersonaRoster personas = workspace.load_personas();
        const auto selected_persona = selector.select_persona(personas);
        if (!selected_persona) {
            throw std::runtime_error("Persona selection cancelled");
        }
        selected_persona_id = selected_persona->id;
    }

    std::vector<cha::Forum> forums;
    for (const std::string& forum_name : workspace.forums()) {
        forums.push_back(workspace.load_forum(forum_name));
    }
    const auto forum_name = selector.select_forum(forums);
    if (!forum_name) {
        throw std::runtime_error("Forum selection cancelled");
    }

    const auto selected_session = selector.select_session(
        workspace.sessions(*forum_name));
    if (!selected_session) {
        throw std::runtime_error("Session selection cancelled");
    }
    if (!selected_session->error.empty()) {
        throw std::runtime_error(selected_session->error);
    }

    cha::UvEventLoop event_loop;
    std::unique_ptr<cha::SessionController> controller;
    if (selected_session->id.empty()) {
        const auto session_label = selector.prompt_session_name();
        if (!session_label) {
            throw std::runtime_error("Session name prompt cancelled");
        }
        controller =
            std::move(workspace.create_session(
                *forum_name,
                *session_label,
                event_loop)
                .controller);
    } else {
        controller = workspace.open_session(
            *forum_name,
            selected_session->id,
            event_loop);
    }

    cha::run_persona(
        terminal,
        *controller,
        event_loop,
        std::move(selected_persona_id));
    cha::log_info("Terminal application stopped");
    return 0;
}
