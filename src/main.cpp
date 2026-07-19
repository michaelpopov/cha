#include "config.h"
#include "conversation.h"
#include "pipe.h"
#include "server.h"
#include "user.h"

#include <atomic>
#include <exception>
#include <iostream>

int main() {
    cha::Config config;

    try {
        config = cha::Config::load();
    } catch (const std::exception& error) {
        std::cerr << "Failed to load config.toml: " << error.what() << '\n';
        return 1;
    }

    cha::Pipe pipe_user2server{};
    cha::Pipe pipe_server2user{};
    cha::Conversation conversation;
    std::atomic_bool cancellation{false};

    try {
        cha::Server server(config, cancellation, conversation);

        server.run(pipe_user2server, pipe_server2user);
        cha::run_user(config.model, cancellation, conversation, pipe_server2user, pipe_user2server);
        server.close();
        pipe_server2user.close();
    } catch (const std::exception& error) {
        std::cerr << "Failed to start cha: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
