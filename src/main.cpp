#include "agent_definition.h"
#include "chat_coordinator.h"
#include "session_database.h"
#include "environment.h"
#include "session_repository.h"
#include "startup_selector.h"
#include "terminal.h"
#include "user.h"
#include "workspace.h"

#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <vector>

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
    cha::Room room;
    std::optional<cha::SessionRepository> session_repository;
    std::string session_name;
    std::filesystem::path session_database;
    std::optional<cha::ConversationRestore> restored;
    std::vector<cha::AgentDefinition> definitions;
    cha::Terminal terminal;
    {
        cha::StartupSelector selector(terminal);
        const auto room_name = selector.select_room(workspace.rooms());
        if (!room_name) {
            return 0;
        }
        room = workspace.load_room(*room_name);
        std::vector<std::filesystem::path> persona_directories;
        for (const std::string& persona : room.persona_names) {
            persona_directories.push_back(workspace.persona_directory(persona));
        }
        definitions = cha::load_agent_definitions(persona_directories, room.directory);
        session_repository.emplace(room.directory / "sessions", room.name);
        std::string selection_error;
        while (true) {
            const auto selected_session = selector.select_session(session_repository->list(), selection_error);
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
                    session_name = session_repository->create(*session_label).id;
                    session_database = session_repository->database_path(session_name);
                    break;
                } catch (const std::exception& error) {
                    selection_error = error.what();
                    continue;
                }
            }

            session_name = selected_session->id;
            try {
                session_database = session_repository->open_database_path(session_name);
                restored = cha::load_conversation_state(session_database);
                break;
            } catch (const std::exception& error) {
                selection_error = error.what();
            }
        }
    }

    cha::ChatCoordinator coordinator(
        std::move(definitions),
        session_database,
        restored ? std::move(*restored) : cha::ConversationRestore{});
    cha::run_user(terminal, coordinator);

    return 0;
}
