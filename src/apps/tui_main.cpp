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
    std::unique_ptr<cha::ChatCoordinator> coordinator;
    cha::Terminal terminal;
    {
        cha::StartupSelector selector(terminal);
        const auto room_name = selector.select_room(workspace.rooms());
        if (!room_name) {
            return 0;
        }
        cha::PreparedRoom room = workspace.prepare_room(*room_name);
        std::string selection_error;
        while (true) {
            const auto selected_session = selector.select_session(
                room.sessions(),
                selection_error);
            if (!selected_session) {
                return 0;
            }
            if (!selected_session->error.empty()) {
                selection_error = selected_session->error;
                continue;
            }
            if (selected_session->id.empty()) {
                const auto session_label = selector.prompt_session_name();
                if (!session_label) {
                    return 0;
                }
                try {
                    coordinator = room.create_session(*session_label);
                    break;
                } catch (const std::exception& error) {
                    selection_error = error.what();
                    continue;
                }
            }
            try {
                coordinator = room.open_session(selected_session->id);
                break;
            } catch (const std::exception& error) {
                selection_error = error.what();
            }
        }
    }

    cha::run_user(terminal, *coordinator);

    return 0;
}
