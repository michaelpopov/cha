#pragma once

#include "config.h"

#include <atomic>
#include <curl/curl.h>

#include <string>
#include <thread>
#include <vector>

namespace cha {

class Pipe;

class Server {
public:
    Server(const Config& config, std::atomic_bool& cancellation);
    ~Server();

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void run(Pipe& pipe_in, Pipe& pipe_out);
    void close();
    void join();

private:
    struct Message {
        std::string role;
        std::string content;
    };

    void dialog(Pipe& pipe_in, Pipe& pipe_out);
    void reset_history();
    [[nodiscard]] bool handle_command(const std::string& input, Pipe& pipe_out);
    [[nodiscard]] std::string complete(Pipe& pipe_out);
    [[nodiscard]] std::string base_url() const;
    [[nodiscard]] std::string endpoint() const;

    const Config& _config;
    std::atomic_bool& _cancellation;
    CURL* curl_{};
    std::string model_;
    std::string system_prompt_;
    std::vector<Message> messages_;
    std::thread thread_;
};

} // namespace cha
