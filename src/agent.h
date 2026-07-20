#pragma once

#include "agent_info.h"
#include "agent_protocol.h"
#include "config.h"

#include <atomic>
#include <curl/curl.h>

#include <filesystem>
#include <string>
#include <thread>

namespace cha {

// Runs one configured LLM connection and translates immutable requests into identified result events.
class Agent {
public:
    Agent(
        std::atomic_bool& cancellation,
        std::string name = {});
    ~Agent();

    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;

    void init(const Config& config);
    // Loads this agent's persona settings and combines its prompt with room-specific instructions.
    void init(const std::filesystem::path& persona_directory, const std::filesystem::path& room_directory);
    void run(CompletionRequestChannel& requests, AgentEventChannel& events);
    [[nodiscard]] AgentInfo info() const;
    // Unblocks the worker before joining, making the same shutdown path safe for destruction.
    void stop();

private:
    void initialize();
    void discover_model();
    void dialog(CompletionRequestChannel& requests, AgentEventChannel& events);
    [[nodiscard]] bool complete(const CompletionRequest& request, AgentEventChannel& events);
    [[nodiscard]] std::string base_url() const;
    [[nodiscard]] std::string endpoint() const;
    [[nodiscard]] std::string models_endpoint() const;

    Config _config;
    std::string api_key_;
    std::atomic_bool& _cancellation;
    CURL* curl_{};
    std::string name_;
    std::string system_prompt_;
    std::thread thread_;
    CompletionRequestChannel* input_{};
};

} // namespace cha
