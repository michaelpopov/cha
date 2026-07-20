#include "conversation.h"
#include "environment.h"
#include "pipe.h"
#include "server.h"
#include "user.h"

#include <atomic>
#include <exception>
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

    cha::Pipe pipe_user2server{};
    cha::Pipe pipe_server2user{};
    cha::Conversation conversation;
    std::atomic_bool cancellation{false};

    cha::Server server(cancellation, conversation);
    server.init();

    server.run(pipe_user2server, pipe_server2user);
    cha::run_user(cancellation, conversation, pipe_server2user, pipe_user2server);
    server.stop();
    pipe_server2user.close();

    return 0;
}
