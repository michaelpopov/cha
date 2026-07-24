#include "application/chat_coordinator.h"
#include "application/workspace_service.h"
#include "interfaces/terminal/startup_selector.h"
#include "interfaces/terminal/terminal.h"
#include "interfaces/terminal/user.h"
#include "util/environment.h"

#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>

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

    cha::WorkspaceService workspace;
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

    std::unique_ptr<cha::ChatCoordinator> coordinator;
    if (selected_session->id.empty()) {
        const auto session_label = selector.prompt_session_name();
        if (!session_label) {
            throw std::runtime_error("Session name prompt cancelled");
        }
        coordinator = workspace.create_session(*room_name, *session_label);
    } else {
        coordinator = workspace.open_session(*room_name, selected_session->id);
    }

    cha::run_user(terminal, *coordinator);
    return 0;
}
