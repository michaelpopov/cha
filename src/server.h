#pragma once

#include "config.h"

#include <atomic>
#include <curl/curl.h>

#include <string>
#include <thread>

namespace cha {

class Conversation;
class Pipe;

class Server {
public:
    Server(
        std::atomic_bool& cancellation,
        Conversation& conversation,
        std::string name = "Assistant");
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void init();
    void init(const Config& config);
    void run(Pipe& pipe_in, Pipe& pipe_out);
    // Unblocks the worker before joining, making the same shutdown path safe for destruction.
    void stop();

private:
    void initialize();
    void discover_model();
    void dialog(Pipe& pipe_in, Pipe& pipe_out);
    [[nodiscard]] bool handle_command(const std::string& input, Pipe& pipe_out);
    void complete(Pipe& pipe_out);
    [[nodiscard]] std::string base_url() const;
    [[nodiscard]] std::string endpoint() const;
    [[nodiscard]] std::string models_endpoint() const;

    Config _config;
    std::atomic_bool& _cancellation;
    Conversation& _conversation;
    CURL* curl_{};
    std::string name_;
    std::string system_prompt_;
    std::thread thread_;
    Pipe* input_{};
};

} // namespace cha
