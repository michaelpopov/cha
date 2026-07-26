#include "session/session_controller.h"
#include "session/workspace.h"
#include "ui/tui/startup_selector.h"
#include "ui/tui/terminal.h"
#include "ui/tui/user.h"
#include "util/environment.h"
#include "util/uv_event_loop.h"

#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

static int main_internal();

int main() {
    try {
        return main_internal();
    } catch (const std::exception& error) {
        std::cerr << "Failed: " << error.what() << '\n';
        return 1;
    }
}

int main_internal() {
    cha::load_dotenv();

    cha::Workspace workspace;
    cha::Terminal terminal;
    cha::StartupSelector selector(terminal);

    const auto forum_name = selector.select_forum(workspace.forums());
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

    cha::run_user(terminal, *controller, event_loop);
    return 0;
}
