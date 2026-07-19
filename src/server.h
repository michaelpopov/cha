#pragma once

#include "config.h"

#include <atomic>
#include <curl/curl.h>

#include <string>
#include <thread>
#include <vector>

namespace cha {

class Conversation;
class Pipe;

class Server {
public:
    Server(
        const Config& config,
        std::atomic_bool& cancellation,
        Conversation& conversation,
        std::string name = "Assistant");
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void run(Pipe& pipe_in, Pipe& pipe_out);
    // Waits for the worker, so callers must close its input pipe before closing the server.
    void close();

private:
    struct Message {
        std::string role;
        std::string content;
    };

    void dialog(Pipe& pipe_in, Pipe& pipe_out);
    [[nodiscard]] std::vector<Message> context() const;
    [[nodiscard]] bool handle_command(const std::string& input, Pipe& pipe_out);
    void complete(Pipe& pipe_out);
    [[nodiscard]] std::string base_url() const;
    [[nodiscard]] std::string endpoint() const;

    const Config& _config;
    std::atomic_bool& _cancellation;
    Conversation& _conversation;
    CURL* curl_{};
    std::string name_;
    std::string model_;
    std::string system_prompt_;
    std::thread thread_;
};

} // namespace cha
