#include "conversation.h"
#include "conversation_file.h"
#include "environment.h"
#include "pipe.h"
#include "server.h"
#include "startup_selector.h"
#include "user.h"
#include "workspace.h"

#include <atomic>
#include <exception>
#include <filesystem>
#include <iostream>

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
    std::string session_name;
    {
        cha::StartupSelector selector;
        const auto room_name = selector.select_room(workspace.rooms());
        if (!room_name) {
            return 0;
        }
        room = workspace.load_room(*room_name);
        const auto selected_session = selector.select_session(workspace.sessions(room));
        if (!selected_session) {
            return 0;
        }
        if (selected_session->id.empty()) {
            const auto session_label = selector.prompt_session_name();
            if (!session_label) {
                return 0;
            }
            session_name = workspace.create_session(room, *session_label).id;
        } else {
            session_name = selected_session->id;
        }
    }

    cha::Pipe pipe_user2server{};
    cha::Pipe pipe_server2user{};
    cha::Conversation conversation;
    const std::filesystem::path session_data = room.directory / "sessions" / (session_name + ".data");
    if (std::filesystem::exists(session_data)) {
        conversation.replace_messages(cha::load_conversation_file(session_data));
    }
    std::atomic_bool cancellation{false};

    cha::Server server(cancellation, conversation);
    server.init(room.config);

    server.run(pipe_user2server, pipe_server2user);
    cha::run_user(cancellation, conversation, pipe_server2user, pipe_user2server);
    server.stop();
    pipe_server2user.close();
    cha::save_conversation_file(session_data, conversation);

    return 0;
}
