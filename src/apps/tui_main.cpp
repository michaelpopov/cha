#include "session/session_controller.h"
#include "session/workspace.h"
#include "ui/tui/startup_selector.h"
#include "ui/tui/terminal.h"
#include "ui/tui/user.h"
#include "util/environment.h"

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

    const auto room_name = selector.select_room(workspace.rooms());
    if (!room_name) {
        throw std::runtime_error("Room selection cancelled");
    }

    const auto selected_session = selector.select_session(
        workspace.sessions(*room_name));
    if (!selected_session) {
        throw std::runtime_error("Session selection cancelled");
    }
    if (!selected_session->error.empty()) {
        throw std::runtime_error(selected_session->error);
    }

    std::unique_ptr<cha::SessionController> controller;
    if (selected_session->id.empty()) {
        const auto session_label = selector.prompt_session_name();
        if (!session_label) {
            throw std::runtime_error("Session name prompt cancelled");
        }
        controller =
            std::move(workspace.create_session(*room_name, *session_label)
                .controller);
    } else {
        controller = workspace.open_session(*room_name, selected_session->id);
    }

    cha::run_user(terminal, *controller);
    return 0;
}
