#pragma once

#include "agent_definition.h"
#include "agent_info.h"
#include "agent_protocol.h"

#include <atomic>
#include <curl/curl.h>

#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace cha {

// Runs one configured LLM connection and translates immutable requests into identified result events.
class Agent {
public:
    Agent(
        AgentDefinition definition,
        std::atomic_bool& cancellation);
    ~Agent();

    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;

    // Starts this agent's worker exactly once.
    void start(CompletionRequestChannel& requests, AgentEventChannel& events);
    [[nodiscard]] AgentInfo info() const;
    // Unblocks and joins the worker; repeated calls are harmless.
    void stop();

private:
    // Tracks the enforced one-shot worker lifecycle.
    enum class State {
        ready,
        running,
        stopped,
    };

    // Classifies the terminal result of one network completion before it becomes an AgentEvent.
    enum class CompletionOutcome {
        completed,
        cancelled,
        protocol_error,
        transport_error,
    };

    // Carries the classified completion result and an explanatory failure message when applicable.
    struct CompletionResult {
        CompletionOutcome outcome{CompletionOutcome::completed};
        std::string message;
    };

    void discover_model();
    void dialog(CompletionRequestChannel& requests, AgentEventChannel& events);
    [[nodiscard]] CompletionResult complete(const CompletionRequest& request, AgentEventChannel& events);
    [[nodiscard]] std::string base_url() const;
    [[nodiscard]] std::string endpoint() const;
    [[nodiscard]] std::string models_endpoint() const;

    Config config_;
    std::string api_key_;
    std::atomic_bool& cancellation_;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl_{nullptr, &curl_easy_cleanup};
    std::string system_prompt_;
    std::thread thread_;
    CompletionRequestChannel* input_{};
    std::mutex lifecycle_mutex_;
    State state_{State::ready};
};

} // namespace cha
